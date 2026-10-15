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
// Real-Time Note Counter & Auto Hit (Smart Move) State
// =========================================================================
static bool featureFloatingHUD = true;      // Default ON: display Floating HUD panel
static bool featureIncludeErradas = true;   // Default ON: count fret/strum errors as Miss

// Auto Hit (Smart Move) Settings
static std::atomic<bool> g_autoMaster{true};       // Master switch for Auto Hit
static std::atomic<bool> g_autoGreen{true};        // Auto Green note (Fret 0)
static std::atomic<bool> g_autoRed{true};          // Auto Red note (Fret 1)
static std::atomic<bool> g_autoYellow{true};       // Auto Yellow note (Fret 2)
static std::atomic<bool> g_autoBlue{true};         // Auto Blue note (Fret 3)
static std::atomic<bool> g_autoOrange{true};       // Auto Orange note (Fret 4)
static std::atomic<int> g_autoTimingMode{0};       // 0 = 100% Perfect, 1 = Humanized (Legit), 2 = Instant
static std::atomic<bool> g_autoVisualFret{true};    // Animate visual fret buttons on screen
static std::atomic<bool> g_autoSustain{true};       // Automatically hold sustain/rastro notes

// Per-fret internal trackers (frets 0..4)
static std::atomic<int> g_fretWindowFrames[5]{0, 0, 0, 0, 0};
static std::atomic<int> g_fretVisualTimer[5]{0, 0, 0, 0, 0};
static std::atomic<bool> g_fretSustaining[5]{false, false, false, false, false};
static std::atomic<int32_t> g_autoHitsCount[5]{0, 0, 0, 0, 0};
static std::atomic<int32_t> g_totalAutoHits{0};

// Pointers to active game instances
static void *mainInstance = nullptr;        // Pointer to Main (MonoBehaviour)
static void *mainNotesInstance = nullptr;   // Pointer to Main_Notes
static void *mainScorerInstance = nullptr;  // Pointer to Main_Scorer
static void *mainFretsInstance = nullptr;   // Pointer to Main_Frets

static inline bool isColorFretAutoEnabled(int fret) {
    if (!g_autoMaster.load()) return false;
    switch (fret) {
        case 0: return g_autoGreen.load();
        case 1: return g_autoRed.load();
        case 2: return g_autoYellow.load();
        case 3: return g_autoBlue.load();
        case 4: return g_autoOrange.load();
        default: return false;
    }
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
        g_fretWindowFrames[i] = 0;
        g_fretVisualTimer[i] = 0;
        g_fretSustaining[i] = false;
        g_autoHitsCount[i] = 0;
    }
    g_totalAutoHits = 0;

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

// Forward declaration of visual fret function pointer
void (*old_Main_Frets_byc)(void *instance, int32_t fret, bool pressed) = nullptr;

