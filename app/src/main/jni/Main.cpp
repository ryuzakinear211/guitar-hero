#include <list>
#include <vector>
#include <string.h>
#include <pthread.h>
#include <thread>
#include <cstring>
#include <jni.h>
#include <unistd.h>
#include <fstream>
#include <iostream>
#include <dlfcn.h>
#include <atomic>
#include <cstdarg>
#include <sys/stat.h>
#include <sys/types.h>
#include <ctime>
#include "Includes/Logger.h"
#include "Includes/obfuscate.h"
#include "Includes/Utils.h"
#include "KittyMemory/MemoryPatch.h"
#include "Menu/Setup.h"

// Target library for Unity il2cpp games
#define targetLibName OBFUSCATE("libil2cpp.so")

#include "Includes/Macros.h"

// =========================================================================
// Lightweight Debug Logging System
// Writes log entries to /sdcard/0/Documents/guitar_hero_debug.log and
// /storage/emulated/0/Documents/guitar_hero_debug.log
// =========================================================================

static pthread_mutex_t g_logMutex = PTHREAD_MUTEX_INITIALIZER;
static const char *CANDIDATE_LOG_DIRS[] = {
    "/sdcard/0/Documents",
    "/storage/emulated/0/Documents",
    "/sdcard/Documents"
};

static void createDirectoryRecursive(const char *path) {
    char temp[512];
    char *p = nullptr;
    snprintf(temp, sizeof(temp), "%s", path);
    size_t len = strlen(temp);
    if (len == 0) return;
    if (temp[len - 1] == '/') {
        temp[len - 1] = 0;
    }
    for (p = temp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            mkdir(temp, 0777);
            *p = '/';
        }
    }
    mkdir(temp, 0777);
}

void LogDebug(const char *fmt, ...) {
    char message[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);

    // Also output to Android logcat
    LOGI("[GuitarHeroDebug] %s", message);

    time_t now = time(nullptr);
    struct tm tm_info;
    localtime_r(&now, &tm_info);
    char timeBuf[64];
    strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M:%S", &tm_info);

    pthread_mutex_lock(&g_logMutex);
    for (const char *dir : CANDIDATE_LOG_DIRS) {
        createDirectoryRecursive(dir);
        char filePath[512];
        snprintf(filePath, sizeof(filePath), "%s/guitar_hero_debug.log", dir);
        FILE *fp = fopen(filePath, "a");
        if (fp != nullptr) {
            fprintf(fp, "[%s] %s\n", timeBuf, message);
            fflush(fp);
            fclose(fp);
        }
    }
    pthread_mutex_unlock(&g_logMutex);
}

static std::atomic<int> g_hitThrottler{0};
static std::atomic<int> g_missThrottler{0};

static void LogHitThrottled(int fret, int green, int red, int yellow, int blue, int orange, int total) {
    int c = ++g_hitThrottler;
    if (c <= 10 || (c % 25) == 0) {
        LogDebug("NOTE HIT #%d: fret=%d (G:%d R:%d Y:%d B:%d O:%d) | Total Masuk:%d",
                 c, fret, green, red, yellow, blue, orange, total);
    }
}

static void LogMissThrottled(int fret, int totalMiss, int perdidas, int erradas) {
    int c = ++g_missThrottler;
    if (c <= 10 || (c % 15) == 0) {
        LogDebug("NOTE MISS #%d: fret=%d | Total Miss:%d (Perdidas:%d, Erradas:%d)",
                 c, fret, totalMiss, perdidas, erradas);
    }
}

// =========================================================================
// Real-Time Note Counter & Auto Play State
// =========================================================================
static bool featureFloatingHUD = false;     // Default OFF
static bool featureIncludeErradas = false;  // Default OFF

// Auto Play Settings (Single Toggle, OFF by default)
static std::atomic<bool> g_autoPlay{false};       // Master switch for Auto Play

// Per-fret internal trackers (frets 0..4)
static std::atomic<int> g_fretVisualTimer[5]{0, 0, 0, 0, 0};
static std::atomic<bool> g_fretSustaining[5]{false, false, false, false, false};
static std::atomic<int32_t> g_autoHitsCount[5]{0, 0, 0, 0, 0};
static std::atomic<int32_t> g_totalAutoHits{0};

// Pointers to active game instances
static void *mainInstance = nullptr;        // Pointer to Main (MonoBehaviour)
static void *mainNotesInstance = nullptr;   // Pointer to Main_Notes
static void *mainScorerInstance = nullptr;  // Pointer to Main_Scorer
static void *mainFretsInstance = nullptr;   // Pointer to Main_Frets
static void *mainSpecialInstance = nullptr; // Pointer to Main_Special (MonoBehaviour)

// =========================================================================
// Auto Special Ability (Chart-Aware Smart Pathing) State
// =========================================================================
static std::atomic<bool> g_autoSpecial{false};               // Master toggle: Auto Special Ability
static std::atomic<bool> g_isSpecialActive{false};          // Live status: is Special active?
static std::atomic<int32_t> g_currentCombo{0};              // Current live streak / combo count
static std::atomic<int32_t> g_currentScore{0};              // Current total score
static std::atomic<int32_t> g_lastActivatedAtNote{-1};      // Hit count at last activation to prevent double fire
static std::atomic<int32_t> g_totalSpecialActivations{0};   // Total successful activations count
static std::atomic<int32_t> g_specialEnergyLevel{0};        // Current natural SP energy (0..4)

// =========================================================================
// Deterministic Smart Path & Milestone System
// =========================================================================
struct SpPathMilestone {
    int32_t targetHit;       // The exact hit count (or combo) where ability is triggered
    int32_t requiredEnergy;  // Required energy bars (2, 3, or 4)
};

// Competitive chart path for Dragonforce - Through the Fire and Flames (Hard mode)
// Path:  2    2    2    2    2    2    2     3     3     2     2     2     4
// Kombo: 128  294  456  589  825  969  1095  1366  1823  2018  2152  2259  2607
static const std::vector<SpPathMilestone> DRAGONFORCE_HARD_PATH = {
    {128, 2},
    {294, 2},
    {456, 2},
    {589, 2},
    {825, 2},
    {969, 2},
    {1095, 2},
    {1366, 3},
    {1823, 3},
    {2018, 2},
    {2152, 2},
    {2259, 2},
    {2607, 4}
};

static std::vector<SpPathMilestone> g_activePathMilestones;
static std::atomic<size_t> g_currentPathStep{0};
static std::atomic<bool> g_isDragonforceSong{false};
static std::string g_currentSongName = "";

// Cached Song Notes from Chart & Add hooks
struct CachedSongNote {
    int32_t num;
    float time;
    float duration;
    int32_t special; // 1 if Star Power note, 0 otherwise
};
static std::vector<CachedSongNote> g_cachedChartNotes;
static pthread_mutex_t g_chartNotesMutex = PTHREAD_MUTEX_INITIALIZER;
static std::atomic<bool> g_starPhraseInProgress{false};
static void *g_chartInstance = nullptr; // Pointer to ed (Chart Manager)

// Circular buffer for calculating real-time note density (notes per second)
static const int DENSITY_BUFFER_SIZE = 16;
static float g_recentHitTimes[DENSITY_BUFFER_SIZE] = {0};
static int g_recentHitIndex = 0;
static int g_recentHitCount = 0;
static pthread_mutex_t g_pathMutex = PTHREAD_MUTEX_INITIALIZER;

static void RecordHitTime(float time) {
    pthread_mutex_lock(&g_pathMutex);
    g_recentHitTimes[g_recentHitIndex % DENSITY_BUFFER_SIZE] = time;
    g_recentHitIndex++;
    if (g_recentHitCount < DENSITY_BUFFER_SIZE) {
        g_recentHitCount++;
    }
    pthread_mutex_unlock(&g_pathMutex);
}

static float CalculateNoteDensity(float currentSongTime) {
    pthread_mutex_lock(&g_pathMutex);
    int count = 0;
    for (int i = 0; i < g_recentHitCount; i++) {
        float t = g_recentHitTimes[i];
        if (currentSongTime - t >= 0.0f && currentSongTime - t <= 1.5f) {
            count++;
        }
    }
    pthread_mutex_unlock(&g_pathMutex);
    return (float)count / 1.5f;
}

// Color-specific note hit counters:
// Fret 0 = Green, 1 = Red, 2 = Yellow, 3 = Blue, 4 = Orange
static std::atomic<int32_t> g_greenHits{0};
static std::atomic<int32_t> g_redHits{0};
static std::atomic<int32_t> g_yellowHits{0};
static std::atomic<int32_t> g_blueHits{0};
static std::atomic<int32_t> g_orangeHits{0};
static std::atomic<int32_t> g_otherHits{0};

// Color-specific note miss counters
static std::atomic<int32_t> g_greenMisses{0};
static std::atomic<int32_t> g_redMisses{0};
static std::atomic<int32_t> g_yellowMisses{0};
static std::atomic<int32_t> g_blueMisses{0};
static std::atomic<int32_t> g_orangeMisses{0};

