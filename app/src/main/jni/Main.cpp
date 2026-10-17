#include <list>
#include <vector>
#include <algorithm>
#include <cmath>
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
static float g_fretSustainStartTime[5]{0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
static std::atomic<int32_t> g_autoHitsCount[5]{0, 0, 0, 0, 0};
static std::atomic<int32_t> g_totalAutoHits{0};

// Live combo and score trackers
static std::atomic<int32_t> g_currentCombo{0};
static std::atomic<int32_t> g_currentScore{0};

// Pointers to active game instances
static void *mainInstance = nullptr;        // Pointer to Main (MonoBehaviour)
static void *mainNotesInstance = nullptr;   // Pointer to Main_Notes
static void *mainSpecialInstance = nullptr; // Pointer to Main_Special
static void *mainScorerInstance = nullptr;  // Pointer to Main_Scorer
static void *mainFretsInstance = nullptr;   // Pointer to Main_Frets
static void *g_chartInstance = nullptr;     // Pointer to ed (Chart Manager)

// Special / Star Power Feature (OFF by default)
static std::atomic<bool> g_instantFullSpecialEnergy{false};
static void (*GameObject_SetActive)(void *gameObject, bool value, void *methodInfo) = nullptr;
static void *(*lx_op_Implicit_from_int)(int32_t value, void *methodInfo) = nullptr;
static int32_t (*lx_op_Implicit_to_int)(void *obscuredInt, void *methodInfo) = nullptr;
static void (*Main_Special_bzk)(void *instance, bool a) = nullptr; // RVA: 0x20bae98 - Special Power native activation
static std::atomic<bool> g_autoUseSpecial{false};

// Helper: Inspects active notes in Main_Notes::eoq directly from the engine
// Returns true if a note on this fret is hit and actively sustaining
static bool isFretActivelySustaining(int32_t fret) {
    if (mainNotesInstance == nullptr || fret < 0 || fret >= 5) {
        return false;
    }

    // List<hm> eoq at offset 0x20 in Main_Notes
    void *eoq = *(void **)((uintptr_t)mainNotesInstance + 0x20);
    if (eoq == nullptr || (uintptr_t)eoq < 0x10000) return false;

    // Array of hm items at offset 0x10 in List<T>
    void *items = *(void **)((uintptr_t)eoq + 0x10);
    if (items == nullptr || (uintptr_t)items < 0x10000) return false;

    int32_t size = *(int32_t *)((uintptr_t)eoq + 0x18);
    int32_t capacity = *(int32_t *)((uintptr_t)items + 0x18);
    if (size <= 0 || size > 300) return false;
    if (size > capacity && capacity > 0) size = capacity;

    for (int32_t i = 0; i < size; i++) {
        void *hm = *(void **)((uintptr_t)items + 0x20 + i * sizeof(void *));
        if (hm == nullptr || (uintptr_t)hm < 0x10000) continue;

        int32_t noteFret = *(int32_t *)((uintptr_t)hm + 0x3c);
        if (noteFret != fret) continue;

        int32_t noteStatus = *(int32_t *)((uintptr_t)hm + 0x40);
        // NoteStatus: 0 = normal (upcoming), 1 = hit, 2 = missed
        if (noteStatus != 1) continue;

        void *rastro = *(void **)((uintptr_t)hm + 0x28);
        if (rastro == nullptr || (uintptr_t)rastro < 0x10000) continue;

        bool hasRastro = *(bool *)((uintptr_t)rastro + 0x20);
        if (!hasRastro) continue;

        int32_t rastroStatus = *(int32_t *)((uintptr_t)rastro + 0x24);
        // RastroStatus: 0 = normal (just hit), 1 = hitting (sustaining)
        // 2 = missed or finished naturally when tail passes strike line
        if (rastroStatus == 0 || rastroStatus == 1) {
            return true;
        }
    }
    return false;
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

// Function pointers for fret fire particle burst & Unity ParticleSystem (from commit e4947d)
static void *(*Main_Frets_byh)(void *instance, int32_t fret) = nullptr;
static void (*ParticleSystem_Play)(void *instance, void *dummy) = nullptr;

// Helper to trigger the fire particle burst on a fret circle (from commit e4947d)
static void TriggerFretFireEffect(int32_t fret) {
    if (fret < 0 || fret >= 5) return;
    if (Main_Frets_byh != nullptr && ParticleSystem_Play != nullptr && mainFretsInstance != nullptr) {
        void *ps = Main_Frets_byh(mainFretsInstance, fret);
        if (ps != nullptr) {
            ParticleSystem_Play(ps, nullptr);
        }
    }
}

// Forward declaration of visual fret function pointer
void (*old_Main_Frets_byc)(void *instance, int32_t fret, bool pressed) = nullptr;

// =========================================================================
// Chart Extraction, Star Power Path & Potential Score Engine State
// =========================================================================

struct CachedSongNote {
    int32_t num;
    int32_t fret;
    float time;
    float duration;
    int32_t special;
};

struct SpPathMilestone {
    int32_t targetHit;       // Hit / Combo count to trigger activation
    int32_t requiredEnergy;  // Required energy bars (2, 3, or 4)
    float startTime;         // Song time of activation
    float duration;          // Duration of activation (bars * 3.75s)
};

static pthread_mutex_t g_chartNotesMutex = PTHREAD_MUTEX_INITIALIZER;
static std::vector<CachedSongNote> g_cachedChartNotes;

static pthread_mutex_t g_chartAnalysisMutex = PTHREAD_MUTEX_INITIALIZER;
static bool g_chartHasData = false;
static std::atomic<bool> g_chartAnalyzed{false};
static std::atomic<bool> g_chartNeedReanalysis{false};
static std::string g_songTitle = "Menunggu Lagu...";
static std::string g_songTitleFromBab = "";
static float g_songDuration = 0.0f;
static std::string g_songDurationStr = "--:--";
static int32_t g_totalChartNotes = 0;
static int32_t g_totalSpNotes = 0;
static int32_t g_totalSpPhrases = 0;
static int64_t g_bestPotentialScore = 0;
static int64_t g_baseFcScore = 0;
static std::string g_pathSummary = "-";
static std::vector<SpPathMilestone> g_activePathMilestones;
static std::atomic<size_t> g_currentMilestoneIndex{0};

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
        g_fretSustainStartTime[i] = 0.0f;
        g_autoHitsCount[i] = 0;
        if (old_Main_Frets_byc != nullptr && mainFretsInstance != nullptr) {
            old_Main_Frets_byc(mainFretsInstance, i, false);
        }
    }
    g_totalAutoHits = 0;
    g_currentCombo = 0;
    g_currentScore = 0;
    g_currentMilestoneIndex = 0;

    if (mainNotesInstance != nullptr) {
        *(int32_t *)((uintptr_t)mainNotesInstance + 0x64) = 0; // acertadas
        *(int32_t *)((uintptr_t)mainNotesInstance + 0x68) = 0; // acertadasSP
        *(int32_t *)((uintptr_t)mainNotesInstance + 0x6c) = 0; // erradas
        *(int32_t *)((uintptr_t)mainNotesInstance + 0x70) = 0; // perdidas
    }

    pthread_mutex_lock(&g_chartNotesMutex);
    g_cachedChartNotes.clear();
    pthread_mutex_unlock(&g_chartNotesMutex);
    g_chartNeedReanalysis = true;

    LogDebug("User reset all note counters and auto-hit stats to 0");
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



static std::string Il2CppStringToStdString(void *il2cppString) {
    if (il2cppString == nullptr || (uintptr_t)il2cppString < 0x10000) {
        return "";
    }
    int32_t len = *(int32_t *)((uintptr_t)il2cppString + 0x10);
    if (len <= 0 || len > 256) {
        return "";
    }
    uint16_t *chars = (uint16_t *)((uintptr_t)il2cppString + 0x14);
    std::string res;
    res.reserve(len);
    for (int32_t i = 0; i < len; i++) {
        uint16_t c = chars[i];
        if (c < 128) {
            res.push_back((char)c);
        } else if (c < 0x800) {
            res.push_back((char)(0xC0 | (c >> 6)));
            res.push_back((char)(0x80 | (c & 0x3F)));
        } else {
            res.push_back((char)(0xE0 | (c >> 12)));
            res.push_back((char)(0x80 | ((c >> 6) & 0x3F)));
            res.push_back((char)(0x80 | (c & 0x3F)));
        }
    }
    return res;
}

static std::string FormatDuration(float seconds) {
    if (seconds <= 0.0f) return "00:00";
    int32_t totalSec = (int32_t)seconds;
    int32_t mins = totalSec / 60;
    int32_t secs = totalSec % 60;
    char buf[32];
    snprintf(buf, sizeof(buf), "%02d:%02d", mins, secs);
    return std::string(buf);
}

static std::string EscapeJsonString(const std::string &input) {
    std::string output;
    for (char c : input) {
        if (c == '"') output += "\\\"";
        else if (c == '\\') output += "\\\\";
        else if (c == '\b') output += "\\b";
        else if (c == '\f') output += "\\f";
        else if (c == '\n') output += "\\n";
        else if (c == '\r') output += "\\r";
        else if (c == '\t') output += "\\t";
        else if ((unsigned char)c >= 32) output += c;
    }
    return output;
}

static void AnalyzeChartAndCalculateMetrics() {
    std::vector<CachedSongNote> notes;

    // 1. Try to read directly from g_chartInstance (ed) safely
    if (g_chartInstance != nullptr && (uintptr_t)g_chartInstance > 0x10000) {
        void *dya = *(void **)((uintptr_t)g_chartInstance + 0x20);
        if (dya != nullptr && (uintptr_t)dya > 0x10000) {
            int32_t count = *(int32_t *)((uintptr_t)dya + 0x18);
            if (count > 0 && count < 30000) {
                void **items = (void **)((uintptr_t)dya + 0x20);
                notes.reserve(count);
                for (int32_t i = 0; i < count; i++) {
                    void *nc = items[i];
                    if (nc == nullptr || (uintptr_t)nc < 0x10000) continue;
                    CachedSongNote note;
                    note.num = i;
                    note.fret = *(int32_t *)((uintptr_t)nc + 0x10);
                    note.time = *(float *)((uintptr_t)nc + 0x14);
                    note.duration = *(float *)((uintptr_t)nc + 0x18);
                    note.special = *(int32_t *)((uintptr_t)nc + 0x1c);
                    notes.push_back(note);
                }
            }
        }
    }

    // 2. Fallback to g_cachedChartNotes if direct read was empty
    if (notes.empty()) {
        pthread_mutex_lock(&g_chartNotesMutex);
        notes = g_cachedChartNotes;
        pthread_mutex_unlock(&g_chartNotesMutex);
    }

    if (notes.empty()) {
        return; // Chart not ready yet
    }

    // 3. Extract Song Title safely
    std::string title = "";
    if (g_chartInstance != nullptr && (uintptr_t)g_chartInstance > 0x10000) {
        void *dyb = *(void **)((uintptr_t)g_chartInstance + 0x28); // artist
        void *dyc = *(void **)((uintptr_t)g_chartInstance + 0x30); // title
        std::string artist = Il2CppStringToStdString(dyb);
        std::string song = Il2CppStringToStdString(dyc);
        if (!artist.empty() && !song.empty()) {
            title = artist + " - " + song;
        } else if (!song.empty()) {
            title = song;
        } else if (!artist.empty()) {
            title = artist;
        }
    }
    if (title.empty()) {
        pthread_mutex_lock(&g_chartAnalysisMutex);
        title = g_songTitleFromBab;
        pthread_mutex_unlock(&g_chartAnalysisMutex);
    }
    if (title.empty()) {
        title = "Guitar Flash Track";
    }

    // 4. Extract Song Duration
    float duration = 0.0f;
    if (g_chartInstance != nullptr && (uintptr_t)g_chartInstance > 0x10000) {
        duration = *(float *)((uintptr_t)g_chartInstance + 0x38); // dyd (Length)
    }
    if (duration <= 0.0f && !notes.empty()) {
        duration = notes.back().time + notes.back().duration;
    }

    // 5. Group Special Notes into Phrases
    int32_t spNotesCount = 0;
    struct SpPhrase {
        int32_t startNote;
        int32_t endNote;
        int32_t count;
        float startTime;
        float endTime;
    };
    std::vector<SpPhrase> phrases;
    bool inPhrase = false;
    SpPhrase curPhrase{0, 0, 0, 0.0f, 0.0f};

    for (size_t i = 0; i < notes.size(); i++) {
        if (notes[i].special > 0) {
            spNotesCount++;
            if (!inPhrase) {
                inPhrase = true;
                curPhrase.startNote = (int32_t)(i + 1); // 1-indexed combo hit
                curPhrase.startTime = notes[i].time;
                curPhrase.count = 0;
            }
            curPhrase.endNote = (int32_t)(i + 1); // 1-indexed combo hit
            curPhrase.endTime = notes[i].time + notes[i].duration;
            curPhrase.count++;
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
    int32_t totalPhrases = (int32_t)phrases.size();

    // 6. Build Deterministic Optimal Path via Dynamic Programming (100% Dynamic Global Score Optimizer)
    std::vector<SpPathMilestone> milestones;
    if (!phrases.empty()) {
        int32_t P = (int32_t)phrases.size();
        int32_t numNotes = (int32_t)notes.size();

        // Precompute note score prefix sums for fast O(1) interval bonus calculation
        // Each note at 4x combo gives 20 pts base bonus + sustain bonus under Special Power
        std::vector<int64_t> prefixBonus(numNotes + 1, 0);
        int32_t simC = 0;
        for (int32_t i = 0; i < numNotes; i++) {
            int64_t nBase = 5;
            float mult = 1.0f;
            if (simC >= 30) { nBase = 20; mult = 4.0f; }
            else if (simC >= 20) { nBase = 15; mult = 3.0f; }
            else if (simC >= 10) { nBase = 10; mult = 2.0f; }

            int64_t sBase = 0;
            if (notes[i].duration > 0.1f) {
                sBase = (int64_t)(notes[i].duration * 2.5f * mult);
            }
            prefixBonus[i + 1] = prefixBonus[i] + (nBase + sBase);
            simC++;
        }

        // Helper lambda to calculate bonus points gained in window (startT, startT + dur]
        auto getWindowBonus = [&](float startT, float dur) -> int64_t {
            float endT = startT + dur;
            // Binary search for first note with time > startT
            int32_t left = 0, right = numNotes;
            while (left < right) {
                int32_t mid = left + (right - left) / 2;
                if (notes[mid].time <= startT) {
                    left = mid + 1;
                } else {
                    right = mid;
                }
            }
            int32_t startIdx = left;

            // Binary search for first note with time > endT
            left = startIdx; right = numNotes;
            while (left < right) {
                int32_t mid = left + (right - left) / 2;
                if (notes[mid].time <= endT) {
                    left = mid + 1;
                } else {
                    right = mid;
                }
            }
            int32_t endIdx = left;

            return prefixBonus[endIdx] - prefixBonus[startIdx];
        };

        // DP Table: (P + 1) rows, 5 columns (energy 0..4)
        // dp[p][e]: maximum future score from phrase p onwards given incoming energy e
        // action[p][e]: 0 = HOLD, 2, 3, 4 = ACTIVATE with that energy
        std::vector<std::vector<int64_t>> dp(P + 1, std::vector<int64_t>(5, -1));
        std::vector<std::vector<int32_t>> action(P + 1, std::vector<int32_t>(5, 0));

        // Base case: after phrase P (end of song)
        for (int32_t e = 0; e <= 4; e++) {
            dp[P][e] = 0;
            action[P][e] = 0;
        }

        // Bottom-up DP from last phrase (P - 1) backwards to 0
        for (int32_t p = P - 1; p >= 0; p--) {
            for (int32_t e = 0; e <= 3; e++) {
                int32_t eCurr = std::min(4, e + 1);
                int64_t bestScore = -1;
                int32_t bestAct = 0;

                // Option 1: HOLD (if not last phrase and eCurr < 4)
                if (p < P - 1 && eCurr < 4) {
                    if (dp[p + 1][eCurr] >= 0) {
                        bestScore = dp[p + 1][eCurr];
                        bestAct = 0; // HOLD
                    }
                }

                // Option 2: ACTIVATE with eCurr (if eCurr >= 2)
                if (eCurr >= 2) {
                    float dur = (float)eCurr * 3.75f;
                    float startT = phrases[p].endTime;
                    bool canActivate = false;

                    if (p == P - 1) {
                        canActivate = true; // Last phrase of song
                    } else if (startT + dur <= phrases[p + 1].startTime + 0.20f) {
                        canActivate = true;
                    }

                    if (canActivate) {
                        int64_t wBonus = getWindowBonus(startT, dur);
                        if (dp[p + 1][0] >= 0) {
                            int64_t tot = wBonus + dp[p + 1][0];
                            if (tot > bestScore) {
                                bestScore = tot;
                                bestAct = eCurr; // ACTIVATE with eCurr
                            }
                        }
                    }
                }

                // Fallback: If full eCurr didn't fit gap, test smaller energy (testE = 3, 2)
                if (bestScore < 0 && eCurr >= 2) {
                    for (int32_t testE = eCurr - 1; testE >= 2; testE--) {
                        float dur = (float)testE * 3.75f;
                        float startT = phrases[p].endTime;
                        if (p == P - 1 || startT + dur <= phrases[p + 1].startTime + 0.20f) {
                            int64_t wBonus = getWindowBonus(startT, dur);
                            if (dp[p + 1][0] >= 0) {
                                int64_t tot = wBonus + dp[p + 1][0];
                                if (tot > bestScore) {
                                    bestScore = tot;
                                    bestAct = testE;
                                }
                            }
                        }
                    }
                }

                // Safety fallback: ensure a valid transition exists
                if (bestScore < 0) {
                    if (p < P - 1) {
                        bestScore = (dp[p + 1][eCurr] >= 0) ? dp[p + 1][eCurr] : 0;
                        bestAct = 0;
                    } else {
                        bestScore = 0;
                        bestAct = (eCurr >= 2) ? eCurr : 0;
                    }
                }

                dp[p][e] = bestScore;
                action[p][e] = bestAct;
            }
        }

        // Reconstruct the optimal path starting from phrase 0 with energy 0
        int32_t curE = 0;
        for (int32_t p = 0; p < P; p++) {
            int32_t act = action[p][curE];
            int32_t eCurr = std::min(4, curE + 1);

            if (act >= 2) {
                int32_t targetHit = phrases[p].endNote;
                if (targetHit < 30) targetHit = 30; // Wait for 4x base multiplier
                float dur = (float)act * 3.75f;
                milestones.push_back({ targetHit, act, phrases[p].endTime, dur });
                curE = 0; // Reset energy after activation
            } else {
                curE = eCurr;
            }
        }

        // Outro check: if energy remained after the last phrase (and was not yet scheduled)
        if (curE >= 2 && !notes.empty() && (milestones.empty() || milestones.back().targetHit != phrases.back().endNote)) {
            int32_t outroHit = phrases.back().endNote;
            if (outroHit < 30) outroHit = 30;
            float dur = (float)curE * 3.75f;
            milestones.push_back({ outroHit, curE, phrases.back().endTime, dur });
        }
    } else {
        // Fallback for custom charts without star notes
        for (int32_t h = 120; h < (int32_t)notes.size() - 50; h += 200) {
            milestones.push_back({h, 2, (float)h * 0.2f, 7.5f});
        }
    }

    // 7. Format Path Summary string (e.g. "2-2-2-2-2-2-2-3-3-2-2-2-4")
    std::string pathSummary = "";
    for (size_t i = 0; i < milestones.size(); i++) {
        if (i > 0) pathSummary += "-";
        pathSummary += std::to_string(milestones[i].requiredEnergy);
    }
    if (pathSummary.empty()) pathSummary = "-";

    // 8. Calculate Best Potential Score & Base FC Score using exact Guitar Flash scoring rules
    int64_t bestScore = 0;
    int64_t baseScore = 0;
    int32_t simCombo = 0;

    for (size_t i = 0; i < notes.size(); i++) {
        // Base note points:
        // Combo 0..9: 5 pts (1x)
        // Combo 10..19: 10 pts (2x)
        // Combo 20..29: 15 pts (3x)
        // Combo >= 30: 20 pts (4x)
        int64_t noteBase = 5;
        float mult = 1.0f;
        if (simCombo >= 30) {
            noteBase = 20;
            mult = 4.0f;
        } else if (simCombo >= 20) {
            noteBase = 15;
            mult = 3.0f;
        } else if (simCombo >= 10) {
            noteBase = 10;
            mult = 2.0f;
        }

        // Sustain points
        int64_t sustainBase = 0;
        if (notes[i].duration > 0.1f) {
            sustainBase = (int64_t)(notes[i].duration * 2.5f * mult);
        }
        baseScore += noteBase + sustainBase;

        // Check if note falls within an active SP window
        bool isSpActive = false;
        for (const auto &m : milestones) {
            if (notes[i].time > m.startTime && notes[i].time <= (m.startTime + m.duration)) {
                isSpActive = true;
                break;
            }
        }

        // With Special Power active:
        // Multiplier doubles: 10, 20, 30, 40 pts and sustain points double
        int64_t noteSp = isSpActive ? (noteBase * 2) : noteBase;
        int64_t sustainSp = isSpActive ? (sustainBase * 2) : sustainBase;
        bestScore += noteSp + sustainSp;

        simCombo++;
    }

    // 9. Store Analyzed Metrics (Thread-Safe)
    pthread_mutex_lock(&g_chartAnalysisMutex);
    g_chartHasData = true;
    g_chartAnalyzed = true;
    g_chartNeedReanalysis = false;
    g_songTitle = title;
    g_songDuration = duration;
    g_songDurationStr = FormatDuration(duration);
    g_totalChartNotes = (int32_t)notes.size();
    g_totalSpNotes = spNotesCount;
    g_totalSpPhrases = totalPhrases;
    g_bestPotentialScore = bestScore;
    g_baseFcScore = baseScore;
    g_pathSummary = pathSummary;
    g_activePathMilestones = milestones;

    // Sync current milestone index with current hits
    int32_t curHits = getNotesHit();
    size_t sIdx = 0;
    while (sIdx < milestones.size() && curHits >= milestones[sIdx].targetHit) {
        sIdx++;
    }
    g_currentMilestoneIndex = sIdx;

    pthread_mutex_unlock(&g_chartAnalysisMutex);

    LogDebug("⭐ [ChartAnalysis] Extracted '%s' | Dur: %s (%.1fs) | Notes: %d | SP Notes: %d (%d Frase) | Best Score: %lld (Base: %lld) | Path: %s (%zu steps)",
             title.c_str(), g_songDurationStr.c_str(), duration, (int)notes.size(),
             spNotesCount, totalPhrases, (long long)bestScore, (long long)baseScore,
             pathSummary.c_str(), milestones.size());
}

// Hook ed::bab (Chart loader & Song detector)
static void (*old_ed_bab)(void *instance, int32_t a, void *b, int32_t c, void *d, void *e) = nullptr;
static void hook_ed_bab(void *instance, int32_t a, void *b, int32_t c, void *d, void *e) {
    if (instance != nullptr && (uintptr_t)instance > 0x10000) {
        g_chartInstance = instance;
    }
    if (b != nullptr && (uintptr_t)b > 0x10000) {
        std::string name = Il2CppStringToStdString(b);
        if (!name.empty()) {
            pthread_mutex_lock(&g_chartAnalysisMutex);
            g_songTitleFromBab = name;
            pthread_mutex_unlock(&g_chartAnalysisMutex);
        }
    }
    g_chartNeedReanalysis = true;

    if (old_ed_bab != nullptr) {
        old_ed_bab(instance, a, b, c, d, e);
    }
}

// Hook Main_Notes::Add (Captures mainNotesInstance and caches chart notes)
static void (*old_Main_Notes_Add)(void *instance, int32_t num, void *noteChart) = nullptr;
static void hook_Main_Notes_Add(void *instance, int32_t num, void *noteChart) {
    if (instance != nullptr) {
        mainNotesInstance = instance;
    }
    if (noteChart != nullptr) {
        CachedSongNote note;
        note.num = num;
        note.fret = *(int32_t *)((uintptr_t)noteChart + 0x10);
        note.time = *(float *)((uintptr_t)noteChart + 0x14);
        note.duration = *(float *)((uintptr_t)noteChart + 0x18);
        note.special = *(int32_t *)((uintptr_t)noteChart + 0x1c);

        pthread_mutex_lock(&g_chartNotesMutex);
        g_cachedChartNotes.push_back(note);
        pthread_mutex_unlock(&g_chartNotesMutex);
    }
    if (old_Main_Notes_Add != nullptr) {
        old_Main_Notes_Add(instance, num, noteChart);
    }
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

// Automatic Special Power Trigger based on Dynamic Optimal Route
static void CheckAndTriggerAutoSpecial() {
    if (!g_autoUseSpecial.load() && !g_autoPlay.load()) {
        return;
    }
    if (mainSpecialInstance == nullptr || (uintptr_t)mainSpecialInstance < 0x10000) {
        return;
    }
    if (Main_Special_bzk == nullptr) {
        return;
    }

    // Check if Special Power is already active (active: bool at offset 0x21)
    bool isSpActive = *(bool *)((uintptr_t)mainSpecialInstance + 0x21);
    if (isSpActive) {
        return;
    }

    // Check timeBlocked at offset 0x24
    float timeBlocked = *(float *)((uintptr_t)mainSpecialInstance + 0x24);
    if (timeBlocked > 0.0f) {
        return;
    }

    // Check current energy bars (epe: lx/ObscuredInt at offset 0x40)
    int32_t currentEnergy = 0;
    if (lx_op_Implicit_to_int != nullptr) {
        void *epe = *(void **)((uintptr_t)mainSpecialInstance + 0x40);
        if (epe != nullptr) {
            currentEnergy = lx_op_Implicit_to_int(epe, nullptr);
        }
    }
    if (currentEnergy < 2) {
        return; // Minimum 2 bars required to activate
    }

    int32_t currentHits = getNotesHit();

    pthread_mutex_lock(&g_chartAnalysisMutex);
    size_t idx = g_currentMilestoneIndex.load();
    // Catch up index if hits have far exceeded older milestone
    while (idx < g_activePathMilestones.size() && currentHits > g_activePathMilestones[idx].targetHit + 25) {
        idx++;
    }
    g_currentMilestoneIndex = idx;

    if (idx < g_activePathMilestones.size()) {
        const auto &m = g_activePathMilestones[idx];
        if (currentHits >= m.targetHit && currentEnergy >= m.requiredEnergy) {
            g_currentMilestoneIndex = idx + 1;
            pthread_mutex_unlock(&g_chartAnalysisMutex);

            LogDebug("⚡ [AutoSpecial] Triggering Special! Step %zu/%zu at Hit #%d (Target: %d, Energy: %d/%d)",
                     idx + 1, g_activePathMilestones.size(), currentHits, m.targetHit, currentEnergy, m.requiredEnergy);
            Main_Special_bzk(mainSpecialInstance, false);
            return;
        }
    }
    pthread_mutex_unlock(&g_chartAnalysisMutex);
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
            if (rastro != nullptr && (uintptr_t)rastro > 0x10000) {
                // bool has: offset 0x20
                hasRastro = *(bool *)((uintptr_t)rastro + 0x20);
            }

            if (hasRastro) {
                float songTime = 0.0f;
                if (mainInstance != nullptr) {
                    songTime = *(float *)((uintptr_t)mainInstance + 0x28);
                }
                g_fretSustaining[fret] = true;
                g_fretSustainStartTime[fret] = songTime;
                g_fretVisualTimer[fret] = 0; // Hold active sustain
                LogDebug("Sustain START: fret=%d, num=%d, songTime=%.2fs", fret, num, songTime);
            } else {
                // Normal tap note (no sustain trail)
                g_fretSustaining[fret] = false;
                g_fretSustainStartTime[fret] = 0.0f;
            }
        }
    }

    if (old_Main_Notes_byy != nullptr) {
        old_Main_Notes_byy(instance, hm, b);
    }

    // Auto Use Special check right upon hitting milestone notes
    CheckAndTriggerAutoSpecial();

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

    // 100% ACCURACY SAFETY NET FOR AUTO PLAY (from commit e4947d):
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

                int32_t fret = *(int32_t *)((uintptr_t)hm + 0x3c);
                if (fret >= 0 && fret < 5) {
                    TriggerFretFireEffect(fret);
                    if (old_Main_Frets_byc != nullptr && mainFretsInstance != nullptr) {
                        old_Main_Frets_byc(mainFretsInstance, fret, true);
                        if (g_fretSustaining[fret].load()) {
                            g_fretVisualTimer[fret] = 0;
                        } else {
                            g_fretVisualTimer[fret] = 5;
                        }
                    }
                    g_autoHitsCount[fret]++;
                    g_totalAutoHits++;
                    LogDebug("Auto Play 100%% Safety Hit: fret=%d, recovered unhit note at miss threshold", fret);
                }
                return; // Suppress miss completely - streak and combo preserved!
            }
        } else {
            // Stray strum error (erradas), suppress in Auto Play mode
            return;
        }
    }

    int32_t fret = -1;
    if (hm != nullptr) {
        fret = *(int32_t *)((uintptr_t)hm + 0x3c);
        switch (fret) {
            case 0: g_greenMisses++; break;
            case 1: g_redMisses++; break;
            case 2: g_yellowMisses++; break;
            case 3: g_blueMisses++; break;
            case 4: g_orangeMisses++; break;
            default: break;
        }
        if (fret >= 0 && fret < 5) {
            if (!isFretActivelySustaining(fret)) {
                g_fretSustaining[fret] = false;
                g_fretSustainStartTime[fret] = 0.0f;
                g_fretVisualTimer[fret] = 0;
                if (old_Main_Frets_byc != nullptr && mainFretsInstance != nullptr) {
                    old_Main_Frets_byc(mainFretsInstance, fret, false);
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
        g_fretSustainStartTime[i] = 0.0f;
        g_autoHitsCount[i] = 0;
        if (old_Main_Frets_byc != nullptr && mainFretsInstance != nullptr) {
            old_Main_Frets_byc(mainFretsInstance, i, false);
        }
    }
    g_totalAutoHits = 0;
    g_currentMilestoneIndex = 0;

    pthread_mutex_lock(&g_chartNotesMutex);
    g_cachedChartNotes.clear();
    pthread_mutex_unlock(&g_chartNotesMutex);
    g_chartNeedReanalysis = true;

    if (old_Main_Notes_byv != nullptr) {
        old_Main_Notes_byv(instance);
    }

    LogDebug("SONG START / RESTART: all note counters & auto-hit states reset to 0 (instance=0x%lx)", (unsigned long)instance);
}

// 5. Hook Main_Notes::bzc (RVA: 0x20b9874)
// Called when a note's sustain/rastro trail ends or misses
void (*old_Main_Notes_bzc)(void *instance, void *hm) = nullptr;
void hook_Main_Notes_bzc(void *instance, void *hm) {
    if (hm != nullptr) {
        int32_t fret = *(int32_t *)((uintptr_t)hm + 0x3c);
        if (fret >= 0 && fret < 5) {
            if (!isFretActivelySustaining(fret)) {
                g_fretSustaining[fret] = false;
                g_fretSustainStartTime[fret] = 0.0f;
                g_fretVisualTimer[fret] = 0;
                if (old_Main_Frets_byc != nullptr && mainFretsInstance != nullptr) {
                    old_Main_Frets_byc(mainFretsInstance, fret, false); // Release visual fret
                }
            }
        }
    }
    if (old_Main_Notes_bzc != nullptr) {
        old_Main_Notes_bzc(instance, hm);
    }
}

// 6. Hook Main_Frets::byc (RVA: 0x1f9cec8)
// Animates and controls visual button press/release on screen (from commit e4947d)
void hook_Main_Frets_byc(void *instance, int32_t fret, bool pressed) {
    if (instance != nullptr) {
        mainFretsInstance = instance;
    }

    if (g_autoPlay.load() && fret >= 0 && fret < 5) {
        // Prevent idle touch polling (byb) from canceling the visual touch during active hit or sustain
        if (!pressed && (g_fretVisualTimer[fret] > 0 || g_fretSustaining[fret].load() || isFretActivelySustaining(fret))) {
            return;
        }
    }

    if (old_Main_Frets_byc != nullptr) {
        old_Main_Frets_byc(instance, fret, pressed);
    }
}

// 7. Hook Main_Notes::bzd (RVA: 0x20b9d5c)
// Called every frame for each active note to update position
// Triggers Auto Play HIT precisely when note enters the color circle! (from commit e4947d)
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

            // In Guitar Flash (from commit e4947d):
            // Center of the color circle is at Z ≈ -13.05f.
            // When pos.z <= -12.7f, the note is visually inside the color circle!
            // Hits at Z ∈ [-12.7f, -15.5f] look completely natural, legit, and centered.
            if (pos.z <= -12.7f && pos.z >= -15.5f) {
                // Execute Perfect hit directly on note
                hook_Main_Notes_byy(instance, hm, time);

                // Trigger FIRE particle burst on the fret circle! (from commit e4947d)
                TriggerFretFireEffect(fret);

                // Animate visual fret touch press synchronized with hit fire burst
                if (old_Main_Frets_byc != nullptr && mainFretsInstance != nullptr) {
                    old_Main_Frets_byc(mainFretsInstance, fret, true);
                    if (g_fretSustaining[fret].load()) {
                        g_fretVisualTimer[fret] = 0; // Sustain hold is active
                    } else {
                        g_fretVisualTimer[fret] = 5; // Crisp visual tap animation (~83ms, synchronized with hit)
                    }
                }

                g_autoHitsCount[fret]++;
                g_totalAutoHits++;
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
        if (isFretActivelySustaining(fret) || g_fretSustaining[fret].load()) {
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

        // Main.special: 0xc0
        void *special = *(void **)((uintptr_t)instance + 0xc0);
        if (special != nullptr) {
            mainSpecialInstance = special;
        }

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

        // Main.scorer: 0x110
        void *scorer = *(void **)((uintptr_t)instance + 0x110);
        if (scorer != nullptr) {
            mainScorerInstance = scorer;
        }

        // Main.chart: 0x58
        void *chart = *(void **)((uintptr_t)instance + 0x58);
        if (chart != nullptr && (uintptr_t)chart > 0x10000) {
            g_chartInstance = chart;
            void *dya = *(void **)((uintptr_t)chart + 0x20);
            if (dya != nullptr && (uintptr_t)dya > 0x10000) {
                int32_t count = *(int32_t *)((uintptr_t)dya + 0x18);
                if (count > 0 && (!g_chartAnalyzed.load() || g_chartNeedReanalysis.load())) {
                    AnalyzeChartAndCalculateMetrics();
                }
            }
        }

        if (!g_mainCapturedLogged && notes != nullptr) {
            g_mainCapturedLogged = true;
            LogDebug("Captured active instances: Main=0x%lx, Frets=0x%lx, Notes=0x%lx, Special=0x%lx, Scorer=0x%lx, Chart=0x%lx",
                     (unsigned long)mainInstance, (unsigned long)mainFretsInstance,
                     (unsigned long)mainNotesInstance, (unsigned long)mainSpecialInstance,
                     (unsigned long)mainScorerInstance, (unsigned long)g_chartInstance);
        }
    }

    float songTime = 0.0f;
    if (mainInstance != nullptr) {
        songTime = *(float *)((uintptr_t)mainInstance + 0x28);
    }

    // Manage visual fret hold and release timers
    for (int i = 0; i < 5; i++) {
        bool activelySustaining = isFretActivelySustaining(i);

        // A. Manage sustain note hold and natural finish
        if (g_fretSustaining[i].load()) {
            bool timeExceeded = (songTime > 0.0f && g_fretSustainStartTime[i] > 0.0f && 
                                (songTime - g_fretSustainStartTime[i] > 6.0f || songTime < g_fretSustainStartTime[i] - 1.0f));

            if (!activelySustaining || timeExceeded) {
                g_fretSustaining[i] = false;
                g_fretSustainStartTime[i] = 0.0f;
                g_fretVisualTimer[i] = 0;
                if (old_Main_Frets_byc != nullptr && mainFretsInstance != nullptr) {
                    old_Main_Frets_byc(mainFretsInstance, i, false); // Release visual fret cleanly!
                }
                LogDebug("Sustain END: fret=%d released cleanly (active=%d, timeExceeded=%d)", 
                         i, activelySustaining ? 1 : 0, timeExceeded ? 1 : 0);
            }
        } else if (activelySustaining) {
            g_fretSustaining[i] = true;
            if (g_fretSustainStartTime[i] <= 0.0f) {
                g_fretSustainStartTime[i] = songTime;
            }
        }

        // B. Release tap timer for standard notes
        if (g_fretVisualTimer[i] > 0) {
            int t = --g_fretVisualTimer[i];
            if (t == 0 && !g_fretSustaining[i].load() && !activelySustaining) {
                if (old_Main_Frets_byc != nullptr && mainFretsInstance != nullptr) {
                    old_Main_Frets_byc(mainFretsInstance, i, false); // Release visual fret cleanly!
                }
            }
        }
    }

    // Auto Use Special periodic frame check
    CheckAndTriggerAutoSpecial();
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
// Instant Full Special Energy Feature (Toggle: OFF by default)
// =========================================================================
// 16. Hook Main_Special::bzn (RVA: 0x20b92c0)
// Called whenever any note is hit to process Special / Star Power phrase progress
void (*old_Main_Special_bzn)(void *instance, void *hm) = nullptr;
void hook_Main_Special_bzn(void *instance, void *hm) {
    if (instance != nullptr) {
        mainSpecialInstance = instance;
    }

    if (g_instantFullSpecialEnergy.load() && instance != nullptr && hm != nullptr) {
        void *sp = *(void **)((uintptr_t)hm + 0x30);
        if (sp != nullptr && (uintptr_t)sp > 0x10000) {
            bool starNote = *(bool *)((uintptr_t)sp + 0x10);
            if (starNote) {
                // Instantly complete phrase on this single hit
                int32_t seq = *(int32_t *)((uintptr_t)sp + 0x14);
                *(int32_t *)((uintptr_t)instance + 0x34) = seq; // currentSequence
                *(int32_t *)((uintptr_t)instance + 0x38) = 0;   // currentHitsInPhrase = 0
                *(int32_t *)((uintptr_t)sp + 0x18) = 1;         // sequenceSize = 1

                // Pre-fill energy to 3 so native addition (+1) reaches 4 (100% full)
                if (lx_op_Implicit_from_int != nullptr) {
                    *(void **)((uintptr_t)instance + 0x40) = lx_op_Implicit_from_int(3, nullptr);
                }
            }
        }
    }

    if (old_Main_Special_bzn != nullptr) {
        old_Main_Special_bzn(instance, hm);
    }

    if (g_instantFullSpecialEnergy.load() && instance != nullptr && hm != nullptr) {
        void *sp = *(void **)((uintptr_t)hm + 0x30);
        if (sp != nullptr && (uintptr_t)sp > 0x10000) {
            bool starNote = *(bool *)((uintptr_t)sp + 0x10);
            if (starNote) {
                // Guarantee 100% full energy (4 bars) and activate button
                if (lx_op_Implicit_from_int != nullptr) {
                    *(void **)((uintptr_t)instance + 0x40) = lx_op_Implicit_from_int(4, nullptr);
                }
                *(bool *)((uintptr_t)instance + 0x20) = true; // spShow = true

                bool active = *(bool *)((uintptr_t)instance + 0x21);
                if (!active) {
                    void *button = *(void **)((uintptr_t)instance + 0xb8);
                    if (button != nullptr && (uintptr_t)button > 0x10000 && GameObject_SetActive != nullptr) {
                        GameObject_SetActive(button, true, nullptr);
                    }
                }
                LogDebug("Instant Full Special Energy: Star note hit! Energy filled to 4/4 (100%%) instantly.");
            }
        }
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

    // Resolve Fret Fire Particle Effect & Unity ParticleSystem (from commit e4947d)
    Main_Frets_byh = reinterpret_cast<void *(*)(void *, int32_t)>(base + 0x1f9d290);
    ParticleSystem_Play = reinterpret_cast<void (*)(void *, void *)>(base + 0x3d05c0c);
    LogDebug("Resolve Fret Fire: byh=%s, PS_Play=%s",
             Main_Frets_byh != nullptr ? "OK" : "FAIL",
             ParticleSystem_Play != nullptr ? "OK" : "FAIL");


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

    // 15. Hook Main_Notes::Add (0x20b8570) - Real-time Note Duration Recorder
    HOOK("0x20b8570", hook_Main_Notes_Add, old_Main_Notes_Add);
    LogDebug("Hook Main_Notes::Add (0x20b8570): %s", old_Main_Notes_Add != nullptr ? "SUCCESS" : "FAILED");

    // 16. Hook Main_Special::bzn (0x20b92c0) - Instant Full Special Energy on Star Note Hit
    HOOK("0x20b92c0", hook_Main_Special_bzn, old_Main_Special_bzn);
    LogDebug("Hook Main_Special::bzn (0x20b92c0): %s", old_Main_Special_bzn != nullptr ? "SUCCESS" : "FAILED");

    // Resolve Unity GameObject, ObscuredInt & Special Power activation methods
    GameObject_SetActive = reinterpret_cast<void (*)(void *, bool, void *)>(base + 0x3caddcc);
    lx_op_Implicit_from_int = reinterpret_cast<void *(*)(int32_t, void *)>(base + 0x210d21c);
    lx_op_Implicit_to_int = reinterpret_cast<int32_t (*)(void *, void *)>(base + 0x210bc94);
    Main_Special_bzk = reinterpret_cast<void (*)(void *, bool)>(base + 0x20bae98);
    LogDebug("Resolve Special Energy methods: SetActive=%s, from_int=%s, to_int=%s, bzk=%s",
             GameObject_SetActive != nullptr ? "OK" : "FAIL",
             lx_op_Implicit_from_int != nullptr ? "OK" : "FAIL",
             lx_op_Implicit_to_int != nullptr ? "OK" : "FAIL",
             Main_Special_bzk != nullptr ? "OK" : "FAIL");

    // Hook ed::bab for song detection
    HOOK("0x1c31de0", hook_ed_bab, old_ed_bab);
    LogDebug("Resolve ed methods: Hook bab=%s",
             old_ed_bab != nullptr ? "SUCCESS" : "FAILED");

    LogDebug("All Guitar Hero Real-Time, Auto Play, Song Unlock & Chart Analysis hooks installed successfully!");

#else 
    LogDebug("ARM32 architecture not supported for this 64-bit target");
#endif

    return NULL;
}

// =========================================================================
// JNI Method Implementations
// =========================================================================

// Returns [green, red, yellow, blue, orange, totalHits, totalMisses, totalCreated, totalAutoHits, autoPlay, currentCombo, currentScore]
jintArray GetNoteStats(JNIEnv *env, jobject thiz) {
    jintArray result = env->NewIntArray(12);
    if (!result) return nullptr;

    int32_t totalHits = getNotesHit();
    if (g_autoPlay.load() && totalHits > g_totalAutoHits.load()) {
        g_totalAutoHits = totalHits;
    }

    jint fill[12];
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
    fill[10] = g_currentCombo.load();
    fill[11] = g_currentScore.load();

    env->SetIntArrayRegion(result, 0, 12, fill);
    return result;
}

// Returns JSON string with chart info, special notes, optimal path, and potential score
jstring GetChartDetails(JNIEnv *env, jobject thiz) {
    if (g_chartNeedReanalysis.load() || !g_chartAnalyzed.load()) {
        AnalyzeChartAndCalculateMetrics();
    }

    pthread_mutex_lock(&g_chartAnalysisMutex);

    int32_t currentHits = getNotesHit();
    size_t stepIdx = 0;
    while (stepIdx < g_activePathMilestones.size() && currentHits >= g_activePathMilestones[stepIdx].targetHit) {
        stepIdx++;
    }

    int32_t nextTargetHit = 0;
    int32_t nextReqEnergy = 0;
    if (stepIdx < g_activePathMilestones.size()) {
        nextTargetHit = g_activePathMilestones[stepIdx].targetHit;
        nextReqEnergy = g_activePathMilestones[stepIdx].requiredEnergy;
    }

    char jsonBuf[2048];
    snprintf(jsonBuf, sizeof(jsonBuf),
        "{"
        "\"hasData\":%s,"
        "\"title\":\"%s\","
        "\"duration\":%.1f,"
        "\"durationStr\":\"%s\","
        "\"totalNotes\":%d,"
        "\"spNotes\":%d,"
        "\"spPhrases\":%d,"
        "\"bestScore\":%lld,"
        "\"baseScore\":%lld,"
        "\"pathSummary\":\"%s\","
        "\"stepIndex\":%zu,"
        "\"stepTotal\":%zu,"
        "\"nextTargetHit\":%d,"
        "\"nextReqEnergy\":%d,"
        "\"currentHits\":%d"
        "}",
        g_chartHasData ? "true" : "false",
        EscapeJsonString(g_songTitle).c_str(),
        g_songDuration,
        g_songDurationStr.c_str(),
        g_totalChartNotes,
        g_totalSpNotes,
        g_totalSpPhrases,
        (long long)g_bestPotentialScore,
        (long long)g_baseFcScore,
        g_pathSummary.c_str(),
        stepIdx,
        g_activePathMilestones.size(),
        nextTargetHit,
        nextReqEnergy,
        currentHits
    );

    pthread_mutex_unlock(&g_chartAnalysisMutex);

    return env->NewStringUTF(jsonBuf);
}

jobjectArray GetFeatureList(JNIEnv *env, jobject context) {
    jobjectArray ret;

    const char *features[] = {
            OBFUSCATE("Category_⚡ Automation"),
            OBFUSCATE("10_Toggle_Auto Play"),
            OBFUSCATE("11_Toggle_Auto Activate Special (Optimal Route)"),

            OBFUSCATE("Category_⭐ Special & Multiplier"),
            OBFUSCATE("12_Toggle_Instant Full Special Energy"),

            OBFUSCATE("Category_🎵 Songs & Unlocks"),
            OBFUSCATE("20_Toggle_Unlock All Songs"),

            OBFUSCATE("Category_Real-Time Note Counter"),
            OBFUSCATE("0_Toggle_Floating Note & Chart HUD"),
            OBFUSCATE("1_Toggle_Count Fret Errors as Miss"),
            OBFUSCATE("2_Button_Check Live Stats (Toast)"),
            OBFUSCATE("3_Button_Reset Note Counters"),
            OBFUSCATE("4_Button_Check Debug Log Path"),

            OBFUSCATE("Category_HUD & Color Legend"),
            OBFUSCATE("RichTextView_🟢 Green (0) | 🔴 Red (1) | 🟡 Yellow (2)<br>🔵 Blue (3) | 🟠 Orange (4)<br><small>⚡ Auto Play: 100% akurat tepat di lingkaran.<br>⭐ Auto Special: Otomatis aktivasi SP di rute terbaik.<br>⚡ Instant Special: Energi langsung 100% saat note bintang kena.<br>🎵 Unlock Songs: Buka seluruh lagu & akses VIP.<br>📊 Chart HUD: Info lagu, note special, rute optimal, & skor maks.</small>"),
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
            if (!boolean) {
                for (int i = 0; i < 5; i++) {
                    g_fretVisualTimer[i] = 0;
                    g_fretSustaining[i] = false;
                    g_fretSustainStartTime[i] = 0.0f;
                    if (old_Main_Frets_byc != nullptr && mainFretsInstance != nullptr) {
                        old_Main_Frets_byc(mainFretsInstance, i, false);
                    }
                }
            }
            LogDebug("Auto Play toggled: %d", boolean);
            Toast(env, obj, boolean ? OBFUSCATE("⚡ Auto Play: AKTIF") : OBFUSCATE("⚡ Auto Play: NONAKTIF"), ToastLength::LENGTH_SHORT);
            break;

        // Auto Activate Special (Optimal Route) Feature (OFF by default)
        case 11:
            g_autoUseSpecial = boolean;
            LogDebug("Auto Activate Special toggled: %d", boolean);
            Toast(env, obj, boolean ? OBFUSCATE("⭐ Auto Activate Special: AKTIF") : OBFUSCATE("⭐ Auto Activate Special: NONAKTIF"), ToastLength::LENGTH_SHORT);
            break;

        // Instant Full Special Energy Feature (OFF by default)
        case 12:
            g_instantFullSpecialEnergy = boolean;
            LogDebug("Instant Full Special Energy toggled: %d", boolean);
            Toast(env, obj, boolean ? OBFUSCATE("⭐ Instant Full Special Energy: AKTIF") : OBFUSCATE("⭐ Instant Full Special Energy: NONAKTIF"), ToastLength::LENGTH_SHORT);
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
            int32_t miss = getNotesMiss();
            int32_t total = getNotesTotalCreated();
            int32_t autoTotal = g_totalAutoHits.load();
            int32_t combo = g_currentCombo.load();

            int32_t energyBars = 0;
            if (mainSpecialInstance != nullptr && lx_op_Implicit_to_int != nullptr) {
                void *epe = *(void **)((uintptr_t)mainSpecialInstance + 0x40);
                if (epe != nullptr) {
                    energyBars = lx_op_Implicit_to_int(epe, nullptr);
                }
            }

            char statsMsg[512];
            snprintf(statsMsg, sizeof(statsMsg),
                     "LIVE STATS:\n🟢 G:%d 🔴 R:%d 🟡 Y:%d 🔵 B:%d 🟠 O:%d\n🎯 Total Hit: %d | ⚡ Auto Hit: %d\n🔥 Combo: %d | ⭐ Energy: %d/4\n❌ Miss: %d | 📋 Total Notes: %d",
                     green, red, yellow, blue, orange, hit, autoTotal, combo, energyBars, miss, total);

            LogDebug("User clicked Live Stats: Hit=%d, Auto=%d, Combo=%d, Energy=%d, Miss=%d",
                     hit, autoTotal, combo, energyBars, miss);
            Toast(env, obj, statsMsg, ToastLength::LENGTH_LONG);
            break;
        }

        case 3: // Button: Reset Note Counters
            resetAllNoteCounters();
            Toast(env, obj, OBFUSCATE("Semua counter note & auto hit di-reset ke 0!"), ToastLength::LENGTH_SHORT);
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
            {OBFUSCATE("GetChartDetails"),  OBFUSCATE("()Ljava/lang/String;"), reinterpret_cast<void *>(GetChartDetails)},
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