// 1. Hook Main_Scorer::cdc (RVA: 0x20c38a0)
void (*old_Main_Scorer_cdc)(void *instance, int32_t a, int32_t b) = nullptr;
void hook_Main_Scorer_cdc(void *instance, int32_t a, int32_t b) {
    if (instance != nullptr) {
        mainScorerInstance = instance;
    }
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
        switch (fret) {
            case 0: g_greenHits++; break;
            case 1: g_redHits++; break;
            case 2: g_yellowHits++; break;
            case 3: g_blueHits++; break;
            case 4: g_orangeHits++; break;
            default: g_otherHits++; break;
        }

        // Track sustain notes (rastro: offset 0x28)
        if (fret >= 0 && fret < 5) {
            void *rastro = *(void **)((uintptr_t)hm + 0x28);
            if (rastro != nullptr) {
                // bool has: offset 0x20
                bool hasRastro = *(bool *)((uintptr_t)rastro + 0x20);
                if (hasRastro) {
                    g_fretSustaining[fret] = true;
                }
            }
        }
    }

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
        switch (fret) {
            case 0: g_greenMisses++; break;
            case 1: g_redMisses++; break;
            case 2: g_yellowMisses++; break;
            case 3: g_blueMisses++; break;
            case 4: g_orangeMisses++; break;
            default: break;
        }
        if (fret >= 0 && fret < 5) {
            g_fretWindowFrames[fret] = 0;
            g_fretSustaining[fret] = false;
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
        g_fretWindowFrames[i] = 0;
        g_fretVisualTimer[i] = 0;
        g_fretSustaining[i] = false;
        g_autoHitsCount[i] = 0;
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
            if (old_Main_Frets_byc != nullptr && mainFretsInstance != nullptr) {
                old_Main_Frets_byc(mainFretsInstance, fret, false); // Release visual fret
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
    if (old_Main_Frets_byc != nullptr) {
        old_Main_Frets_byc(instance, fret, pressed);
    }
}

// 7. Hook Main_Frets::byd (RVA: 0x1f9d19c)
// Called by Main::FixedUpdate to test if a fret was strummed/hit for an active note in window
bool (*old_Main_Frets_byd)(void *instance, int32_t fret) = nullptr;
bool hook_Main_Frets_byd(void *instance, int32_t fret) {
    if (instance != nullptr) {
        mainFretsInstance = instance;
    }

    // A. Prioritize manual player press 100%
    bool manual = false;
    if (old_Main_Frets_byd != nullptr) {
        manual = old_Main_Frets_byd(instance, fret);
    }
    if (manual) {
        if (fret >= 0 && fret < 5) {
            g_fretWindowFrames[fret] = 0;
        }
        return true;
    }

    // B. Smart Move Auto Hit logic
    if (fret >= 0 && fret < 5 && isColorFretAutoEnabled(fret)) {
        int timingMode = g_autoTimingMode.load();
        int frames = ++g_fretWindowFrames[fret];

        // Frame target inside hit window:
        // Mode 0: 100% Perfect (frame 2, center sweet spot of hit window)
        // Mode 1: Humanized (Legit pro-player timing variation: mostly frame 2, sometimes frame 3)
        // Mode 2: Instant (frame 1, immediate trigger upon entering window)
        int targetFrames = 1;
        if (timingMode == 0) {
            targetFrames = 2; // Perfect sweet spot
        } else if (timingMode == 1) {
            static unsigned int randSeed = 98765;
            randSeed = randSeed * 1103515245 + 12345;
            targetFrames = ((randSeed >> 16) % 10 < 8) ? 2 : 3;
        }

        if (frames >= targetFrames) {
            g_fretWindowFrames[fret] = 0;
            g_autoHitsCount[fret]++;
            g_totalAutoHits++;

            // Smart Move: Animate visual fret press on screen
            if (g_autoVisualFret.load() && old_Main_Frets_byc != nullptr && mainFretsInstance != nullptr) {
                old_Main_Frets_byc(mainFretsInstance, fret, true); // Press visual fret
                g_fretVisualTimer[fret] = 5; // Hold visual press for ~80-100ms
            }

            return true;
        }
    }

    return false;
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
    if (g_autoSustain.load() && fret >= 0 && fret < 5 && isColorFretAutoEnabled(fret)) {
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

        // Main.scorer: 0x110
        void *scorer = *(void **)((uintptr_t)instance + 0x110);
        if (scorer != nullptr) {
            mainScorerInstance = scorer;
        }

        if (!g_mainCapturedLogged && notes != nullptr) {
            g_mainCapturedLogged = true;
            LogDebug("Captured active instances: Main=0x%lx, Frets=0x%lx, Notes=0x%lx, Scorer=0x%lx",
                     (unsigned long)mainInstance, (unsigned long)mainFretsInstance,
                     (unsigned long)mainNotesInstance, (unsigned long)mainScorerInstance);
        }
    }

    // Smart Move: Manage visual fret release timers
    for (int i = 0; i < 5; i++) {
        if (g_fretVisualTimer[i] > 0) {
            int t = --g_fretVisualTimer[i];
            if (t == 0 && !g_fretSustaining[i].load()) {
                if (old_Main_Frets_byc != nullptr && mainFretsInstance != nullptr) {
                    old_Main_Frets_byc(mainFretsInstance, i, false); // Release visual fret
                }
            }
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

    // 7. Hook Main_Frets::byd (0x1f9d19c) - Strum/hit trigger check
    HOOK("0x1f9d19c", hook_Main_Frets_byd, old_Main_Frets_byd);
    LogDebug("Hook Main_Frets::byd (0x1f9d19c): %s", old_Main_Frets_byd != nullptr ? "SUCCESS" : "FAILED");

    // 8. Hook Main_Frets::bye (0x1f9d1dc) - Sustain hold check
    HOOK("0x1f9d1dc", hook_Main_Frets_bye, old_Main_Frets_bye);
    LogDebug("Hook Main_Frets::bye (0x1f9d1dc): %s", old_Main_Frets_bye != nullptr ? "SUCCESS" : "FAILED");

    // 9. Hook Main::Update (0x20c50b0)
    HOOK("0x20c50b0", hook_Main_Update, old_Main_Update);
    LogDebug("Hook Main::Update (0x20c50b0): %s", old_Main_Update != nullptr ? "SUCCESS" : "FAILED");

    LogDebug("All Guitar Hero Real-Time & Auto Hit hooks installed successfully!");

#else 
    LogDebug("ARM32 architecture not supported for this 64-bit target");
#endif

    return NULL;
}

// =========================================================================
// JNI Method Implementations
// =========================================================================

// Returns [green, red, yellow, blue, orange, totalHits, totalMisses, totalCreated, totalAutoHits, autoMaster]
jintArray GetNoteStats(JNIEnv *env, jobject thiz) {
    jintArray result = env->NewIntArray(10);
    if (!result) return nullptr;

    jint fill[10];
    fill[0] = g_greenHits.load();
    fill[1] = g_redHits.load();
    fill[2] = g_yellowHits.load();
    fill[3] = g_blueHits.load();
    fill[4] = g_orangeHits.load();
    fill[5] = getNotesHit();
    fill[6] = getNotesMiss();
    fill[7] = getNotesTotalCreated();
    fill[8] = g_totalAutoHits.load();
    fill[9] = g_autoMaster.load() ? 1 : 0;

    env->SetIntArrayRegion(result, 0, 10, fill);
    return result;
}

jobjectArray GetFeatureList(JNIEnv *env, jobject context) {
    jobjectArray ret;

    const char *features[] = {
            OBFUSCATE("Category_⚡ Auto Hit (Smart Move)"),
            OBFUSCATE("10_Toggle_True_Master Auto Hit"),
            OBFUSCATE("11_Toggle_True_🟢 Auto Green Note"),
            OBFUSCATE("12_Toggle_True_🔴 Auto Red Note"),
            OBFUSCATE("13_Toggle_True_🟡 Auto Yellow Note"),
            OBFUSCATE("14_Toggle_True_🔵 Auto Blue Note"),
            OBFUSCATE("15_Toggle_True_🟠 Auto Orange Note"),
            OBFUSCATE("16_Spinner_Timing Mode_100% Perfect,Humanized (Legit),Instant"),
            OBFUSCATE("17_Toggle_True_Sync Visual Fret Press"),
            OBFUSCATE("18_Toggle_True_Auto Sustain (Long Note)"),

            OBFUSCATE("Category_Real-Time Note Counter"),
            OBFUSCATE("0_Toggle_True_Floating Note HUD"),
            OBFUSCATE("1_Toggle_True_Count Fret Errors as Miss"),
            OBFUSCATE("2_Button_Check Live Stats (Toast)"),
            OBFUSCATE("3_Button_Reset Note Counters"),
            OBFUSCATE("4_Button_Check Debug Log Path"),

            OBFUSCATE("Category_HUD & Fret Legend"),
            OBFUSCATE("RichTextView_🟢 Green (0) | 🔴 Red (1) | 🟡 Yellow (2)<br>🔵 Blue (3) | 🟠 Orange (4)<br><small>Smart Move: Animasi visual & timing natural.</small>"),
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
        // Auto Hit Features
        case 10: // Master Auto Hit
            g_autoMaster = boolean;
            LogDebug("Auto Hit Master toggled: %d", boolean);
            Toast(env, obj, boolean ? OBFUSCATE("⚡ Auto Hit Master: AKTIF") : OBFUSCATE("⚡ Auto Hit Master: NONAKTIF"), ToastLength::LENGTH_SHORT);
            break;

        case 11: // Auto Green
            g_autoGreen = boolean;
            LogDebug("Auto Green toggled: %d", boolean);
            Toast(env, obj, boolean ? OBFUSCATE("🟢 Auto Green: ON") : OBFUSCATE("🟢 Auto Green: OFF"), ToastLength::LENGTH_SHORT);
            break;

        case 12: // Auto Red
            g_autoRed = boolean;
            LogDebug("Auto Red toggled: %d", boolean);
            Toast(env, obj, boolean ? OBFUSCATE("🔴 Auto Red: ON") : OBFUSCATE("🔴 Auto Red: OFF"), ToastLength::LENGTH_SHORT);
            break;

        case 13: // Auto Yellow
            g_autoYellow = boolean;
            LogDebug("Auto Yellow toggled: %d", boolean);
            Toast(env, obj, boolean ? OBFUSCATE("🟡 Auto Yellow: ON") : OBFUSCATE("🟡 Auto Yellow: OFF"), ToastLength::LENGTH_SHORT);
            break;

        case 14: // Auto Blue
            g_autoBlue = boolean;
            LogDebug("Auto Blue toggled: %d", boolean);
            Toast(env, obj, boolean ? OBFUSCATE("🔵 Auto Blue: ON") : OBFUSCATE("🔵 Auto Blue: OFF"), ToastLength::LENGTH_SHORT);
            break;

        case 15: // Auto Orange
            g_autoOrange = boolean;
            LogDebug("Auto Orange toggled: %d", boolean);
            Toast(env, obj, boolean ? OBFUSCATE("🟠 Auto Orange: ON") : OBFUSCATE("🟠 Auto Orange: OFF"), ToastLength::LENGTH_SHORT);
            break;

        case 16: // Timing Mode Spinner
            g_autoTimingMode = value;
            LogDebug("Auto Timing Mode changed: %d", value);
            if (value == 0) {
                Toast(env, obj, OBFUSCATE("Timing: 100% Perfect (Sweet Spot)"), ToastLength::LENGTH_SHORT);
            } else if (value == 1) {
                Toast(env, obj, OBFUSCATE("Timing: Humanized (Legit Pro)"), ToastLength::LENGTH_SHORT);
            } else {
                Toast(env, obj, OBFUSCATE("Timing: Instant (Edge of Window)"), ToastLength::LENGTH_SHORT);
            }
            break;

        case 17: // Sync Visual Fret Press
            g_autoVisualFret = boolean;
            LogDebug("Auto Visual Fret Press toggled: %d", boolean);
            Toast(env, obj, boolean ? OBFUSCATE("Animasi Visual Fret: ON") : OBFUSCATE("Animasi Visual Fret: OFF"), ToastLength::LENGTH_SHORT);
            break;

        case 18: // Auto Sustain
            g_autoSustain = boolean;
            LogDebug("Auto Sustain toggled: %d", boolean);
            Toast(env, obj, boolean ? OBFUSCATE("Auto Sustain (Long Note): ON") : OBFUSCATE("Auto Sustain: OFF"), ToastLength::LENGTH_SHORT);
            break;

        // Note Counter & HUD Features
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
            int32_t hitSP = getNotesHitSP();
            int32_t perd = getNotesPerdidas();
            int32_t err = getNotesErradas();
            int32_t miss = getNotesMiss();
            int32_t total = getNotesTotalCreated();
            int32_t autoTotal = g_totalAutoHits.load();

            char statsMsg[450];
            snprintf(statsMsg, sizeof(statsMsg),
                     "LIVE NOTE STATS:\n🟢 G: %d | 🔴 R: %d | 🟡 Y: %d\n🔵 B: %d | 🟠 O: %d\n🎯 Total Hit: %d (SP: %d)\n⚡ Total Auto Hit: %d\n❌ Miss: %d (Perd: %d, Err: %d)\nTotal Notes: %d",
                     green, red, yellow, blue, orange, hit, hitSP, autoTotal, miss, perd, err, total);

            LogDebug("User clicked Live Stats: Hit=%d, Auto=%d, Miss=%d, G:%d R:%d Y:%d B:%d O:%d",
                     hit, autoTotal, miss, green, red, yellow, blue, orange);
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