// Helper functions to get current note stats from Main_Notes
static inline int32_t getNotesHit() {
    if (mainNotesInstance != nullptr) {
        // acertadas: 0x64 (Int32) in Main_Notes
        return *(int32_t *)((uintptr_t)mainNotesInstance + 0x64);
    }
    // Fallback to sum of color hits
    return g_greenHits.load() + g_redHits.load() + g_yellowHits.load() +
           g_blueHits.load() + g_orangeHits.load() + g_otherHits.load();
}

static inline int32_t getNotesHitSP() {
    if (mainNotesInstance != nullptr) {
        // acertadasSP: 0x68 (Int32) in Main_Notes
        return *(int32_t *)((uintptr_t)mainNotesInstance + 0x68);
    }
    return 0;
}

static inline int32_t getNotesPerdidas() {
    if (mainNotesInstance != nullptr) {
        // perdidas: 0x70 (Int32) in Main_Notes (missed on fretboard)
        return *(int32_t *)((uintptr_t)mainNotesInstance + 0x70);
    }
    return 0;
}

static inline int32_t getNotesErradas() {
    if (mainNotesInstance != nullptr) {
        // erradas: 0x6c (Int32) in Main_Notes (wrong fret strum)
        return *(int32_t *)((uintptr_t)mainNotesInstance + 0x6c);
    }
    return 0;
}

static inline int32_t getNotesMiss() {
    int32_t miss = getNotesPerdidas();
    if (featureIncludeErradas) {
        miss += getNotesErradas();
    }
    return miss;
}

static inline int32_t getNotesTotalCreated() {
    if (mainNotesInstance != nullptr) {
        // created: 0x74 (Int32) in Main_Notes
        return *(int32_t *)((uintptr_t)mainNotesInstance + 0x74);
    }
    return 0;
}

// Forward declaration of visual fret & particle function pointers
void (*old_Main_Frets_byc)(void *instance, int32_t fret, bool pressed) = nullptr;
static void (*GameObject_SetActive)(void *gameObject, bool active) = nullptr;
static void *(*Main_Frets_byh)(void *instance, int32_t fret) = nullptr; // fire
static void *(*Main_Frets_byi)(void *instance, int32_t fret) = nullptr; // spark
static void (*ParticleSystem_Play)(void *ps, void *args) = nullptr;
static void (*ParticleSystem_Stop)(void *ps, void *args) = nullptr;

// Helper to explicitly ensure the inner circular button GameObject is activated for finger touch
static void SetFretButtonActive(int32_t fret, bool active) {
    if (fret < 0 || fret >= 5 || mainFretsInstance == nullptr || GameObject_SetActive == nullptr) {
        return;
    }
    void *fretsArr = *(void **)((uintptr_t)mainFretsInstance + 0x90);
    if (fretsArr == nullptr) return;
    void **items = (void **)((uintptr_t)fretsArr + 0x20);
    if (items == nullptr) return;
    void *fretObj = items[fret];
    if (fretObj == nullptr) return;
    void *button = *(void **)((uintptr_t)fretObj + 0x18);
    if (button != nullptr) {
        GameObject_SetActive(button, active);
    }
}

static void resetAllNoteCounters() {
    g_greenHits = 0;
    g_redHits = 0;
    g_yellowHits = 0;
    g_blueHits = 0;
    g_orangeHits = 0;
    g_otherHits = 0;

    g_greenMisses = 0;
    g_redMisses = 0;
    g_yellowMisses = 0;
    g_blueMisses = 0;
    g_orangeMisses = 0;

    for (int i = 0; i < 5; i++) {
        g_fretVisualTimer[i] = 0;
        g_fretSustaining[i] = false;
        g_autoHitsCount[i] = 0;
        SetFretButtonActive(i, false);
        if (old_Main_Frets_byc != nullptr && mainFretsInstance != nullptr) {
            old_Main_Frets_byc(mainFretsInstance, i, false);
        }
        if (Main_Frets_byh != nullptr && ParticleSystem_Stop != nullptr && mainFretsInstance != nullptr) {
            void *firePs = Main_Frets_byh(mainFretsInstance, i);
            if (firePs != nullptr) {
                ParticleSystem_Stop(firePs, nullptr);
            }
        }
    }
    g_totalAutoHits = 0;

    g_lastActivatedAtNote = -1;
    g_totalSpecialActivations = 0;
    g_currentCombo = 0;
    g_currentScore = 0;
    g_isSpecialActive = false;
    g_starPhraseInProgress = false;

    pthread_mutex_lock(&g_chartNotesMutex);
    g_cachedChartNotes.clear();
    pthread_mutex_unlock(&g_chartNotesMutex);

    pthread_mutex_lock(&g_pathMutex);
    g_activePathMilestones.clear();
    g_currentPathStep = 0;
    g_recentHitCount = 0;
    g_recentHitIndex = 0;
    pthread_mutex_unlock(&g_pathMutex);

    if (mainNotesInstance != nullptr) {
        *(int32_t *)((uintptr_t)mainNotesInstance + 0x64) = 0; // acertadas
        *(int32_t *)((uintptr_t)mainNotesInstance + 0x68) = 0; // acertadasSP
        *(int32_t *)((uintptr_t)mainNotesInstance + 0x6c) = 0; // erradas
        *(int32_t *)((uintptr_t)mainNotesInstance + 0x70) = 0; // perdidas
    }
    LogDebug("User reset all note counters, auto-hit and special path stats to 0");
}

// =========================================================================
// Function Pointers & Hook Implementations
// =========================================================================

struct Vector3 {
    float x;
    float y;
    float z;
};

static void *(*Component_get_transform)(void *component) = nullptr;
static Vector3 (*Transform_get_position)(void *transform) = nullptr;
// Function pointers for Special Ability & Obscured types
static void (*Main_Special_bzk)(void *instance, bool a) = nullptr;
static void (*old_Main_Special_bzk)(void *instance, bool a) = nullptr;
static void (*Main_Special_bzl)(void *instance) = nullptr;
static void (*old_Main_Special_bzl)(void *instance) = nullptr;
static int32_t (*lx_op_Implicit_to_int)(void *obscuredInt) = nullptr;
static void *(*lx_op_Implicit_from_int)(int32_t value) = nullptr;

// Function pointers for Chart & Note inspection
static int32_t (*ed_Count)(void *instance) = nullptr;
static void *(*ed_bad)(void *instance, int32_t a) = nullptr;

// Helper to convert an Il2Cpp System.String to std::string
static std::string Il2CppStringToStdString(void *il2cppString) {
    if (il2cppString == nullptr) return "";
    int32_t len = *(int32_t *)((uintptr_t)il2cppString + 0x10);
    if (len <= 0 || len > 512) return "";
    const uint16_t *chars = (const uint16_t *)((uintptr_t)il2cppString + 0x14);
    std::string result;
    result.reserve(len);
    for (int i = 0; i < len; i++) {
        uint16_t ch = chars[i];
        if (ch < 128) {
            result.push_back((char)ch);
        } else {
            result.push_back('?');
        }
    }
    return result;
}

// Hook ed::bab (Chart loader & Song detector)
static void (*old_ed_bab)(void *instance, int32_t a, void *b, int32_t c, void *d, void *e) = nullptr;
static void hook_ed_bab(void *instance, int32_t a, void *b, int32_t c, void *d, void *e) {
    if (instance != nullptr) {
        g_chartInstance = instance;
        LogDebug("ed::bab hooked: Captured g_chartInstance=0x%lx", (unsigned long)instance);
    }
    if (b != nullptr) {
        std::string songStr = Il2CppStringToStdString(b);
        g_currentSongName = songStr;
        std::string lowerStr = songStr;
        for (char &ch : lowerStr) ch = ::tolower(ch);
        LogDebug("ed::bab song identifier: '%s'", songStr.c_str());
        if (lowerStr.find("dragonforce") != std::string::npos ||
            lowerStr.find("through the fire") != std::string::npos ||
            lowerStr.find("fire and flames") != std::string::npos ||
            lowerStr.find("ttfaf") != std::string::npos) {
            g_isDragonforceSong = true;
            LogDebug("Dragonforce TTFAF identified from song identifier!");
        } else {
            g_isDragonforceSong = false;
        }
    }
    if (old_ed_bab != nullptr) {
        old_ed_bab(instance, a, b, c, d, e);
    }
}

// Hook Main_Notes::Add (Real-time Note Recorder)
static void (*old_Main_Notes_Add)(void *instance, int32_t num, void *noteChart) = nullptr;
static void hook_Main_Notes_Add(void *instance, int32_t num, void *noteChart) {
    if (instance != nullptr) {
        mainNotesInstance = instance;
    }
    if (noteChart != nullptr) {
        float nTime = *(float *)((uintptr_t)noteChart + 0x14);
        float nDur = *(float *)((uintptr_t)noteChart + 0x18);
        int32_t nSp = *(int32_t *)((uintptr_t)noteChart + 0x1c);
        pthread_mutex_lock(&g_chartNotesMutex);
        if (num == 0) {
            g_cachedChartNotes.clear();
        }
        g_cachedChartNotes.push_back({num, nTime, nDur, nSp});
        pthread_mutex_unlock(&g_chartNotesMutex);
    }
    if (old_Main_Notes_Add != nullptr) {
        old_Main_Notes_Add(instance, num, noteChart);
    }
}

