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
#include <string>
#include <mutex>
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

static char g_resolvedLogPath[512] = "";
static bool g_logPathResolved = false;

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
    if (!g_logPathResolved) {
        for (const char *dir : CANDIDATE_LOG_DIRS) {
            createDirectoryRecursive(dir);
            char filePath[512];
            snprintf(filePath, sizeof(filePath), "%s/guitar_hero_debug.log", dir);
            FILE *fp = fopen(filePath, "a");
            if (fp != nullptr) {
                snprintf(g_resolvedLogPath, sizeof(g_resolvedLogPath), "%s", filePath);
                g_logPathResolved = true;
                fprintf(fp, "[%s] %s\n", timeBuf, message);
                fflush(fp);
                fclose(fp);
                break;
            }
        }
    } else {
        FILE *fp = fopen(g_resolvedLogPath, "a");
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

// Standalone Set Score Feature State (Client-Side)
static std::atomic<bool> g_setScoreEnabled{false}; // Standalone toggle for Set Score
static std::atomic<int32_t> g_targetScore{0};      // Target Score value (> 0)
static std::atomic<bool> g_scoreFrozen{false};     // True when score reaches target or is client-frozen

// Pointers to active game instances
static void *mainInstance = nullptr;        // Pointer to Main (MonoBehaviour)
static void *mainNotesInstance = nullptr;   // Pointer to Main_Notes
static void *mainScorerInstance = nullptr;  // Pointer to Main_Scorer
static void *mainFretsInstance = nullptr;   // Pointer to Main_Frets
static void *g_chartInstance = nullptr;     // Pointer to ed (Chart Manager)
static void *mainSpecialInstance = nullptr; // Pointer to Main_Special
static void *mainMultiplierInstance = nullptr; // Pointer to Main_Multiplier
static void *g_gcInstance = nullptr;         // Pointer to gc (GameController / Song List)
void *(*old_gc_blx)(void *instance, int32_t index) = nullptr; // RVA: 0x1eaa984 - Song getter by index
int32_t (*gc_bma)(void *instance) = nullptr;  // RVA: 0x1eaa9dc - Song count

// =========================================================================
// Official Game Ranking Recording & Metadata System
// =========================================================================
struct RecordedRankingScore {
    bool hasData = false;
    int32_t songId = 0;
    std::string songTitle;
    std::string artist;
    std::string difficulty;
    int32_t score = 0;
    int32_t combo = 0;
    int32_t accuracy = 0;
    int32_t hits = 0;
    int32_t misses = 0;
    int32_t erradas = 0;
    bool setScoreApplied = false;
    int32_t targetScore = 0;
    bool autoPlayUsed = false;
    bool submittedOnline = false;
    std::string recordTime;
};

static RecordedRankingScore g_lastRankingRecord;
static std::mutex g_rankingRecordMutex;
static int32_t g_totalRankingSubmissions = 0;

// Active song tracking during gameplay
static bool g_isSongActive = false;
static int32_t g_activeSongId = 0;
static std::string g_activeSongTitle = "";
static std::string g_activeSongArtist = "";
static std::string g_activeDifficulty = "Normal";

// Safe conversion from IL2CPP String object to std::string
static std::string Il2CppStringToStdString(void *il2cppStr) {
    if (!il2cppStr) return "";
    uintptr_t ptr = (uintptr_t)il2cppStr;
    if (ptr < 0x10000 || ptr > 0x7fffffffffULL) return "";

    int32_t length = *(int32_t *)(ptr + 0x10);
    if (length <= 0 || length > 256) return "";

    const uint16_t *chars = (const uint16_t *)(ptr + 0x14);
    std::string out;
    out.reserve(length);
    for (int32_t i = 0; i < length; i++) {
        uint16_t c = chars[i];
        if (c >= 32 && c <= 126) {
            out.push_back((char)c);
        } else if (c == 0) {
            break;
        } else if (c < 128) {
            out.push_back((char)c);
        } else {
            out.push_back((char)(0xC0 | ((c >> 6) & 0x1F)));
            out.push_back((char)(0x80 | (c & 0x3F)));
        }
    }
    return out;
}

static std::string FormatDifficulty(const std::string &rawDif) {
    if (rawDif.empty()) return "Normal";
    std::string lower = rawDif;
    for (char &c : lower) c = (char)tolower((unsigned char)c);
    if (lower.find("facil") != std::string::npos || lower.find("easy") != std::string::npos) return "Easy";
    if (lower.find("medio") != std::string::npos || lower.find("med") != std::string::npos) return "Medium";
    if (lower.find("dificil") != std::string::npos || lower.find("hard") != std::string::npos) return "Hard";
    if (lower.find("expert") != std::string::npos) return "Expert";
    return rawDif;
}

static void LookupSongMeta(int32_t songId, std::string &outTitle, std::string &outArtist) {
    if (g_gcInstance == nullptr || songId <= 0) return;
    int32_t count = 0;
    if (gc_bma != nullptr) {
        count = gc_bma(g_gcInstance);
    } else {
        void *list = *(void **)((uintptr_t)g_gcInstance + 0x20);
        if (list) count = *(int32_t *)((uintptr_t)list + 0x18);
    }
    if (count <= 0 || count > 5000) return;

    for (int i = 0; i < count; i++) {
        void *song = old_gc_blx != nullptr ? old_gc_blx(g_gcInstance, i) : nullptr;
        if (song != nullptr) {
            int32_t id = *(int32_t *)((uintptr_t)song + 0x10);
            if (id == songId) {
                void *musicaStr = *(void **)((uintptr_t)song + 0x18);
                void *bandaStr = *(void **)((uintptr_t)song + 0x20);
                if (musicaStr) {
                    std::string t = Il2CppStringToStdString(musicaStr);
                    if (!t.empty()) outTitle = t;
                }
                if (bandaStr) {
                    std::string a = Il2CppStringToStdString(bandaStr);
                    if (!a.empty()) outArtist = a;
                }
                return;
            }
        }
    }
}

// Freeze Special Feature State (Single Toggle, OFF by default)
static std::atomic<bool> g_freezeSpecial{false};
static std::atomic<bool> g_appliedFreezeSpecial{false};
static float g_frozenSpecialSongTime = -1.0f;
static void applyFreezeSpecialState(bool enable, bool force = false);

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
    g_scoreFrozen = false;

    if (mainNotesInstance != nullptr) {
        *(int32_t *)((uintptr_t)mainNotesInstance + 0x64) = 0; // acertadas
        *(int32_t *)((uintptr_t)mainNotesInstance + 0x68) = 0; // acertadasSP
        *(int32_t *)((uintptr_t)mainNotesInstance + 0x6c) = 0; // erradas
        *(int32_t *)((uintptr_t)mainNotesInstance + 0x70) = 0; // perdidas
    }
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
// Function pointers for Chart & Note inspection
static int32_t (*ed_Count)(void *instance) = nullptr;
static void *(*ed_bad)(void *instance, int32_t a) = nullptr;

// Function pointers for Obscured types (lx and lw)
static void (*lx_set_Value)(void *instance, int32_t value) = nullptr;   // RVA: 0x2116444
static int32_t (*lx_get_Value)(void *instance) = nullptr;               // RVA: 0x211655c
static void (*lw_set_Value)(void *instance, float value) = nullptr;     // RVA: 0x2115ed4
static float (*lw_get_Value)(void *instance) = nullptr;                 // RVA: 0x2115fec

// Function pointers for Audio inspection (Song Duration)
static void *(*AudioSource_get_clip)(void *as) = nullptr;               // RVA: 0x3c6f828
static float (*AudioClip_get_length)(void *clip) = nullptr;             // RVA: 0x3c6e70c

static float getSongDuration() {
    if (mainInstance == nullptr) return -1.0f;
    void *songPlayer = *(void **)((uintptr_t)mainInstance + 0x60); // gp at offset 0x60
    if (songPlayer == nullptr || (uintptr_t)songPlayer < 0x10000) return -1.0f;
    void *as = *(void **)((uintptr_t)songPlayer + 0x50); // AudioSource at offset 0x50
    if (as == nullptr || (uintptr_t)as < 0x10000) return -1.0f;
    if (AudioSource_get_clip != nullptr && AudioClip_get_length != nullptr) {
        void *clip = AudioSource_get_clip(as);
        if (clip != nullptr && (uintptr_t)clip > 0x10000) {
            return AudioClip_get_length(clip);
        }
    }
    return -1.0f;
}

// Hook Main_Notes::Add (Captures mainNotesInstance)
static void (*old_Main_Notes_Add)(void *instance, int32_t num, void *noteChart) = nullptr;
static void hook_Main_Notes_Add(void *instance, int32_t num, void *noteChart) {
    if (instance != nullptr) {
        mainNotesInstance = instance;
        void *special = *(void **)((uintptr_t)instance + 0x88);
        if (special != nullptr) {
            mainSpecialInstance = special;
        }
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

    if (g_setScoreEnabled.load() && g_targetScore.load() > 0) {
        int32_t target = g_targetScore.load();
        if (b >= target || g_scoreFrozen.load()) {
            b = target;
            g_scoreFrozen = true;
            if (mainNotesInstance != nullptr) {
                void *score_lw = *(void **)((uintptr_t)mainNotesInstance + 0x48);
                if (score_lw != nullptr && lw_set_Value != nullptr) {
                    lw_set_Value(score_lw, (float)target);
                }
                *(int32_t *)((uintptr_t)mainNotesInstance + 0x50) = target;
            }
        }
    }

    g_currentCombo = a;
    g_currentScore = b;
    if (old_Main_Scorer_cdc != nullptr) {
        return old_Main_Scorer_cdc(instance, a, b);
    }
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

    if (g_setScoreEnabled.load() && g_targetScore.load() > 0 && mainNotesInstance != nullptr) {
        int32_t target = g_targetScore.load();
        void *score_lw = *(void **)((uintptr_t)mainNotesInstance + 0x48);
        if (score_lw != nullptr && lw_get_Value != nullptr && lw_set_Value != nullptr) {
            float curScore = lw_get_Value(score_lw);
            if (curScore >= (float)target || g_scoreFrozen.load()) {
                lw_set_Value(score_lw, (float)target);
                *(int32_t *)((uintptr_t)mainNotesInstance + 0x50) = target;
                g_currentScore = target;
                g_scoreFrozen = true;
                if (mainScorerInstance != nullptr && old_Main_Scorer_cdc != nullptr) {
                    old_Main_Scorer_cdc(mainScorerInstance, g_currentCombo.load(), target);
                }
            }
        }
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
    g_currentScore = 0;
    g_scoreFrozen = false;
    g_frozenSpecialSongTime = -1.0f;
    g_appliedFreezeSpecial = false;
    mainSpecialInstance = nullptr;
    mainMultiplierInstance = nullptr;

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

        // Main.special: 0xc0
        void *special = *(void **)((uintptr_t)instance + 0xc0);
        if (special != nullptr) {
            mainSpecialInstance = special;
        }

        // Main.multiplier: 0xf8
        void *multiplier = *(void **)((uintptr_t)instance + 0xf8);
        if (multiplier != nullptr) {
            mainMultiplierInstance = multiplier;
        }

        // Main.chart: 0x58
        void *chart = *(void **)((uintptr_t)instance + 0x58);
        if (chart != nullptr) {
            g_chartInstance = chart;
        }

        // Main.settings: 0x70 (type je : MonoBehaviour)
        void *settings = *(void **)((uintptr_t)instance + 0x70);
        if (settings != nullptr && (uintptr_t)settings > 0x10000) {
            int32_t sId = *(int32_t *)((uintptr_t)settings + 0x20);
            if (sId > 0 && sId != g_activeSongId) {
                g_activeSongId = sId;
                void *difStr = *(void **)((uintptr_t)settings + 0x28);
                if (difStr) {
                    g_activeDifficulty = FormatDifficulty(Il2CppStringToStdString(difStr));
                }
                std::string title, artist;
                LookupSongMeta(sId, title, artist);
                if (!title.empty()) g_activeSongTitle = title;
                if (!artist.empty()) g_activeSongArtist = artist;
                g_isSongActive = true;
                LogDebug("[ActiveSong] Captured from Main.settings: ID=%d, Title='%s', Artist='%s', Diff='%s'",
                         sId, g_activeSongTitle.c_str(), g_activeSongArtist.c_str(), g_activeDifficulty.c_str());
            }
        }

        if (!g_mainCapturedLogged && notes != nullptr) {
            g_mainCapturedLogged = true;
            LogDebug("Captured active instances: Main=0x%lx, Frets=0x%lx, Notes=0x%lx, Scorer=0x%lx, Chart=0x%lx",
                     (unsigned long)mainInstance, (unsigned long)mainFretsInstance,
                     (unsigned long)mainNotesInstance,
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

    // Continually manage Freeze Special state transitions and per-frame lock on the Unity Main Thread
    bool desiredFreezeSpecial = g_freezeSpecial.load();
    if (mainMultiplierInstance != nullptr && mainSpecialInstance != nullptr) {
        if (g_appliedFreezeSpecial.load() != desiredFreezeSpecial) {
            g_appliedFreezeSpecial = desiredFreezeSpecial;
            applyFreezeSpecialState(desiredFreezeSpecial, true);
        }
    }

    if (desiredFreezeSpecial) {
        // Per-frame silent lock: directly maintain memory fields without audio/log overhead!
        if (mainSpecialInstance != nullptr && (uintptr_t)mainSpecialInstance > 0x10000) {
            *(bool *)((uintptr_t)mainSpecialInstance + 0x21) = true; // active
            *(bool *)((uintptr_t)mainSpecialInstance + 0x20) = true; // spShow
        }
        if (mainMultiplierInstance != nullptr && (uintptr_t)mainMultiplierInstance > 0x10000) {
            void *erh = *(void **)((uintptr_t)mainMultiplierInstance + 0x20);
            if (erh != nullptr && (uintptr_t)erh > 0x10000 && lx_set_Value != nullptr) {
                lx_set_Value(erh, 8);
            }
            *(int32_t *)((uintptr_t)mainMultiplierInstance + 0x28) = 8;
        }
    }

    // Continually ensure Set Score management & client-side freeze (Standalone)
    if (g_setScoreEnabled.load() && g_targetScore.load() > 0) {
        int32_t target = g_targetScore.load();

        float sTime = 0.0f;
        if (mainInstance != nullptr) {
            sTime = *(float *)((uintptr_t)mainInstance + 0x28);
        }
        float duration = getSongDuration();
        bool isSongFinishing = (duration > 0.0f && sTime > 0.0f && sTime >= (duration - 2.0f));

        if (mainNotesInstance != nullptr) {
            void *score_lw = *(void **)((uintptr_t)mainNotesInstance + 0x48);
            if (score_lw != nullptr && lw_get_Value != nullptr && lw_set_Value != nullptr) {
                float curScore = lw_get_Value(score_lw);
                if (curScore >= (float)target || isSongFinishing) {
                    lw_set_Value(score_lw, (float)target);
                    *(int32_t *)((uintptr_t)mainNotesInstance + 0x50) = target;
                    g_currentScore = target;
                    g_scoreFrozen = true;

                    if (mainScorerInstance != nullptr && old_Main_Scorer_cdc != nullptr) {
                        old_Main_Scorer_cdc(mainScorerInstance, g_currentCombo.load(), target);
                    }
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
    if (g_gcInstance != nullptr && old_gc_blx != nullptr) {
        void *song = old_gc_blx(g_gcInstance, a);
        if (song != nullptr) {
            g_activeSongId = *(int32_t *)((uintptr_t)song + 0x10);
            void *musicaStr = *(void **)((uintptr_t)song + 0x18);
            void *bandaStr = *(void **)((uintptr_t)song + 0x20);
            if (musicaStr) g_activeSongTitle = Il2CppStringToStdString(musicaStr);
            if (bandaStr) g_activeSongArtist = Il2CppStringToStdString(bandaStr);
            g_isSongActive = true;
            LogDebug("[SetlistLaunch] Selected song index %d: ID=%d, Title='%s', Artist='%s'",
                     a, g_activeSongId, g_activeSongTitle.c_str(), g_activeSongArtist.c_str());
        }
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

// =========================================================================
// Freeze Special & Permanent x8 Multiplier System (Client-Side)
// =========================================================================

// Function pointer for Main_Special::bzm (Activate Special method)
static void (*Main_Special_bzm)(void *instance) = nullptr;              // RVA: 0x20bb388

// 16. Hook Main_Special::bzt (RVA: 0x20bb658) - Duration drain / frame timer
void (*old_Main_Special_bzt)(void *instance, float a) = nullptr;
void hook_Main_Special_bzt(void *instance, float a) {
    if (instance != nullptr) {
        mainSpecialInstance = instance;
    }

    if (g_freezeSpecial.load() && instance != nullptr) {
        *(bool *)((uintptr_t)instance + 0x21) = true; // active = true
        *(bool *)((uintptr_t)instance + 0x20) = true; // spShow = true

        void *eph = *(void **)((uintptr_t)instance + 0x58);
        if (eph != nullptr && lw_set_Value != nullptr) {
            lw_set_Value(eph, 100.0f);
        }
        void *epi = *(void **)((uintptr_t)instance + 0x60);
        if (epi != nullptr && lw_set_Value != nullptr) {
            lw_set_Value(epi, 100.0f);
        }

        if (g_frozenSpecialSongTime < 0.0f) {
            g_frozenSpecialSongTime = a;
        }

        // Pass frozen timestamp to prevent duration decay & expiration
        if (old_Main_Special_bzt != nullptr) {
            old_Main_Special_bzt(instance, g_frozenSpecialSongTime);
        }
        return;
    } else {
        g_frozenSpecialSongTime = -1.0f;
    }

    if (old_Main_Special_bzt != nullptr) {
        old_Main_Special_bzt(instance, a);
    }
}

// 17. Hook Main_Special::bzv (RVA: 0x20b9708) - Hit note special multiplier check
bool (*old_Main_Special_bzv)(void *instance, float a) = nullptr;
bool hook_Main_Special_bzv(void *instance, float a) {
    if (instance != nullptr) {
        mainSpecialInstance = instance;
    }
    if (g_freezeSpecial.load()) {
        return true; // Special is permanently active for hit score & multiplier!
    }
    if (old_Main_Special_bzv != nullptr) {
        return old_Main_Special_bzv(instance, a);
    }
    return false;
}

// 18. Hook Main_Special::bzw (RVA: 0x20bb950) - Special active query
bool (*old_Main_Special_bzw)(void *instance) = nullptr;
bool hook_Main_Special_bzw(void *instance) {
    if (instance != nullptr) {
        mainSpecialInstance = instance;
    }
    if (g_freezeSpecial.load()) {
        return true;
    }
    if (old_Main_Special_bzw != nullptr) {
        return old_Main_Special_bzw(instance);
    }
    return false;
}

// 19. Hook Main_Multiplier::cch (RVA: 0x20c2070) - Multiplier tier & value calculation
void (*old_Main_Multiplier_cch)(void *instance, int32_t a, bool b) = nullptr;
void hook_Main_Multiplier_cch(void *instance, int32_t a, bool b) {
    if (instance != nullptr) {
        mainMultiplierInstance = instance;
    }

    if (g_freezeSpecial.load()) {
        a = 30;     // Combo tier 30+ (4x base)
        b = true;   // Special active (4x * 2 = 8x)
    }

    if (old_Main_Multiplier_cch != nullptr) {
        old_Main_Multiplier_cch(instance, a, b);
    }

    if (g_freezeSpecial.load() && instance != nullptr) {
        void *erh = *(void **)((uintptr_t)instance + 0x20);
        if (erh != nullptr && lx_set_Value != nullptr) {
            lx_set_Value(erh, 8);
        }
        *(int32_t *)((uintptr_t)instance + 0x28) = 8;
    }
}

// 20. Hook Main_Multiplier::Special (RVA: 0x20c22e4) - Special visual mode on multiplier
void (*old_Main_Multiplier_Special)(void *instance, bool on) = nullptr;
void hook_Main_Multiplier_Special(void *instance, bool on) {
    if (instance != nullptr) {
        mainMultiplierInstance = instance;
    }
    if (g_freezeSpecial.load()) {
        on = true; // Always lock special visual ON
    }
    if (old_Main_Multiplier_Special != nullptr) {
        old_Main_Multiplier_Special(instance, on);
    }
}

// 21. Hook Main_Multiplier::cci (RVA: 0x20c249c) - Multiplier reset suppressor
void (*old_Main_Multiplier_cci)(void *instance) = nullptr;
void hook_Main_Multiplier_cci(void *instance) {
    if (instance != nullptr) {
        mainMultiplierInstance = instance;
    }
    if (g_freezeSpecial.load()) {
        if (old_Main_Multiplier_Special != nullptr) {
            old_Main_Multiplier_Special(instance, true);
        }
        if (old_Main_Multiplier_cch != nullptr) {
            old_Main_Multiplier_cch(instance, 30, true);
        }
        return;
    }
    if (old_Main_Multiplier_cci != nullptr) {
        old_Main_Multiplier_cci(instance);
    }
}

static void applyFreezeSpecialState(bool enable, bool force) {
    if (!force && g_freezeSpecial.load() == enable) {
        return;
    }

    g_freezeSpecial = enable;
    g_frozenSpecialSongTime = -1.0f;
    LogDebug("[FreezeSpecial] State changed: %s", enable ? "ENABLED (x8 Locked)" : "DISABLED");

    if (enable) {
        if (mainSpecialInstance != nullptr && (uintptr_t)mainSpecialInstance > 0x10000) {
            bool wasActive = *(bool *)((uintptr_t)mainSpecialInstance + 0x21);
            *(bool *)((uintptr_t)mainSpecialInstance + 0x21) = true; // active
            *(bool *)((uintptr_t)mainSpecialInstance + 0x20) = true; // spShow

            void *eph = *(void **)((uintptr_t)mainSpecialInstance + 0x58);
            if (eph != nullptr && (uintptr_t)eph > 0x10000 && lw_set_Value != nullptr) {
                lw_set_Value(eph, 100.0f);
            }
            void *epi = *(void **)((uintptr_t)mainSpecialInstance + 0x60);
            if (epi != nullptr && (uintptr_t)epi > 0x10000 && lw_set_Value != nullptr) {
                lw_set_Value(epi, 100.0f);
            }

            // Only trigger ActivateSpecial audio/animation if not already active!
            if (!wasActive && Main_Special_bzm != nullptr) {
                Main_Special_bzm(mainSpecialInstance);
            }
        }

        if (mainMultiplierInstance != nullptr && (uintptr_t)mainMultiplierInstance > 0x10000) {
            if (old_Main_Multiplier_Special != nullptr) {
                old_Main_Multiplier_Special(mainMultiplierInstance, true);
            }
            if (old_Main_Multiplier_cch != nullptr) {
                old_Main_Multiplier_cch(mainMultiplierInstance, 30, true);
            }
            void *erh = *(void **)((uintptr_t)mainMultiplierInstance + 0x20);
            if (erh != nullptr && (uintptr_t)erh > 0x10000 && lx_set_Value != nullptr) {
                lx_set_Value(erh, 8);
            }
            *(int32_t *)((uintptr_t)mainMultiplierInstance + 0x28) = 8;
        }
    } else {
        if (mainSpecialInstance != nullptr && (uintptr_t)mainSpecialInstance > 0x10000) {
            *(bool *)((uintptr_t)mainSpecialInstance + 0x21) = false;
            *(bool *)((uintptr_t)mainSpecialInstance + 0x20) = false;
        }
        if (mainMultiplierInstance != nullptr && (uintptr_t)mainMultiplierInstance > 0x10000) {
            if (old_Main_Multiplier_Special != nullptr) {
                old_Main_Multiplier_Special(mainMultiplierInstance, false);
            }
            if (old_Main_Multiplier_cch != nullptr) {
                int32_t combo = g_currentCombo.load();
                old_Main_Multiplier_cch(mainMultiplierInstance, combo, false);
            }
        }
    }
}

// =========================================================================
// Standalone Set Score & Final Screen 100% FC Accuracy Enforcement System
// =========================================================================

// 22. Hook Main::ceq (RVA: 0x20c6d70) - Song finish handler (before SongScoreInfo is generated)
void (*old_Main_ceq)(void *instance) = nullptr;
void hook_Main_ceq(void *instance) {
    if (instance != nullptr) {
        mainInstance = instance;
    }
    g_isSongActive = false;

    if (g_setScoreEnabled.load() && g_targetScore.load() > 0) {
        int32_t target = g_targetScore.load();
        if (mainNotesInstance != nullptr) {
            void *score_lw = *(void **)((uintptr_t)mainNotesInstance + 0x48);
            if (score_lw != nullptr && lw_set_Value != nullptr) {
                lw_set_Value(score_lw, (float)target);
            }
            *(int32_t *)((uintptr_t)mainNotesInstance + 0x50) = target; // scorePrevious
        }
        g_currentScore = target;
        g_scoreFrozen = true;
        LogDebug("[SetScore] Main::ceq invoked, song completed! Score client-frozen to %d", target);
    }

    // 100% FC & Accuracy Pre-Calculation for Result Screen:
    // If Auto Play is active or player had 0 misses, ensure Main_Notes counters match total chart notes!
    if (mainNotesInstance != nullptr) {
        int32_t hits = *(int32_t *)((uintptr_t)mainNotesInstance + 0x64);
        int32_t perdidas = *(int32_t *)((uintptr_t)mainNotesInstance + 0x70);
        int32_t totalChart = (g_chartInstance != nullptr && ed_Count != nullptr) ? ed_Count(g_chartInstance) : hits;

        if (g_autoPlay.load() || (perdidas == 0 && hits > 0)) {
            if (totalChart > hits) {
                *(int32_t *)((uintptr_t)mainNotesInstance + 0x60) = totalChart; // comboMax (0x60)
                *(int32_t *)((uintptr_t)mainNotesInstance + 0x64) = totalChart; // acertadas (0x64)
            }
            *(int32_t *)((uintptr_t)mainNotesInstance + 0x6c) = 0; // erradas = 0
            *(int32_t *)((uintptr_t)mainNotesInstance + 0x70) = 0; // perdidas = 0
            LogDebug("[ResultScreen] Pre-ceq notes updated to full combo: %d notes", totalChart > hits ? totalChart : hits);
        }
    }

    if (old_Main_ceq != nullptr) {
        old_Main_ceq(instance);
    }
}

// 23. Hook Main_FinalScreen::bwo (RVA: 0x1f99040) - Final result screen score display
void (*old_Main_FinalScreen_bwo)(void *instance, void *songTitle, bool b, void *songScoreInfo) = nullptr;
void hook_Main_FinalScreen_bwo(void *instance, void *songTitle, bool b, void *songScoreInfo) {
    if (songTitle != nullptr) {
        std::string t = Il2CppStringToStdString(songTitle);
        if (!t.empty()) {
            g_activeSongTitle = t;
        }
    }

    if (songScoreInfo != nullptr) {
        // Standalone Set Score logic
        if (g_setScoreEnabled.load() && g_targetScore.load() > 0) {
            int32_t target = g_targetScore.load();
            void *pts_lx = *(void **)((uintptr_t)songScoreInfo + 0x20);
            if (pts_lx != nullptr && lx_set_Value != nullptr) {
                lx_set_Value(pts_lx, target);
                LogDebug("[SetScore] Main_FinalScreen::bwo: pts forced to %d", target);
            }
            g_currentScore = target;
            g_scoreFrozen = true;
        }

        // 100% FC / Accuracy Fix for Result Screen:
        // Solves the 99% bug where chart count was slightly higher than hit count (e.g. 1043/1044 = 99%)
        int32_t acertadas = *(int32_t *)((uintptr_t)songScoreInfo + 0x30);
        int32_t perdidas = *(int32_t *)((uintptr_t)songScoreInfo + 0x3c);
        int32_t combo = *(int32_t *)((uintptr_t)songScoreInfo + 0x28);
        int32_t totalChart = (g_chartInstance != nullptr && ed_Count != nullptr) ? ed_Count(g_chartInstance) : acertadas;

        if (g_autoPlay.load() || (perdidas == 0 && acertadas > 0)) {
            if (totalChart > acertadas) acertadas = totalChart;
            if (totalChart > combo) combo = totalChart;

            *(int32_t *)((uintptr_t)songScoreInfo + 0x30) = acertadas; // Notes Hit (acertadas)
            *(int32_t *)((uintptr_t)songScoreInfo + 0x28) = combo;     // Max Combo (combo)
            *(int32_t *)((uintptr_t)songScoreInfo + 0x38) = 0;         // Overstrums (erradas = 0)
            *(int32_t *)((uintptr_t)songScoreInfo + 0x3c) = 0;         // Missed Notes (perdidas = 0)
            *(int32_t *)((uintptr_t)songScoreInfo + 0x2c) = 100;       // Accuracy (porcentagem = 100% FC!)
            LogDebug("[ResultScreen] Enforced 100%% FC in bwo: acertadas=%d, combo=%d, porcentagem=100", acertadas, combo);
        }
    }

    if (old_Main_FinalScreen_bwo != nullptr) {
        old_Main_FinalScreen_bwo(instance, songTitle, b, songScoreInfo);
    }
}

// 24. Hook jd::cey (RVA: 0x20e18a4) - Score saving handler
void (*old_jd_cey)(void *instance, void *songScoreInfo, void *finalScreen, bool c) = nullptr;
void hook_jd_cey(void *instance, void *songScoreInfo, void *finalScreen, bool c) {
    if (songScoreInfo != nullptr) {
        // Standalone Set Score logic
        if (g_setScoreEnabled.load() && g_targetScore.load() > 0) {
            int32_t target = g_targetScore.load();
            void *pts_lx = *(void **)((uintptr_t)songScoreInfo + 0x20);
            if (pts_lx != nullptr && lx_set_Value != nullptr) {
                lx_set_Value(pts_lx, target);
                LogDebug("[SetScore] jd::cey: pts forced to %d", target);
            }
            g_currentScore = target;
            g_scoreFrozen = true;
        }

        // 100% FC / Accuracy Fix when saving
        int32_t acertadas = *(int32_t *)((uintptr_t)songScoreInfo + 0x30);
        int32_t perdidas = *(int32_t *)((uintptr_t)songScoreInfo + 0x3c);
        int32_t combo = *(int32_t *)((uintptr_t)songScoreInfo + 0x28);
        int32_t totalChart = (g_chartInstance != nullptr && ed_Count != nullptr) ? ed_Count(g_chartInstance) : acertadas;

        if (g_autoPlay.load() || (perdidas == 0 && acertadas > 0)) {
            if (totalChart > acertadas) acertadas = totalChart;
            if (totalChart > combo) combo = totalChart;

            *(int32_t *)((uintptr_t)songScoreInfo + 0x30) = acertadas;
            *(int32_t *)((uintptr_t)songScoreInfo + 0x28) = combo;
            *(int32_t *)((uintptr_t)songScoreInfo + 0x38) = 0;
            *(int32_t *)((uintptr_t)songScoreInfo + 0x3c) = 0;
            *(int32_t *)((uintptr_t)songScoreInfo + 0x2c) = 100;
        }
    }

    if (old_jd_cey != nullptr) {
        old_jd_cey(instance, songScoreInfo, finalScreen, c);
    }
}

// 25. Hook lk::cxq (RVA: 0x210b7f0) - Anti-cheat score calculation validator
bool (*old_lk_cxq)(void *instance, void *songScoreInfo) = nullptr;
bool hook_lk_cxq(void *instance, void *songScoreInfo) {
    if (g_setScoreEnabled.load() && g_targetScore.load() > 0) {
        LogDebug("[SetScore] lk::cxq: Bypassing score calculation validation for target score %d", g_targetScore.load());
        return true;
    }
    if (old_lk_cxq != nullptr) {
        return old_lk_cxq(instance, songScoreInfo);
    }
    return true;
}

// 26. Hook lk::cxt (RVA: 0x210b99c) - Previous best score getter
int32_t (*old_lk_cxt)(void *instance, int32_t a, void *b) = nullptr;
int32_t hook_lk_cxt(void *instance, int32_t a, void *b) {
    if (g_setScoreEnabled.load() && g_targetScore.load() > 0) {
        // Return 0 so that cxm ALWAYS treats target score as a new high score record
        // and guarantees that score is saved both locally and submitted to online ranking!
        LogDebug("[SetScore] lk::cxt: Returning 0 so target score %d is always saved as new record", g_targetScore.load());
        return 0;
    }
    if (old_lk_cxt != nullptr) {
        return old_lk_cxt(instance, a, b);
    }
    return 0;
}

// Function pointer for lk::cxn (RVA: 0x210c18c) - Direct ranking submission
static void (*lk_cxn)(void *instance, void *songScoreInfo) = nullptr;

// 27. Hook lk::cxm (RVA: 0x210b4f8) - Main score saving & ranking submission coordinator
void (*old_lk_cxm)(void *instance, void *songScoreInfo, bool b) = nullptr;
void hook_lk_cxm(void *instance, void *songScoreInfo, bool b) {
    if (songScoreInfo != nullptr) {
        if (g_setScoreEnabled.load() && g_targetScore.load() > 0) {
            int32_t target = g_targetScore.load();
            void *pts_lx = *(void **)((uintptr_t)songScoreInfo + 0x20);
            if (pts_lx != nullptr && lx_set_Value != nullptr) {
                lx_set_Value(pts_lx, target);
                LogDebug("[SetScore] lk::cxm: pts forced to %d", target);
            }
            g_currentScore = target;
            g_scoreFrozen = true;

            int32_t acertadas = *(int32_t *)((uintptr_t)songScoreInfo + 0x30);
            if (acertadas > 0) {
                int32_t currentSP = *(int32_t *)((uintptr_t)songScoreInfo + 0x34);
                if (target > 20 * acertadas) {
                    int32_t neededSP = (target - 20 * acertadas) / 20 + 1;
                    if (neededSP > acertadas) neededSP = acertadas;
                    if (neededSP > currentSP) {
                        *(int32_t *)((uintptr_t)songScoreInfo + 0x34) = neededSP;
                    }
                    float neededRastros = (float)(target - (40 * neededSP + 20 * (acertadas - neededSP)) + 500);
                    if (neededRastros > *(float *)((uintptr_t)songScoreInfo + 0x40)) {
                        *(float *)((uintptr_t)songScoreInfo + 0x40) = neededRastros;
                    }
                }
            }
        }

        // 100% FC / Accuracy Fix for Result Screen & Ranking
        int32_t acertadas = *(int32_t *)((uintptr_t)songScoreInfo + 0x30);
        int32_t perdidas = *(int32_t *)((uintptr_t)songScoreInfo + 0x3c);
        int32_t combo = *(int32_t *)((uintptr_t)songScoreInfo + 0x28);
        int32_t totalChart = (g_chartInstance != nullptr && ed_Count != nullptr) ? ed_Count(g_chartInstance) : acertadas;

        if (g_autoPlay.load() || (perdidas == 0 && acertadas > 0)) {
            if (totalChart > acertadas) acertadas = totalChart;
            if (totalChart > combo) combo = totalChart;

            *(int32_t *)((uintptr_t)songScoreInfo + 0x30) = acertadas;
            *(int32_t *)((uintptr_t)songScoreInfo + 0x28) = combo;
            *(int32_t *)((uintptr_t)songScoreInfo + 0x38) = 0;
            *(int32_t *)((uintptr_t)songScoreInfo + 0x3c) = 0;
            *(int32_t *)((uintptr_t)songScoreInfo + 0x2c) = 100;
        }
    }

    if (old_lk_cxm != nullptr) {
        old_lk_cxm(instance, songScoreInfo, b);
    }

    // Record official ranking submission data
    if (songScoreInfo != nullptr) {
        int32_t sId = *(int32_t *)((uintptr_t)songScoreInfo + 0x10);
        void *difStr = *(void **)((uintptr_t)songScoreInfo + 0x18);
        std::string dif = difStr ? FormatDifficulty(Il2CppStringToStdString(difStr)) : g_activeDifficulty;
        if (dif.empty()) dif = "Normal";

        void *pts_lx = *(void **)((uintptr_t)songScoreInfo + 0x20);
        int32_t finalScore = (pts_lx != nullptr && lx_get_Value != nullptr) ? lx_get_Value(pts_lx) : g_currentScore.load();
        if (g_setScoreEnabled.load() && g_targetScore.load() > 0) {
            finalScore = g_targetScore.load();
        }

        int32_t combo = *(int32_t *)((uintptr_t)songScoreInfo + 0x28);
        int32_t accuracy = *(int32_t *)((uintptr_t)songScoreInfo + 0x2c);
        int32_t acertadas = *(int32_t *)((uintptr_t)songScoreInfo + 0x30);
        int32_t erradas = *(int32_t *)((uintptr_t)songScoreInfo + 0x38);
        int32_t perdidas = *(int32_t *)((uintptr_t)songScoreInfo + 0x3c);

        std::string title = g_activeSongTitle;
        std::string artist = g_activeSongArtist;
        if (title.empty() && sId > 0) {
            LookupSongMeta(sId, title, artist);
        }
        if (title.empty()) {
            title = "Song #" + std::to_string(sId);
        }

        time_t now = time(nullptr);
        char timeBuf[32];
        struct tm *tm_info = localtime(&now);
        if (tm_info) {
            strftime(timeBuf, sizeof(timeBuf), "%H:%M:%S", tm_info);
        } else {
            snprintf(timeBuf, sizeof(timeBuf), "Baru Saja");
        }

        {
            std::lock_guard<std::mutex> lock(g_rankingRecordMutex);
            g_lastRankingRecord.hasData = true;
            g_lastRankingRecord.songId = sId;
            g_lastRankingRecord.songTitle = title;
            g_lastRankingRecord.artist = artist;
            g_lastRankingRecord.difficulty = dif;
            g_lastRankingRecord.score = finalScore;
            g_lastRankingRecord.combo = combo;
            g_lastRankingRecord.accuracy = accuracy;
            g_lastRankingRecord.hits = acertadas;
            g_lastRankingRecord.misses = perdidas;
            g_lastRankingRecord.erradas = erradas;
            g_lastRankingRecord.setScoreApplied = (g_setScoreEnabled.load() && g_targetScore.load() > 0);
            g_lastRankingRecord.targetScore = g_targetScore.load();
            g_lastRankingRecord.autoPlayUsed = g_autoPlay.load();
            g_lastRankingRecord.submittedOnline = true;
            g_lastRankingRecord.recordTime = timeBuf;
            g_totalRankingSubmissions++;
        }

        LogDebug("[RankingRecord] Recorded official ranking score: Song='%s' (%s), ID=%d, Score=%d, Acc=%d%%, Combo=%d, Hit=%d, Miss=%d, Time=%s",
                 title.c_str(), dif.c_str(), sId, finalScore, accuracy, combo, acertadas, perdidas, timeBuf);

        g_isSongActive = false;
    }

    // Safety net: If Set Score is active, make sure lk_cxn was triggered
    if (g_setScoreEnabled.load() && g_targetScore.load() > 0 && instance != nullptr && songScoreInfo != nullptr) {
        if (lk_cxn != nullptr) {
            LogDebug("[SetScore] Submitting score %d to ranking server via lk_cxn", g_targetScore.load());
            lk_cxn(instance, songScoreInfo);
        }
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

    // Resolve ed methods for chart lookahead inspection
    ed_Count = reinterpret_cast<int32_t (*)(void *)>(base + 0x1c31f44);
    ed_bad = reinterpret_cast<void *(*)(void *, int32_t)>(base + 0x1c31f14);

    // Resolve Obscured types (lx and lw)
    lx_set_Value = reinterpret_cast<void (*)(void *, int32_t)>(base + 0x2116444);
    lx_get_Value = reinterpret_cast<int32_t (*)(void *)>(base + 0x211655c);
    lw_set_Value = reinterpret_cast<void (*)(void *, float)>(base + 0x2115ed4);
    lw_get_Value = reinterpret_cast<float (*)(void *)>(base + 0x2115fec);

    // Resolve Main_Special::bzm (Activate Special)
    Main_Special_bzm = reinterpret_cast<void (*)(void *)>(base + 0x20bb388);

    // 16. Hook Main_Special::bzt (0x20bb658) - Duration drain / frame timer
    HOOK("0x20bb658", hook_Main_Special_bzt, old_Main_Special_bzt);
    LogDebug("Hook Main_Special::bzt (0x20bb658): %s", old_Main_Special_bzt != nullptr ? "SUCCESS" : "FAILED");

    // 17. Hook Main_Special::bzv (0x20b9708) - Hit note special multiplier check
    HOOK("0x20b9708", hook_Main_Special_bzv, old_Main_Special_bzv);
    LogDebug("Hook Main_Special::bzv (0x20b9708): %s", old_Main_Special_bzv != nullptr ? "SUCCESS" : "FAILED");

    // 18. Hook Main_Special::bzw (0x20bb950) - Special active query
    HOOK("0x20bb950", hook_Main_Special_bzw, old_Main_Special_bzw);
    LogDebug("Hook Main_Special::bzw (0x20bb950): %s", old_Main_Special_bzw != nullptr ? "SUCCESS" : "FAILED");

    // 19. Hook Main_Multiplier::cch (0x20c2070) - Multiplier value & tier calculation
    HOOK("0x20c2070", hook_Main_Multiplier_cch, old_Main_Multiplier_cch);
    LogDebug("Hook Main_Multiplier::cch (0x20c2070): %s", old_Main_Multiplier_cch != nullptr ? "SUCCESS" : "FAILED");

    // 20. Hook Main_Multiplier::Special (0x20c22e4) - Special visual mode on multiplier
    HOOK("0x20c22e4", hook_Main_Multiplier_Special, old_Main_Multiplier_Special);
    LogDebug("Hook Main_Multiplier::Special (0x20c22e4): %s", old_Main_Multiplier_Special != nullptr ? "SUCCESS" : "FAILED");

    // 21. Hook Main_Multiplier::cci (0x20c249c) - Multiplier reset suppressor
    HOOK("0x20c249c", hook_Main_Multiplier_cci, old_Main_Multiplier_cci);
    LogDebug("Hook Main_Multiplier::cci (0x20c249c): %s", old_Main_Multiplier_cci != nullptr ? "SUCCESS" : "FAILED");

    // 22. Hook Main::ceq (0x20c6d70) - Song finish handler
    HOOK("0x20c6d70", hook_Main_ceq, old_Main_ceq);
    LogDebug("Hook Main::ceq (0x20c6d70): %s", old_Main_ceq != nullptr ? "SUCCESS" : "FAILED");

    // 23. Hook Main_FinalScreen::bwo (0x1f99040) - Final score screen
    HOOK("0x1f99040", hook_Main_FinalScreen_bwo, old_Main_FinalScreen_bwo);
    LogDebug("Hook Main_FinalScreen::bwo (0x1f99040): %s", old_Main_FinalScreen_bwo != nullptr ? "SUCCESS" : "FAILED");

    // 24. Hook jd::cey (0x20e18a4) - Score saving handler
    HOOK("0x20e18a4", hook_jd_cey, old_jd_cey);
    LogDebug("Hook jd::cey (0x20e18a4): %s", old_jd_cey != nullptr ? "SUCCESS" : "FAILED");

    // Resolve lk::cxn (0x210c18c) - Direct ranking submission
    lk_cxn = reinterpret_cast<void (*)(void *, void *)>(base + 0x210c18c);
    LogDebug("Resolve lk::cxn (0x210c18c): %s", lk_cxn != nullptr ? "SUCCESS" : "FAILED");

    // 25. Hook lk::cxq (0x210b7f0) - Anti-cheat score calculation validator
    HOOK("0x210b7f0", hook_lk_cxq, old_lk_cxq);
    LogDebug("Hook lk::cxq (0x210b7f0): %s", old_lk_cxq != nullptr ? "SUCCESS" : "FAILED");

    // 26. Hook lk::cxt (0x210b99c) - Previous best score getter
    HOOK("0x210b99c", hook_lk_cxt, old_lk_cxt);
    LogDebug("Hook lk::cxt (0x210b99c): %s", old_lk_cxt != nullptr ? "SUCCESS" : "FAILED");

    // 27. Hook lk::cxm (0x210b4f8) - Main score saving & ranking submission
    HOOK("0x210b4f8", hook_lk_cxm, old_lk_cxm);
    LogDebug("Hook lk::cxm (0x210b4f8): %s", old_lk_cxm != nullptr ? "SUCCESS" : "FAILED");

    // Resolve AudioSource.get_clip & AudioClip.get_length for song duration inspection
    AudioSource_get_clip = reinterpret_cast<void *(*)(void *)>(base + 0x3c6f828);
    AudioClip_get_length = reinterpret_cast<float (*)(void *)>(base + 0x3c6e70c);
    LogDebug("Resolve Audio Duration: get_clip=%s, get_length=%s",
             AudioSource_get_clip != nullptr ? "OK" : "FAIL",
             AudioClip_get_length != nullptr ? "OK" : "FAIL");

    LogDebug("All Guitar Hero Real-Time, Auto Play, Target Score, Song Unlock & Freeze Special hooks installed successfully!");

#else 
    LogDebug("ARM32 architecture not supported for this 64-bit target");
#endif

    return NULL;
}

// =========================================================================
// JNI Method Implementations
// =========================================================================

// Returns [green, red, yellow, blue, orange, totalHits, totalMisses, totalCreated, totalAutoHits, autoPlay, currentCombo, currentScore, targetScore, isFrozen]
jintArray GetNoteStats(JNIEnv *env, jobject thiz) {
    jintArray result = env->NewIntArray(14);
    if (!result) return nullptr;

    int32_t totalHits = getNotesHit();
    if (g_autoPlay.load() && totalHits > g_totalAutoHits.load()) {
        g_totalAutoHits = totalHits;
    }

    jint fill[14];
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
    fill[12] = (g_setScoreEnabled.load() ? g_targetScore.load() : 0);
    fill[13] = g_scoreFrozen.load() ? 1 : 0;

    env->SetIntArrayRegion(result, 0, 14, fill);
    return result;
}

jobjectArray GetFeatureList(JNIEnv *env, jobject context) {
    jobjectArray ret;

    const char *features[] = {
            OBFUSCATE("Category_⚡ Automation"),
            OBFUSCATE("10_Toggle_Auto Play"),
            OBFUSCATE("11_Toggle_Freeze Special"),

            OBFUSCATE("Category_🎯 Set Score (Standalone)"),
            OBFUSCATE("12_Toggle_Enable Set Score"),
            OBFUSCATE("13_InputValue_Target Score"),
            OBFUSCATE("14_Button_Reset Target Score"),
            OBFUSCATE("15_Button_Check Ranking Score (Toast)"),

            OBFUSCATE("Category_🎵 Songs & Unlocks"),
            OBFUSCATE("20_Toggle_Unlock All Songs"),

            OBFUSCATE("Category_Real-Time Note Counter"),
            OBFUSCATE("0_Toggle_Floating Note HUD"),
            OBFUSCATE("1_Toggle_Count Fret Errors as Miss"),
            OBFUSCATE("2_Button_Check Live Stats (Toast)"),
            OBFUSCATE("3_Button_Reset Note Counters"),
            OBFUSCATE("4_Button_Check Debug Log Path"),

            OBFUSCATE("Category_HUD & Color Legend"),
            OBFUSCATE("RichTextView_🟢 Green (0) | 🔴 Red (1) | 🟡 Yellow (2)<br>🔵 Blue (3) | 🟠 Orange (4)<br><small>⚡ Auto Play: 100% akurat tepat di lingkaran (100% FC).<br>❄️ Freeze Special: Multiplier terkunci x8 & durasi beku.<br>🎯 Set Score: Fitur mandiri, kunci score sesuai input target.<br>🏆 Check Ranking: Cek status score resmi yang tercatat di game ranking.<br>🎵 Unlock Songs: Buka seluruh lagu & akses VIP.</small>"),
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

        // Freeze Special Feature (OFF by default) - Completely standalone!
        case 11:
            g_freezeSpecial = boolean;
            LogDebug("Freeze Special toggled: %d", boolean);
            Toast(env, obj, boolean ? OBFUSCATE("❄️ Freeze Special: AKTIF (x8 Multiplier)") : OBFUSCATE("❄️ Freeze Special: NONAKTIF"), ToastLength::LENGTH_SHORT);
            break;

        // Set Score Feature (Standalone Toggle)
        case 12: {
            g_setScoreEnabled = boolean;
            g_scoreFrozen = false;
            LogDebug("Set Score toggled: %d", boolean);
            if (boolean) {
                int32_t curTarget = g_targetScore.load();
                char msg[128];
                if (curTarget > 0) {
                    snprintf(msg, sizeof(msg), "🎯 Set Score: AKTIF (Target: %d)", curTarget);
                } else {
                    snprintf(msg, sizeof(msg), "🎯 Set Score: AKTIF (Silakan masukkan Target Score)");
                }
                Toast(env, obj, msg, ToastLength::LENGTH_SHORT);
            } else {
                Toast(env, obj, OBFUSCATE("🎯 Set Score: NONAKTIF"), ToastLength::LENGTH_SHORT);
            }
            break;
        }

        // Target Score Value Input (featNum 13)
        case 13: {
            g_targetScore = value;
            g_scoreFrozen = false;
            LogDebug("Target Score set: %d", value);
            char targetMsg[128];
            snprintf(targetMsg, sizeof(targetMsg), "🎯 Target Score disetel ke: %d", value);
            Toast(env, obj, targetMsg, ToastLength::LENGTH_SHORT);
            break;
        }

        // Button: Reset Target Score (featNum 14)
        case 14: {
            g_targetScore = 0;
            g_scoreFrozen = false;
            LogDebug("Target Score reset to 0 by user");
            Toast(env, obj, OBFUSCATE("🎯 Target Score di-reset ke 0!"), ToastLength::LENGTH_SHORT);
            break;
        }

        // Button: Check Ranking Score (Toast) (featNum 15)
        case 15: {
            char rankingMsg[512];
            std::lock_guard<std::mutex> lock(g_rankingRecordMutex);

            if (g_lastRankingRecord.hasData) {
                const char *modeTag = "";
                if (g_lastRankingRecord.setScoreApplied) {
                    modeTag = " [TARGET]";
                } else if (g_lastRankingRecord.autoPlayUsed) {
                    modeTag = " [100% FC]";
                }

                if (!g_lastRankingRecord.artist.empty()) {
                    snprintf(rankingMsg, sizeof(rankingMsg),
                             "🏆 SCORE RANKING RESMI:\n🎵 Lagu: %s\n🎤 Artis: %s (ID: %d)\n⭐ Diff: %s\n💎 Score: %d%s\n🎯 Akurasi: %d%% | 🔥 Combo: %d\n✅ Hit: %d | ❌ Miss: %d\n📡 Status: ✅ TERCATAT RESMI\n🕒 Waktu: %s (#%d)",
                             g_lastRankingRecord.songTitle.c_str(),
                             g_lastRankingRecord.artist.c_str(),
                             g_lastRankingRecord.songId,
                             g_lastRankingRecord.difficulty.c_str(),
                             g_lastRankingRecord.score, modeTag,
                             g_lastRankingRecord.accuracy,
                             g_lastRankingRecord.combo,
                             g_lastRankingRecord.hits,
                             g_lastRankingRecord.misses,
                             g_lastRankingRecord.recordTime.c_str(),
                             g_totalRankingSubmissions);
                } else {
                    snprintf(rankingMsg, sizeof(rankingMsg),
                             "🏆 SCORE RANKING RESMI:\n🎵 Lagu: %s (ID: %d)\n⭐ Diff: %s\n💎 Score: %d%s\n🎯 Akurasi: %d%% | 🔥 Combo: %d\n✅ Hit: %d | ❌ Miss: %d\n📡 Status: ✅ TERCATAT RESMI\n🕒 Waktu: %s (#%d)",
                             g_lastRankingRecord.songTitle.c_str(),
                             g_lastRankingRecord.songId,
                             g_lastRankingRecord.difficulty.c_str(),
                             g_lastRankingRecord.score, modeTag,
                             g_lastRankingRecord.accuracy,
                             g_lastRankingRecord.combo,
                             g_lastRankingRecord.hits,
                             g_lastRankingRecord.misses,
                             g_lastRankingRecord.recordTime.c_str(),
                             g_totalRankingSubmissions);
                }

                LogDebug("User clicked Check Ranking Score: Song='%s', ID=%d, Score=%d, Acc=%d%%, Time=%s",
                         g_lastRankingRecord.songTitle.c_str(),
                         g_lastRankingRecord.songId,
                         g_lastRankingRecord.score,
                         g_lastRankingRecord.accuracy,
                         g_lastRankingRecord.recordTime.c_str());
            } else if (g_isSongActive && g_activeSongId > 0) {
                int32_t liveScore = g_currentScore.load();
                int32_t targetScore = g_targetScore.load();
                int32_t liveHit = getNotesHit();
                int32_t liveCombo = g_currentCombo.load();

                snprintf(rankingMsg, sizeof(rankingMsg),
                         "🏆 SCORE RANKING (LIVE PLAY):\n🎵 Sedang Main: %s\n⭐ Diff: %s | ID: %d\n💎 Live Score: %d\n🎯 Target Score: %d (%s)\n🔥 Combo: %d | Hit: %d\n⏳ Status: Akan tercatat saat lagu selesai",
                         g_activeSongTitle.empty() ? "Lagu Aktif" : g_activeSongTitle.c_str(),
                         g_activeDifficulty.c_str(),
                         g_activeSongId,
                         liveScore,
                         targetScore,
                         g_setScoreEnabled.load() ? "AKTIF" : "NONAKTIF",
                         liveCombo, liveHit);

                LogDebug("User clicked Check Ranking Score during active song: ID=%d, Score=%d", g_activeSongId, liveScore);
            } else {
                int32_t target = g_targetScore.load();
                bool setScoreOn = g_setScoreEnabled.load();

                if (setScoreOn && target > 0) {
                    snprintf(rankingMsg, sizeof(rankingMsg),
                             "🏆 SCORE RANKING RESMI:\nℹ️ Belum ada score tercatat pada sesi ini.\n🎯 Set Score Siap: Target %d\nMainkan lagu sampai selesai untuk mencatat score resmi ke server ranking!",
                             target);
                } else {
                    snprintf(rankingMsg, sizeof(rankingMsg),
                             "🏆 SCORE RANKING RESMI:\nℹ️ Belum ada score tercatat pada sesi ini.\nMainkan lagu sampai selesai untuk mencatat score resmi ke server ranking!");
                }

                LogDebug("User clicked Check Ranking Score: No record yet (SetScore=%d, Target=%d)", setScoreOn ? 1 : 0, target);
            }

            Toast(env, obj, rankingMsg, ToastLength::LENGTH_LONG);
            break;
        }

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
            int32_t score = g_currentScore.load();
            bool setScoreOn = g_setScoreEnabled.load();
            int32_t target = g_targetScore.load();
            bool frozen = g_scoreFrozen.load();

            char statsMsg[512];
            snprintf(statsMsg, sizeof(statsMsg),
                     "LIVE STATS:\n🟢 G:%d 🔴 R:%d 🟡 Y:%d 🔵 B:%d 🟠 O:%d\n🎯 Total Hit: %d | ⚡ Auto Hit: %d\n🔥 Combo: %d | ❌ Miss: %d\n💎 Score: %d %s\n❄️ Freeze Special: %s\n📋 Total Notes: %d",
                     green, red, yellow, blue, orange, hit, autoTotal, combo, miss,
                     score, (setScoreOn && target > 0) ? (frozen ? "[FROZEN]" : "[TARGET]") : "",
                     g_freezeSpecial.load() ? "LOCKED (x8)" : "NORMAL", total);

            LogDebug("User clicked Live Stats: Hit=%d, Auto=%d, Combo=%d, Miss=%d, Score=%d, SetScore=%d, Target=%d",
                     hit, autoTotal, combo, miss, score, setScoreOn ? 1 : 0, target);
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