// Build deterministic, non-random Star Power path milestones for any chart
static void BuildSmartPathForChart() {
    pthread_mutex_lock(&g_chartNotesMutex);
    std::vector<CachedSongNote> notes = g_cachedChartNotes;
    pthread_mutex_unlock(&g_chartNotesMutex);

    pthread_mutex_lock(&g_pathMutex);
    g_activePathMilestones.clear();
    g_currentPathStep = 0;

    // Check if current song is Dragonforce TTFAF (either via name or characteristic note count > 2500)
    bool isDragonforce = g_isDragonforceSong.load();
    if (!isDragonforce && notes.size() >= 2500) {
        isDragonforce = true;
        g_isDragonforceSong = true;
    }

    if (isDragonforce) {
        g_activePathMilestones = DRAGONFORCE_HARD_PATH;
        pthread_mutex_unlock(&g_pathMutex);
        LogDebug("⭐ [SmartPath] Dragonforce TTFAF active! Loaded %zu competitive chart milestones:",
                 g_activePathMilestones.size());
        for (size_t i = 0; i < g_activePathMilestones.size(); i++) {
            LogDebug("  [Dragonforce Step %zu] Trigger at Hit/Combo: %d | Req Energy: %d bars",
                     i + 1, g_activePathMilestones[i].targetHit, g_activePathMilestones[i].requiredEnergy);
        }
        return;
    }

    if (notes.empty()) {
        pthread_mutex_unlock(&g_pathMutex);
        LogDebug("[SmartPath] Notes cache empty, awaiting chart load...");
        return;
    }

    // Group Star Power notes into contiguous phrases
    struct SpPhrase {
        int32_t startNote;
        int32_t endNote;
        float startTime;
        float endTime;
    };
    std::vector<SpPhrase> phrases;

    bool inPhrase = false;
    SpPhrase curPhrase = {0, 0, 0.0f, 0.0f};

    for (size_t i = 0; i < notes.size(); i++) {
        if (notes[i].special == 1) {
            if (!inPhrase) {
                inPhrase = true;
                curPhrase.startNote = notes[i].num;
                curPhrase.startTime = notes[i].time;
            }
            curPhrase.endNote = notes[i].num;
            curPhrase.endTime = notes[i].time + notes[i].duration;
        } else {
            if (inPhrase) {
                inPhrase = false;
                phrases.push_back(curPhrase);
            }
        }
    }
    if (inPhrase) {
        phrases.push_back(curPhrase);
    }

    LogDebug("[SmartPath] Chart analyzed: %zu total notes, %zu Star Power phrases detected",
             notes.size(), phrases.size());

    if (phrases.empty()) {
        // Fallback for custom charts without SP phrases: milestones spaced across song
        for (int32_t h = 120; h < (int32_t)notes.size() - 50; h += 200) {
            g_activePathMilestones.push_back({h, 2});
        }
        pthread_mutex_unlock(&g_pathMutex);
        return;
    }

    // Deterministic simulation: schedule activations at exact phrase ends where window is clear
    int32_t simEnergy = 0;
    float simActivationEndTime = -1.0f;

    for (size_t p = 0; p < phrases.size(); p++) {
        const auto &ph = phrases[p];
        if (ph.startTime < simActivationEndTime) {
            // Overlapped by previous active Special, phrase not banked
            continue;
        }

        if (simEnergy < 4) {
            simEnergy++;
        }

        if (simEnergy >= 2) {
            bool hasNextPhrase = (p + 1 < phrases.size());
            float nextPhraseStart = hasNextPhrase ? phrases[p + 1].startTime : 999999.0f;
            float duration = (float)simEnergy * 3.75f;
            float proposedEndTime = ph.endTime + duration + 0.6f;

            bool shouldSchedule = false;
            if (simEnergy >= 4) {
                // Bar full, must activate to prevent waste
                shouldSchedule = true;
            } else if (!hasNextPhrase) {
                // Outro, no more phrases
                shouldSchedule = true;
            } else if (proposedEndTime <= nextPhraseStart) {
                // Clean window without clipping upcoming SP phrase!
                shouldSchedule = true;
            }

            if (shouldSchedule) {
                int32_t targetHit = ph.endNote;
                if (targetHit < 30) targetHit = 30; // 4x multiplier threshold
                g_activePathMilestones.push_back({targetHit, simEnergy});
                simActivationEndTime = ph.endTime + duration;
                simEnergy = 0;
            }
        }
    }

    // Outro check: if energy remained at end of song
    if (simEnergy >= 2 && !notes.empty()) {
        int32_t outroHit = notes.back().num - 30;
        if (outroHit > 30) {
            g_activePathMilestones.push_back({outroHit, simEnergy});
        }
    }

    pthread_mutex_unlock(&g_pathMutex);

    LogDebug("⭐ [SmartPath] Generated %zu deterministic milestones for '%s':",
             g_activePathMilestones.size(), g_currentSongName.c_str());
    for (size_t i = 0; i < g_activePathMilestones.size(); i++) {
        LogDebug("  [Step %zu] Trigger at Hit/Combo: %d | Req Energy: %d bars",
                 i + 1, g_activePathMilestones[i].targetHit, g_activePathMilestones[i].requiredEnergy);
    }
}

// Deterministic Smart Path Trigger:
// Activates Special Ability PRECISELY when the song hit count / combo reaches the charted milestone.
// Zero randomness. Completely aligned with hit milestones and natural energy!
static void CheckAndTriggerSmartSpecial(float songTime) {
    if (!g_autoSpecial.load()) {
        return;
    }

    if (mainSpecialInstance == nullptr) {
        if (mainInstance != nullptr) {
            mainSpecialInstance = *(void **)((uintptr_t)mainInstance + 0xc0);
        }
        if (mainSpecialInstance == nullptr && mainNotesInstance != nullptr) {
            mainSpecialInstance = *(void **)((uintptr_t)mainNotesInstance + 0x88);
        }
    }
    if (mainSpecialInstance == nullptr) {
        return;
    }

    // Check if Special is already active
    bool active = *(bool *)((uintptr_t)mainSpecialInstance + 0x21);
    g_isSpecialActive = active;
    if (active) {
        return; // Already enjoying 8x multiplier!
    }

    // Read natural energy level from epe (0x40)
    void *epe = *(void **)((uintptr_t)mainSpecialInstance + 0x40);
    int32_t currentEnergy = 0;
    if (epe != nullptr && lx_op_Implicit_to_int != nullptr) {
        currentEnergy = lx_op_Implicit_to_int(epe);
    }
    g_specialEnergyLevel = currentEnergy;

    // Minimum energy required by game engine to activate is 2 bars (50%)
    if (currentEnergy < 2) {
        return;
    }

    int32_t combo = g_currentCombo.load();
    int32_t hits = getNotesHit();

    // Prevent rapid re-triggering at the exact same hit count
    if (g_lastActivatedAtNote.load() >= 0 && (hits - g_lastActivatedAtNote.load()) < 10) {
        return;
    }

    // Multiplier safety: ensure combo >= 30 for 4x base multiplier -> 8x
    if (combo < 30) {
        return;
    }

    // Hold if Star Power phrase is actively in progress
    if (currentEnergy < 4 && g_starPhraseInProgress.load()) {
        return;
    }

    pthread_mutex_lock(&g_pathMutex);
    if (g_activePathMilestones.empty()) {
        pthread_mutex_unlock(&g_pathMutex);
        BuildSmartPathForChart();
        pthread_mutex_lock(&g_pathMutex);
    }

    size_t stepIdx = g_currentPathStep.load();
    if (stepIdx >= g_activePathMilestones.size()) {
        pthread_mutex_unlock(&g_pathMutex);
        return; // All scheduled milestones completed
    }

    const SpPathMilestone step = g_activePathMilestones[stepIdx];
    pthread_mutex_unlock(&g_pathMutex);

    // EXACT HIT / COMBO MILESTONE CHECK:
    // Ability must trigger WHEN hit count or combo reaches the scheduled milestone!
    // Never trigger at random hit counts!
    if (hits < step.targetHit && combo < step.targetHit) {
        return; // Milestone hit count not reached yet
    }

    // Energy requirement check: must have at least the required bars
    if (currentEnergy < step.requiredEnergy) {
        return; // Natural energy not filled to required bars yet
    }

    // TRIGGER SPECIAL ABILITY PRECISELY AT THIS MILESTONE!
    *(float *)((uintptr_t)mainSpecialInstance + 0x24) = 0.0f; // timeBlocked = 0
    *(bool *)((uintptr_t)mainSpecialInstance + 0x20) = true;  // spShow = true

    if (old_Main_Special_bzk != nullptr) {
        old_Main_Special_bzk(mainSpecialInstance, false);
    } else if (Main_Special_bzk != nullptr) {
        Main_Special_bzk(mainSpecialInstance, false);
    }

    g_isSpecialActive = true;
    g_lastActivatedAtNote = hits;
    g_totalSpecialActivations++;
    g_currentPathStep++; // Advance to next scheduled milestone!

    LogDebug("⭐ [SmartSpecial] Path Step #%zu TRIGGERED at Hit #%d (Target: %d) | Combo: %d | Natural Energy: %d/%d bars | Total Activations: %d",
             stepIdx + 1, hits, step.targetHit, combo, currentEnergy, step.requiredEnergy, g_totalSpecialActivations.load());
}

// 1. Hook Main_Scorer::cdc (RVA: 0x20c38a0)
void (*old_Main_Scorer_cdc)(void *instance, int32_t a, int32_t b) = nullptr;
void hook_Main_Scorer_cdc(void *instance, int32_t a, int32_t b) {
    if (instance != nullptr) {
        mainScorerInstance = instance;
    }
    g_currentCombo = a;
    g_currentScore = b;
    if (old_Main_Scorer_cdc != nullptr) {
        return old_Main_Scorer_cdc(instance, a, b);
    }
}

// Hook Main_Special::bzk (RVA: 0x20bae98) - Special Ability Activation
void hook_Main_Special_bzk(void *instance, bool a) {
    if (instance != nullptr) {
        mainSpecialInstance = instance;
    }
    if (old_Main_Special_bzk != nullptr) {
        old_Main_Special_bzk(instance, a);
    }
    if (instance != nullptr) {
        g_isSpecialActive = *(bool *)((uintptr_t)instance + 0x21);
    } else {
        g_isSpecialActive = true;
    }
    LogDebug("[SpecialHook] bzk invoked: isSpecialActive=%d", g_isSpecialActive.load() ? 1 : 0);
}

// Hook Main_Special::bzl (RVA: 0x20bb068) - Special Ability Deactivation
void hook_Main_Special_bzl(void *instance) {
    if (instance != nullptr) {
        mainSpecialInstance = instance;
    }
    if (old_Main_Special_bzl != nullptr) {
        old_Main_Special_bzl(instance);
    }
    g_isSpecialActive = false;
    LogDebug("[SpecialHook] bzl invoked: Special ended, multiplier reverted to normal");
}

// 2. Hook Main_Notes::byy (RVA: 0x20b8fec)
// Called whenever a note is successfully hit
void (*old_Main_Notes_byy)(void *instance, void *hm, float b) = nullptr;
void hook_Main_Notes_byy(void *instance, void *hm, float b) {
    if (instance != nullptr) {
        mainNotesInstance = instance;
    }

    int32_t fret = -1;
    if (hm != nullptr) {
        fret = *(int32_t *)((uintptr_t)hm + 0x3c);
        int32_t num = *(int32_t *)((uintptr_t)hm + 0x38);
        switch (fret) {
            case 0: g_greenHits++; break;
            case 1: g_redHits++; break;
            case 2: g_yellowHits++; break;
            case 3: g_blueHits++; break;
            case 4: g_orangeHits++; break;
            default: g_otherHits++; break;
        }

        // Count auto hits centrally here for 100% synchronization with total hits
        if (g_autoPlay.load()) {
            g_totalAutoHits++;
            if (fret >= 0 && fret < 5) {
                g_autoHitsCount[fret]++;
            }
        }

        // Track sustain notes (rastro: offset 0x28)
        if (fret >= 0 && fret < 5) {
            void *rastro = *(void **)((uintptr_t)hm + 0x28);
            bool hasRastro = false;
            if (rastro != nullptr) {
                hasRastro = *(bool *)((uintptr_t)rastro + 0x20);
            }

            if (hasRastro) {
                g_fretSustaining[fret] = true;
                g_fretVisualTimer[fret] = 0; // Sustain manages duration until bzc
            } else {
                g_fretSustaining[fret] = false;
                g_fretVisualTimer[fret] = 8; // Clean natural tap animation (~130ms)
            }

            if (g_autoPlay.load()) {
                // Visual touch button animation (sentuhan fisik tombol lingkaran)
                SetFretButtonActive(fret, true);
                if (old_Main_Frets_byc != nullptr && mainFretsInstance != nullptr) {
                    old_Main_Frets_byc(mainFretsInstance, fret, true); // Visual button pressed
                }

                // Trigger accurate spark & fire hit effect (percikan api pada note dan lingkaran)
                if (mainFretsInstance != nullptr && ParticleSystem_Play != nullptr) {
                    if (Main_Frets_byh != nullptr) {
                        void *firePs = Main_Frets_byh(mainFretsInstance, fret);
                        if (firePs != nullptr) {
                            ParticleSystem_Play(firePs, nullptr);
                        }
                    }
                    if (Main_Frets_byi != nullptr) {
                        void *sparkPs = Main_Frets_byi(mainFretsInstance, fret);
                        if (sparkPs != nullptr) {
                            ParticleSystem_Play(sparkPs, nullptr);
                        }
                    }
                }
            }
        }

        // Track live Star Power phrase progress
        void *spObj = *(void **)((uintptr_t)hm + 0x30); // Special* at 0x30
        if (spObj != nullptr) {
            bool isStar = *(bool *)((uintptr_t)spObj + 0x10);
            int32_t seq = *(int32_t *)((uintptr_t)spObj + 0x14);
            int32_t seqSize = *(int32_t *)((uintptr_t)spObj + 0x18);
            if (isStar) {
                if (seqSize > 0 && seq < seqSize) {
                    g_starPhraseInProgress = true;
                } else if (seqSize > 0 && seq >= seqSize) {
                    g_starPhraseInProgress = false; // Phrase completed!
                }
            }
        }
    }

    RecordHitTime(b);
    CheckAndTriggerSmartSpecial(b);

    if (old_Main_Notes_byy != nullptr) {
        old_Main_Notes_byy(instance, hm, b);
    }

    LogHitThrottled(fret, g_greenHits.load(), g_redHits.load(), g_yellowHits.load(),
                    g_blueHits.load(), g_orangeHits.load(), getNotesHit());
}

// 3. Hook Main_Notes::byz (RVA: 0x20b9758)
// Called whenever a note is missed (fret error or passed by)
void (*old_Main_Notes_byz)(void *instance, void *hm) = nullptr;
void hook_Main_Notes_byz(void *instance, void *hm) {
    if (instance != nullptr) {
        mainNotesInstance = instance;
    }

    int32_t fret = -1;
    if (hm != nullptr) {
        fret = *(int32_t *)((uintptr_t)hm + 0x3c);
    }

    // 100% ACCURACY SAFETY NET FOR AUTO PLAY:
    // If Auto Play is enabled, intercept miss calls and convert unhit notes to 100% Hits!
    if (g_autoPlay.load()) {
        if (hm != nullptr) {
            int32_t status = *(int32_t *)((uintptr_t)hm + 0x40);
            if (status == 0) { // Note was about to be missed in Hard mode
                float songTime = 0.0f;
                if (mainInstance != nullptr) {
                    songTime = *(float *)((uintptr_t)mainInstance + 0x28);
                }
                hook_Main_Notes_byy(instance, hm, songTime);
                LogDebug("Auto Play 100%% Safety Hit: fret=%d, recovered unhit note at miss threshold", fret);
                return; // Suppress miss completely - streak and combo preserved!
            }
        } else {
            // Stray strum error (erradas), suppress in Auto Play mode
            return;
        }
    }

    if (hm != nullptr) {
        switch (fret) {
            case 0: g_greenMisses++; break;
            case 1: g_redMisses++; break;
            case 2: g_yellowMisses++; break;
            case 3: g_blueMisses++; break;
            case 4: g_orangeMisses++; break;
            default: break;
        }
        if (fret >= 0 && fret < 5) {
            g_fretSustaining[fret] = false;
            g_fretVisualTimer[fret] = 0;
            SetFretButtonActive(fret, false);
            if (old_Main_Frets_byc != nullptr && mainFretsInstance != nullptr) {
                old_Main_Frets_byc(mainFretsInstance, fret, false);
            }
            if (Main_Frets_byh != nullptr && ParticleSystem_Stop != nullptr && mainFretsInstance != nullptr) {
                void *firePs = Main_Frets_byh(mainFretsInstance, fret);
                if (firePs != nullptr) {
                    ParticleSystem_Stop(firePs, nullptr);
                }
            }
        }
    }

    if (old_Main_Notes_byz != nullptr) {
        old_Main_Notes_byz(instance, hm);
    }

    LogMissThrottled(fret, getNotesMiss(), getNotesPerdidas(), getNotesErradas());
}

// 4. Hook Main_Notes::byv (RVA: 0x20b8350)
// Called when song starts or restarts
void (*old_Main_Notes_byv)(void *instance) = nullptr;
void hook_Main_Notes_byv(void *instance) {
    if (instance != nullptr) {
        mainNotesInstance = instance;
    }

    pthread_mutex_lock(&g_chartNotesMutex);
    g_cachedChartNotes.clear();
    pthread_mutex_unlock(&g_chartNotesMutex);

    pthread_mutex_lock(&g_pathMutex);
    g_activePathMilestones.clear();
    g_currentPathStep = 0;
    pthread_mutex_unlock(&g_pathMutex);

    g_starPhraseInProgress = false;
    g_lastActivatedAtNote = -1;
    g_totalSpecialActivations = 0;

    g_greenHits = 0;
    g_redHits = 0;
    g_yellowHits = 0;
    g_blueHits = 0;
    g_orangeHits = 0;
    g_otherHits = 0;

    g_greenMisses = 0;
    g_redMisses = 0;
    g_yellowMisses = 0;
    g_blueMisses = 0;
    g_orangeMisses = 0;

    for (int i = 0; i < 5; i++) {
        g_fretVisualTimer[i] = 0;
        g_fretSustaining[i] = false;
        g_autoHitsCount[i] = 0;
        SetFretButtonActive(i, false);
        if (old_Main_Frets_byc != nullptr && mainFretsInstance != nullptr) {
            old_Main_Frets_byc(mainFretsInstance, i, false);
        }
        if (Main_Frets_byh != nullptr && ParticleSystem_Stop != nullptr && mainFretsInstance != nullptr) {
            void *firePs = Main_Frets_byh(mainFretsInstance, i);
            if (firePs != nullptr) {
                ParticleSystem_Stop(firePs, nullptr);
            }
        }
    }
    g_totalAutoHits = 0;

    if (old_Main_Notes_byv != nullptr) {
        old_Main_Notes_byv(instance);
    }

    LogDebug("SONG START / RESTART: all note counters & auto-hit states reset to 0 (instance=0x%lx)", (unsigned long)instance);
}

// 5. Hook Main_Notes::bzc (RVA: 0x20b9874)
// Called when a note's sustain/rastro trail ends
void (*old_Main_Notes_bzc)(void *instance, void *hm) = nullptr;
void hook_Main_Notes_bzc(void *instance, void *hm) {
    if (hm != nullptr) {
        int32_t fret = *(int32_t *)((uintptr_t)hm + 0x3c);
        if (fret >= 0 && fret < 5) {
            g_fretSustaining[fret] = false;
            g_fretVisualTimer[fret] = 0;
            SetFretButtonActive(fret, false);
            if (old_Main_Frets_byc != nullptr && mainFretsInstance != nullptr) {
                old_Main_Frets_byc(mainFretsInstance, fret, false); // Release visual fret
            }
            if (Main_Frets_byh != nullptr && ParticleSystem_Stop != nullptr && mainFretsInstance != nullptr) {
                void *firePs = Main_Frets_byh(mainFretsInstance, fret);
                if (firePs != nullptr) {
                    ParticleSystem_Stop(firePs, nullptr);
                }
            }
        }
    }
    if (old_Main_Notes_bzc != nullptr) {
        old_Main_Notes_bzc(instance, hm);
    }
}

// 6. Hook Main_Frets::byc (RVA: 0x1f9cec8)
// Animates and controls visual button press/release on screen
void hook_Main_Frets_byc(void *instance, int32_t fret, bool pressed) {
    if (instance != nullptr) {
        mainFretsInstance = instance;
    }

    // If Auto Play has an active touch timer or sustain hold on this fret,
    // prevent the game's idle touch polling (byb) from canceling the touch within 1 frame
    if (g_autoPlay.load() && fret >= 0 && fret < 5) {
        if (!pressed && (g_fretVisualTimer[fret] > 0 || g_fretSustaining[fret].load())) {
            return;
        }
    }

    if (old_Main_Frets_byc != nullptr) {
        old_Main_Frets_byc(instance, fret, pressed);
    }
}

// 7. Hook Main_Notes::bzd (RVA: 0x20b9d5c)
// Called every frame for each active note to update position
// Triggers Auto Play HIT precisely when note enters the color circle!
void (*old_Main_Notes_bzd)(void *instance, void *hm, float time) = nullptr;
void hook_Main_Notes_bzd(void *instance, void *hm, float time) {
    // A. Let game update note position first
    if (old_Main_Notes_bzd != nullptr) {
        old_Main_Notes_bzd(instance, hm, time);
    }

    if (instance != nullptr) {
        mainNotesInstance = instance;
    }

    // B. Check if Auto Play is enabled
    if (!g_autoPlay.load() || hm == nullptr) {
        return;
    }

    // Check if note is still active/unhit (NoteStatus.normal == 0)
    int32_t status = *(int32_t *)((uintptr_t)hm + 0x40);
    if (status != 0) {
        return;
    }

    int32_t fret = *(int32_t *)((uintptr_t)hm + 0x3c);
    if (fret < 0 || fret > 4) {
        return;
    }

    // C. Measure note position precisely
    if (Component_get_transform != nullptr && Transform_get_position != nullptr) {
        void *transform = Component_get_transform(hm);
        if (transform != nullptr) {
            Vector3 pos = Transform_get_position(transform);

            // In Guitar Flash: center of color circle is at Z ≈ -13.05f.
            // Hit note cleanly when it reaches the circle:
            if (pos.z <= -12.90f && pos.z >= -15.5f) {
                // Execute hit directly on note (hook_Main_Notes_byy handles press, particles & sustain)
                hook_Main_Notes_byy(instance, hm, time);
                LogDebug("Auto Play HIT inside circle: fret=%d, z=%.2f, time=%.2f", fret, pos.z, time);
            }
        }
    }
}

// 8. Hook Main_Frets::bye (RVA: 0x1f9d1dc)
// Called by Main::FixedUpdate to test if a fret is held down during sustain (rastro)
bool (*old_Main_Frets_bye)(void *instance, int32_t fret) = nullptr;
bool hook_Main_Frets_bye(void *instance, int32_t fret) {
    if (instance != nullptr) {
        mainFretsInstance = instance;
    }

    // A. Manual hold
    bool manual = false;
    if (old_Main_Frets_bye != nullptr) {
        manual = old_Main_Frets_bye(instance, fret);
    }
    if (manual) {
        return true;
    }

    // B. Auto Sustain hold
    if (g_autoPlay.load() && fret >= 0 && fret < 5) {
        if (g_fretSustaining[fret].load()) {
            return true;
        }
    }

    return false;
}

// 9. Hook Main::Update (RVA: 0x20c50b0)
// Frame update for the game's Main controller
static bool g_mainCapturedLogged = false;
void (*old_Main_Update)(void *instance) = nullptr;
void hook_Main_Update(void *instance) {
    if (old_Main_Update != nullptr) {
        old_Main_Update(instance);
    }

    if (instance != nullptr) {
        mainInstance = instance;

        // Main.frets: 0xc8
        void *frets = *(void **)((uintptr_t)instance + 0xc8);
        if (frets != nullptr) {
            mainFretsInstance = frets;
        }

        // Main.notes: 0xd0
        void *notes = *(void **)((uintptr_t)instance + 0xd0);
        if (notes != nullptr) {
            mainNotesInstance = notes;
        }

        // Main.special: 0xc0
        void *special = *(void **)((uintptr_t)instance + 0xc0);
        if (special != nullptr) {
            mainSpecialInstance = special;
        }

        // Main.scorer: 0x110
        void *scorer = *(void **)((uintptr_t)instance + 0x110);
        if (scorer != nullptr) {
            mainScorerInstance = scorer;
        }

        // Main.chart: 0x58
        void *chart = *(void **)((uintptr_t)instance + 0x58);
        if (chart != nullptr) {
            g_chartInstance = chart;
        }

        if (!g_mainCapturedLogged && notes != nullptr) {
            g_mainCapturedLogged = true;
            LogDebug("Captured active instances: Main=0x%lx, Frets=0x%lx, Notes=0x%lx, Special=0x%lx, Scorer=0x%lx, Chart=0x%lx",
                     (unsigned long)mainInstance, (unsigned long)mainFretsInstance,
                     (unsigned long)mainNotesInstance, (unsigned long)mainSpecialInstance,
                     (unsigned long)mainScorerInstance, (unsigned long)g_chartInstance);
        }
    }

    // Evaluate Auto Special Ability smart pathing
    float songTime = 0.0f;
    if (mainInstance != nullptr) {
        songTime = *(float *)((uintptr_t)mainInstance + 0x28);
    }
    CheckAndTriggerSmartSpecial(songTime);

    // Manage visual fret release timers for tap notes
    for (int i = 0; i < 5; i++) {
        if (g_fretVisualTimer[i] > 0) {
            int t = --g_fretVisualTimer[i];
            if (t == 0 && !g_fretSustaining[i].load()) {
                SetFretButtonActive(i, false);
                if (old_Main_Frets_byc != nullptr && mainFretsInstance != nullptr) {
                    old_Main_Frets_byc(mainFretsInstance, i, false); // Release visual fret
                }
            }
        }
    }
}

// 10. Hook Main_Sounds::cfe (RVA: 0x20e267c)
// Plays the screech / clack strum error sound effect
void (*old_Main_Sounds_cfe)(void *instance) = nullptr;
void hook_Main_Sounds_cfe(void *instance) {
    if (g_autoPlay.load()) {
        // Mute miss / strum error sound during Auto Play for a clean, legit audio experience!
        return;
    }
    if (old_Main_Sounds_cfe != nullptr) {
        old_Main_Sounds_cfe(instance);
    }
}

// =========================================================================
// Song Unlock Feature (Toggle: OFF by default)
// =========================================================================
static std::atomic<bool> g_unlockAllSongs{false};
static void *g_gcInstance = nullptr;

// 11. Hook gc::blx (RVA: 0x1eaa984) - Song getter by index
void *(*old_gc_blx)(void *instance, int32_t index) = nullptr;
int32_t (*gc_bma)(void *instance) = nullptr; // RVA: 0x1eaa9dc - Song count

static void UnlockAllSongsInMemory(void *gc) {
    if (!gc) return;
    int32_t count = 0;
    if (gc_bma != nullptr) {
        count = gc_bma(gc);
    } else {
        void *list = *(void **)((uintptr_t)gc + 0x20);
        if (list) count = *(int32_t *)((uintptr_t)list + 0x18);
    }
    LogDebug("[UnlockSongs] Unlocking %d songs in gc memory", count);
    for (int i = 0; i < count; i++) {
        void *song = old_gc_blx != nullptr ? old_gc_blx(gc, i) : nullptr;
        if (song != nullptr) {
            *(int32_t *)((uintptr_t)song + 0x28) = 0; // level = 0
            *(int32_t *)((uintptr_t)song + 0x2c) = 0; // preco = 0 (free)
            *(bool *)((uintptr_t)song + 0x75) = false; // vip = false
        }
    }
}

void *hook_gc_blx(void *instance, int32_t index) {
    if (instance != nullptr) {
        g_gcInstance = instance;
    }
    void *song = nullptr;
    if (old_gc_blx != nullptr) {
        song = old_gc_blx(instance, index);
    }
    if (g_unlockAllSongs.load() && song != nullptr) {
        *(int32_t *)((uintptr_t)song + 0x28) = 0; // level = 0
        *(int32_t *)((uintptr_t)song + 0x2c) = 0; // preco = 0 (free)
        *(bool *)((uintptr_t)song + 0x75) = false; // vip = false
    }
    return song;
}

// 12. Hook ma::get_Vip (RVA: 0x2116cfc) - Static VIP status getter
bool (*old_ma_get_Vip)() = nullptr;
bool hook_ma_get_Vip() {
    if (g_unlockAllSongs.load()) {
        return true; // Full VIP status granted
    }
    if (old_ma_get_Vip != nullptr) {
        return old_ma_get_Vip();
    }
    return false;
}

// 13. Hook MenuMus_Setlist::cnn (RVA: 0x20f2ecc) - Setlist song play launcher
void (*old_MenuMus_Setlist_cnn)(void *setlist, int32_t a, bool b) = nullptr;
void hook_MenuMus_Setlist_cnn(void *setlist, int32_t a, bool b) {
    if (g_unlockAllSongs.load()) {
        b = true; // Mark song as unlocked/owned to bypass purchase dialog
    }
    if (old_MenuMus_Setlist_cnn != nullptr) {
        old_MenuMus_Setlist_cnn(setlist, a, b);
    }
}

// 14. Hook MenuMus_Setlist::cno (RVA: 0x20f3148) - Buy/Unlock song dialog interceptor
void (*old_MenuMus_Setlist_cno)(void *setlist, int32_t a, bool b) = nullptr;
void hook_MenuMus_Setlist_cno(void *setlist, int32_t a, bool b) {
    if (g_unlockAllSongs.load()) {
        LogDebug("[UnlockSongs] Intercepted lock dialog for song index %d, launching song directly", a);
        if (old_MenuMus_Setlist_cnn != nullptr) {
            old_MenuMus_Setlist_cnn(setlist, a, true);
            return;
        }
    }
    if (old_MenuMus_Setlist_cno != nullptr) {
        old_MenuMus_Setlist_cno(setlist, a, b);
    }
}

// Native Thread for Library Detection & Hooking
void *hack_thread(void *) {
    LogDebug("=== Guitar Hero Mod Menu Native Thread Started ===");
    LOGI(OBFUSCATE("pthread created"));

    int waitSeconds = 0;
    do {
        sleep(1);
        waitSeconds++;
        if (waitSeconds % 5 == 0) {
            LogDebug("Waiting for %s to load... (%d seconds elapsed)", (const char *) targetLibName, waitSeconds);
        }
    } while (!isLibraryLoaded(targetLibName));

    uintptr_t base = findLibrary(targetLibName);
    LogDebug("%s successfully loaded at base address: 0x%lx", (const char *) targetLibName, (unsigned long) base);

#if defined(__aarch64__)

    // Resolve Unity Component & Transform methods
    Component_get_transform = reinterpret_cast<void *(*)(void *)>(base + 0x3cac2f4);
    Transform_get_position = reinterpret_cast<Vector3 (*)(void *)>(base + 0x3cbd5c0);
    GameObject_SetActive = reinterpret_cast<void (*)(void *, bool)>(base + 0x3caddcc);
    LogDebug("Resolve GameObject::SetActive (0x3caddcc): %s", GameObject_SetActive != nullptr ? "SUCCESS" : "FAILED");

    // Resolve Fret Particle Effects (Fire: byh 0x1f9d290, Spark: byi 0x1f9d308) & Unity ParticleSystem methods
    Main_Frets_byh = reinterpret_cast<void *(*)(void *, int32_t)>(base + 0x1f9d290); // fire
    Main_Frets_byi = reinterpret_cast<void *(*)(void *, int32_t)>(base + 0x1f9d308); // spark
    ParticleSystem_Play = reinterpret_cast<void (*)(void *, void *)>(base + 0x3d05c0c);
    ParticleSystem_Stop = reinterpret_cast<void (*)(void *, void *)>(base + 0x3d05dcc);
    LogDebug("Resolve Fret Particles: byh=%s, byi=%s, PS_Play=%s, PS_Stop=%s",
             Main_Frets_byh != nullptr ? "OK" : "FAIL",
             Main_Frets_byi != nullptr ? "OK" : "FAIL",
             ParticleSystem_Play != nullptr ? "OK" : "FAIL",
             ParticleSystem_Stop != nullptr ? "OK" : "FAIL");


    // 1. Hook Main_Scorer::cdc (0x20c38a0)
    HOOK("0x20c38a0", hook_Main_Scorer_cdc, old_Main_Scorer_cdc);
    LogDebug("Hook Main_Scorer::cdc (0x20c38a0): %s", old_Main_Scorer_cdc != nullptr ? "SUCCESS" : "FAILED");

    // 2. Hook Main_Notes::byy (0x20b8fec)
    HOOK("0x20b8fec", hook_Main_Notes_byy, old_Main_Notes_byy);
    LogDebug("Hook Main_Notes::byy (0x20b8fec): %s", old_Main_Notes_byy != nullptr ? "SUCCESS" : "FAILED");

    // 3. Hook Main_Notes::byz (0x20b9758)
    HOOK("0x20b9758", hook_Main_Notes_byz, old_Main_Notes_byz);
    LogDebug("Hook Main_Notes::byz (0x20b9758): %s", old_Main_Notes_byz != nullptr ? "SUCCESS" : "FAILED");

    // 4. Hook Main_Notes::byv (0x20b8350)
    HOOK("0x20b8350", hook_Main_Notes_byv, old_Main_Notes_byv);
    LogDebug("Hook Main_Notes::byv (0x20b8350): %s", old_Main_Notes_byv != nullptr ? "SUCCESS" : "FAILED");

    // 5. Hook Main_Notes::bzc (0x20b9874) - Sustain trail finish
    HOOK("0x20b9874", hook_Main_Notes_bzc, old_Main_Notes_bzc);
    LogDebug("Hook Main_Notes::bzc (0x20b9874): %s", old_Main_Notes_bzc != nullptr ? "SUCCESS" : "FAILED");

    // 6. Hook Main_Frets::byc (0x1f9cec8) - Visual fret press/release
    HOOK("0x1f9cec8", hook_Main_Frets_byc, old_Main_Frets_byc);
    LogDebug("Hook Main_Frets::byc (0x1f9cec8): %s", old_Main_Frets_byc != nullptr ? "SUCCESS" : "FAILED");

    // 7. Hook Main_Notes::bzd (0x20b9d5c) - Note position tracker & color circle auto hit
    HOOK("0x20b9d5c", hook_Main_Notes_bzd, old_Main_Notes_bzd);
    LogDebug("Hook Main_Notes::bzd (0x20b9d5c): %s", old_Main_Notes_bzd != nullptr ? "SUCCESS" : "FAILED");

    // 8. Hook Main_Frets::bye (0x1f9d1dc) - Sustain hold check
    HOOK("0x1f9d1dc", hook_Main_Frets_bye, old_Main_Frets_bye);
    LogDebug("Hook Main_Frets::bye (0x1f9d1dc): %s", old_Main_Frets_bye != nullptr ? "SUCCESS" : "FAILED");

    // 9. Hook Main::Update (0x20c50b0)
    HOOK("0x20c50b0", hook_Main_Update, old_Main_Update);
    LogDebug("Hook Main::Update (0x20c50b0): %s", old_Main_Update != nullptr ? "SUCCESS" : "FAILED");

    // 10. Hook Main_Sounds::cfe (0x20e267c) - Mute miss/strum error sound during Auto Play
    HOOK("0x20e267c", hook_Main_Sounds_cfe, old_Main_Sounds_cfe);
    LogDebug("Hook Main_Sounds::cfe (0x20e267c): %s", old_Main_Sounds_cfe != nullptr ? "SUCCESS" : "FAILED");

    // 11. Hook gc::blx (0x1eaa984) - Song getter by index
    HOOK("0x1eaa984", hook_gc_blx, old_gc_blx);
    LogDebug("Hook gc::blx (0x1eaa984): %s", old_gc_blx != nullptr ? "SUCCESS" : "FAILED");

    // Resolve gc::bma (0x1eaa9dc) - Song count getter
    gc_bma = reinterpret_cast<int32_t (*)(void *)>(base + 0x1eaa9dc);

    // 12. Hook ma::get_Vip (0x2116cfc) - Static VIP status getter
    HOOK("0x2116cfc", hook_ma_get_Vip, old_ma_get_Vip);
    LogDebug("Hook ma::get_Vip (0x2116cfc): %s", old_ma_get_Vip != nullptr ? "SUCCESS" : "FAILED");

    // 13. Hook MenuMus_Setlist::cnn (0x20f2ecc) - Setlist song play launcher
    HOOK("0x20f2ecc", hook_MenuMus_Setlist_cnn, old_MenuMus_Setlist_cnn);
    LogDebug("Hook MenuMus_Setlist::cnn (0x20f2ecc): %s", old_MenuMus_Setlist_cnn != nullptr ? "SUCCESS" : "FAILED");

    // 14. Hook MenuMus_Setlist::cno (0x20f3148) - Buy/Unlock song dialog interceptor
    HOOK("0x20f3148", hook_MenuMus_Setlist_cno, old_MenuMus_Setlist_cno);
    LogDebug("Hook MenuMus_Setlist::cno (0x20f3148): %s", old_MenuMus_Setlist_cno != nullptr ? "SUCCESS" : "FAILED");

    // 15. Resolve lx ObscuredInt helper functions
    lx_op_Implicit_to_int = reinterpret_cast<int32_t (*)(void *)>(base + 0x210bc94);
    lx_op_Implicit_from_int = reinterpret_cast<void *(*)(int32_t)>(base + 0x210d21c);

    // 16. Hook Main_Special::bzk (0x20bae98) - Special Ability Activation
    Main_Special_bzk = reinterpret_cast<void (*)(void *, bool)>(base + 0x20bae98);
    HOOK("0x20bae98", hook_Main_Special_bzk, old_Main_Special_bzk);
    LogDebug("Hook Main_Special::bzk (0x20bae98): %s", old_Main_Special_bzk != nullptr ? "SUCCESS" : "FAILED");

    // 17. Hook Main_Special::bzl (0x20bb068) - Special Ability Deactivation
    Main_Special_bzl = reinterpret_cast<void (*)(void *)>(base + 0x20bb068);
    HOOK("0x20bb068", hook_Main_Special_bzl, old_Main_Special_bzl);
    LogDebug("Hook Main_Special::bzl (0x20bb068): %s", old_Main_Special_bzl != nullptr ? "SUCCESS" : "FAILED");

    // 18. Hook ed::bab (0x1c31de0) - Chart Loader & Instance Capture
    HOOK("0x1c31de0", hook_ed_bab, old_ed_bab);
    LogDebug("Hook ed::bab (0x1c31de0): %s", old_ed_bab != nullptr ? "SUCCESS" : "FAILED");

    // 19. Hook Main_Notes::Add (0x20b8570) - Real-time Note & Star Power Chart Recorder
    HOOK("0x20b8570", hook_Main_Notes_Add, old_Main_Notes_Add);
    LogDebug("Hook Main_Notes::Add (0x20b8570): %s", old_Main_Notes_Add != nullptr ? "SUCCESS" : "FAILED");

    // Resolve ed methods for chart lookahead inspection
    ed_Count = reinterpret_cast<int32_t (*)(void *)>(base + 0x1c31f44);
    ed_bad = reinterpret_cast<void *(*)(void *, int32_t)>(base + 0x1c31f14);

    LogDebug("All Guitar Hero Real-Time, Auto Play, Smart Special & Song Unlock hooks installed successfully!");

#else 
    LogDebug("ARM32 architecture not supported for this 64-bit target");
#endif

    return NULL;
}

// =========================================================================
// JNI Method Implementations
// =========================================================================

// Returns [green, red, yellow, blue, orange, totalHits, totalMisses, totalCreated, totalAutoHits, autoPlay,
//          specialActive, specialEnergy, currentCombo, currentScore, autoSpecialEnabled, totalSpecialActivations]
jintArray GetNoteStats(JNIEnv *env, jobject thiz) {
    jintArray result = env->NewIntArray(16);
    if (!result) return nullptr;

    int32_t totalHits = getNotesHit();
    if (g_autoPlay.load() && totalHits > g_totalAutoHits.load()) {
        g_totalAutoHits = totalHits;
    }

    jint fill[16];
    fill[0] = g_greenHits.load();
    fill[1] = g_redHits.load();
    fill[2] = g_yellowHits.load();
    fill[3] = g_blueHits.load();
    fill[4] = g_orangeHits.load();
    fill[5] = totalHits;
    fill[6] = getNotesMiss();
    fill[7] = getNotesTotalCreated();
    fill[8] = g_totalAutoHits.load();
    fill[9] = g_autoPlay.load() ? 1 : 0;
    fill[10] = g_isSpecialActive.load() ? 1 : 0;
    fill[11] = g_specialEnergyLevel.load();
    fill[12] = g_currentCombo.load();
    fill[13] = g_currentScore.load();
    fill[14] = g_autoSpecial.load() ? 1 : 0;
    fill[15] = g_totalSpecialActivations.load();

    env->SetIntArrayRegion(result, 0, 16, fill);
    return result;
}

jobjectArray GetFeatureList(JNIEnv *env, jobject context) {
    jobjectArray ret;

    const char *features[] = {
            OBFUSCATE("Category_⚡ Automation"),
            OBFUSCATE("10_Toggle_Auto Play"),
            OBFUSCATE("11_Toggle_Auto Special (Smart Path)"),

            OBFUSCATE("Category_🎵 Songs & Unlocks"),
            OBFUSCATE("20_Toggle_Unlock All Songs"),

            OBFUSCATE("Category_Real-Time Note Counter"),
            OBFUSCATE("0_Toggle_Floating Note HUD"),
            OBFUSCATE("1_Toggle_Count Fret Errors as Miss"),
            OBFUSCATE("2_Button_Check Live Stats (Toast)"),
            OBFUSCATE("3_Button_Reset Note Counters"),
            OBFUSCATE("4_Button_Check Debug Log Path"),

            OBFUSCATE("Category_HUD & Color Legend"),
            OBFUSCATE("RichTextView_🟢 Green (0) | 🔴 Red (1) | 🟡 Yellow (2)<br>🔵 Blue (3) | 🟠 Orange (4)<br><small>⚡ Auto Play: 100% akurat tepat di lingkaran.<br>⭐ Auto Special: Smart path 8x score otomatis tanpa clipping SP phrase.<br>🎵 Unlock Songs: Buka seluruh lagu & akses VIP.</small>"),
    };

    int Total_Feature = (sizeof features / sizeof features[0]);
    ret = (jobjectArray)
            env->NewObjectArray(Total_Feature, env->FindClass(OBFUSCATE("java/lang/String")),
                                env->NewStringUTF(""));

    for (int i = 0; i < Total_Feature; i++)
        env->SetObjectArrayElement(ret, i, env->NewStringUTF(features[i]));

    return (ret);
}

void Changes(JNIEnv *env, jclass clazz, jobject obj,
             jint featNum, jstring featName, jint value,
             jboolean boolean, jstring str) {

    LOGD(OBFUSCATE("Feature: %d - %s | Value: %d | Bool: %d | Text: %s"), featNum,
         env->GetStringUTFChars(featName, 0), value,
         boolean, str != NULL ? env->GetStringUTFChars(str, 0) : "");

    switch (featNum) {
        // Auto Play Feature (OFF by default)
        case 10:
            g_autoPlay = boolean;
            LogDebug("Auto Play toggled: %d", boolean);
            Toast(env, obj, boolean ? OBFUSCATE("⚡ Auto Play: AKTIF") : OBFUSCATE("⚡ Auto Play: NONAKTIF"), ToastLength::LENGTH_SHORT);
            break;

        // Auto Special Ability (Smart Path)
        case 11:
            g_autoSpecial = boolean;
            LogDebug("Auto Special toggled: %d", boolean);
            Toast(env, obj, boolean ? OBFUSCATE("⭐ Auto Special (Smart Path): AKTIF") : OBFUSCATE("⭐ Auto Special: NONAKTIF"), ToastLength::LENGTH_SHORT);
            break;

        // Unlock All Songs Feature (OFF by default)
        case 20:
            g_unlockAllSongs = boolean;
            LogDebug("Unlock All Songs toggled: %d", boolean);
            if (boolean) {
                if (g_gcInstance != nullptr) {
                    UnlockAllSongsInMemory(g_gcInstance);
                }
                Toast(env, obj, OBFUSCATE("🎵 Unlock All Songs: AKTIF"), ToastLength::LENGTH_SHORT);
            } else {
                Toast(env, obj, OBFUSCATE("🎵 Unlock All Songs: NONAKTIF"), ToastLength::LENGTH_SHORT);
            }
            break;

        // Note Counter & HUD Features (OFF by default)
        case 0: // Toggle Floating Note HUD
            featureFloatingHUD = boolean;
            LogDebug("Feature Floating HUD toggled: %d", boolean);
            if (boolean) {
                Toast(env, obj, OBFUSCATE("Floating Note HUD: Aktif"), ToastLength::LENGTH_SHORT);
            } else {
                Toast(env, obj, OBFUSCATE("Floating Note HUD: Nonaktif"), ToastLength::LENGTH_SHORT);
            }
            break;

        case 1: // Toggle Count Fret Errors as Miss
            featureIncludeErradas = boolean;
            LogDebug("Feature Include Erradas toggled: %d", boolean);
            if (boolean) {
                Toast(env, obj, OBFUSCATE("Hitung Miss: Perdidas + Erradas"), ToastLength::LENGTH_SHORT);
            } else {
                Toast(env, obj, OBFUSCATE("Hitung Miss: Perdidas Saja"), ToastLength::LENGTH_SHORT);
            }
            break;

        case 2: { // Button: Check Live Stats (Toast)
            int32_t green = g_greenHits.load();
            int32_t red = g_redHits.load();
            int32_t yellow = g_yellowHits.load();
            int32_t blue = g_blueHits.load();
            int32_t orange = g_orangeHits.load();
            int32_t hit = getNotesHit();
            int32_t perd = getNotesPerdidas();
            int32_t err = getNotesErradas();
            int32_t miss = getNotesMiss();
            int32_t total = getNotesTotalCreated();
            int32_t autoTotal = g_totalAutoHits.load();
            int32_t combo = g_currentCombo.load();
            int32_t spEnergy = g_specialEnergyLevel.load();
            int32_t spActivations = g_totalSpecialActivations.load();
            bool spActive = g_isSpecialActive.load();

            size_t curStep = g_currentPathStep.load();
            size_t totalSteps = 0;
            int32_t nextTargetHit = -1;
            int32_t nextReqEnergy = 0;
            pthread_mutex_lock(&g_pathMutex);
            totalSteps = g_activePathMilestones.size();
            if (curStep < totalSteps) {
                nextTargetHit = g_activePathMilestones[curStep].targetHit;
                nextReqEnergy = g_activePathMilestones[curStep].requiredEnergy;
            }
            pthread_mutex_unlock(&g_pathMutex);

            char statsMsg[700];
            snprintf(statsMsg, sizeof(statsMsg),
                     "LIVE STATS:\n🟢 G:%d 🔴 R:%d 🟡 Y:%d 🔵 B:%d 🟠 O:%d\n🎯 Total Hit: %d | ⚡ Auto Hit: %d\n🔥 Combo: %d | ❌ Miss: %d\n⭐ SP: %s | Energy: %d/4\n📋 Path Step: %zu/%zu | Target Hit: %d (Req: %d bars)\n⚡ Activations: %d | Total Notes: %d",
                     green, red, yellow, blue, orange, hit, autoTotal, combo, miss,
                     spActive ? "8x ACTIVE 🔥" : (g_autoSpecial.load() ? "READY (Deterministic Path)" : "OFF"),
                     spEnergy, curStep, totalSteps, nextTargetHit, nextReqEnergy,
                     spActivations, total);

            LogDebug("User clicked Live Stats: Hit=%d, Auto=%d, Combo=%d, SPActive=%d, Miss=%d, Step=%zu/%zu",
                     hit, autoTotal, combo, spActive ? 1 : 0, miss, curStep, totalSteps);
            Toast(env, obj, statsMsg, ToastLength::LENGTH_LONG);
            break;
        }

        case 3: // Button: Reset Note Counters
            resetAllNoteCounters();
            Toast(env, obj, OBFUSCATE("Semua counter note, auto hit & special path di-reset ke 0!"), ToastLength::LENGTH_SHORT);
            break;

        case 4: // Button: Check Debug Log Path
            LogDebug("User verified debug log path via menu");
            Toast(env, obj, OBFUSCATE("Log Debug aktif di:\n/sdcard/0/Documents/guitar_hero_debug.log"), ToastLength::LENGTH_LONG);
            break;
    }
}

__attribute__((constructor))
void lib_main() {
    pthread_t ptid;
    pthread_create(&ptid, NULL, hack_thread, NULL);
}

int RegisterMenu(JNIEnv *env) {
    JNINativeMethod methods[] = {
            {OBFUSCATE("Icon"), OBFUSCATE("()Ljava/lang/String;"), reinterpret_cast<void *>(Icon)},
            {OBFUSCATE("IconWebViewData"),  OBFUSCATE("()Ljava/lang/String;"), reinterpret_cast<void *>(IconWebViewData)},
            {OBFUSCATE("IsGameLibLoaded"),  OBFUSCATE("()Z"), reinterpret_cast<void *>(isGameLibLoaded)},
            {OBFUSCATE("Init"),  OBFUSCATE("(Landroid/content/Context;Landroid/widget/TextView;Landroid/widget/TextView;)V"), reinterpret_cast<void *>(Init)},
            {OBFUSCATE("SettingsList"),  OBFUSCATE("()[Ljava/lang/String;"), reinterpret_cast<void *>(SettingsList)},
            {OBFUSCATE("GetFeatureList"),  OBFUSCATE("()[Ljava/lang/String;"), reinterpret_cast<void *>(GetFeatureList)},
            {OBFUSCATE("GetNoteStats"),  OBFUSCATE("()[I"), reinterpret_cast<void *>(GetNoteStats)},
    };

    jclass clazz = env->FindClass(OBFUSCATE("com/android/support/Menu"));
    if (!clazz)
        return JNI_ERR;
    if (env->RegisterNatives(clazz, methods, sizeof(methods) / sizeof(methods[0])) != 0)
        return JNI_ERR;
    return JNI_OK;
}

int RegisterPreferences(JNIEnv *env) {
    JNINativeMethod methods[] = {
            {OBFUSCATE("Changes"), OBFUSCATE("(Landroid/content/Context;ILjava/lang/String;IZLjava/lang/String;)V"), reinterpret_cast<void *>(Changes)},
    };
    jclass clazz = env->FindClass(OBFUSCATE("com/android/support/Preferences"));
    if (!clazz)
        return JNI_ERR;
    if (env->RegisterNatives(clazz, methods, sizeof(methods) / sizeof(methods[0])) != 0)
        return JNI_ERR;
    return JNI_OK;
}

int RegisterMain(JNIEnv *env) {
    JNINativeMethod methods[] = {
            {OBFUSCATE("CheckOverlayPermission"), OBFUSCATE("(Landroid/content/Context;)V"), reinterpret_cast<void *>(CheckOverlayPermission)},
    };
    jclass clazz = env->FindClass(OBFUSCATE("com/android/support/Main"));
    if (!clazz)
        return JNI_ERR;
    if (env->RegisterNatives(clazz, methods, sizeof(methods) / sizeof(methods[0])) != 0)
        return JNI_ERR;

    return JNI_OK;
}

extern "C"
JNIEXPORT jint JNICALL
JNI_OnLoad(JavaVM *vm, void *reserved) {
    JNIEnv *env;
    vm->GetEnv((void **) &env, JNI_VERSION_1_6);
    if (RegisterMenu(env) != 0)
        return JNI_ERR;
    if (RegisterPreferences(env) != 0)
        return JNI_ERR;
    if (RegisterMain(env) != 0)
        return JNI_ERR;
    return JNI_VERSION_1_6;
}
