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
#include "Includes/Logger.h"
#include "Includes/obfuscate.h"
#include "Includes/Utils.h"
#include "KittyMemory/MemoryPatch.h"
#include "Menu/Setup.h"

// Target library for Unity il2cpp games
#define targetLibName OBFUSCATE("libil2cpp.so")

#include "Includes/Macros.h"

// =========================================================================
// Real-Time Note Counter State & Variables
// =========================================================================
static bool featureNoteCounterHUD = true;   // Default ON: display Hit/Miss on in-game HUD
static bool featureIncludeErradas = true;   // Default ON: count fret/strum errors as Miss

// Pointers to active game instances
static void *mainInstance = nullptr;        // Pointer to Main (MonoBehaviour)
static void *mainNotesInstance = nullptr;   // Pointer to Main_Notes
static void *mainScorerInstance = nullptr;  // Pointer to Main_Scorer

// Helper functions to get current note stats from Main_Notes
static inline int32_t getNotesHit() {
    if (mainNotesInstance != nullptr) {
        // acertadas: 0x64 (Int32) in Main_Notes
        return *(int32_t *)((uintptr_t)mainNotesInstance + 0x64);
    }
    return 0;
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
        // perdidas: 0x70 (Int32) in Main_Notes (missed notes on fretboard)
        return *(int32_t *)((uintptr_t)mainNotesInstance + 0x70);
    }
    return 0;
}

static inline int32_t getNotesErradas() {
    if (mainNotesInstance != nullptr) {
        // erradas: 0x6c (Int32) in Main_Notes (wrong fret strum errors)
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

// Reset note counters to 0
static void resetNoteCounters() {
    if (mainNotesInstance != nullptr) {
        *(int32_t *)((uintptr_t)mainNotesInstance + 0x64) = 0; // acertadas
        *(int32_t *)((uintptr_t)mainNotesInstance + 0x68) = 0; // acertadasSP
        *(int32_t *)((uintptr_t)mainNotesInstance + 0x6c) = 0; // erradas
        *(int32_t *)((uintptr_t)mainNotesInstance + 0x70) = 0; // perdidas
    }
}

// =========================================================================
// Function Pointers & Hook Implementations
// =========================================================================

// 1. Hook Main_Scorer::cdc (RVA: 0x20c38a0)
// Updates the two in-game HUD text displays:
//   Top Text (combo): 0x28
//   Bottom Text (pontos): 0x30
// When Note Counter is active, we redirect:
//   Top Text (combo)     -> Note Masuk (Hits: acertadas)
//   Bottom Text (pontos) -> Note Miss  (Misses: perdidas [+ erradas])
void (*old_Main_Scorer_cdc)(void *instance, int32_t a, int32_t b) = nullptr;
void hook_Main_Scorer_cdc(void *instance, int32_t a, int32_t b) {
    if (instance != nullptr) {
        mainScorerInstance = instance;
    }

    if (featureNoteCounterHUD && old_Main_Scorer_cdc != nullptr) {
        int32_t hits = getNotesHit();
        int32_t misses = getNotesMiss();
        return old_Main_Scorer_cdc(instance, hits, misses);
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

    if (old_Main_Notes_byy != nullptr) {
        old_Main_Notes_byy(instance, hm, b);
    }

    // Immediately refresh the HUD display with zero latency
    if (featureNoteCounterHUD && mainScorerInstance != nullptr && old_Main_Scorer_cdc != nullptr) {
        int32_t hits = getNotesHit();
        int32_t misses = getNotesMiss();
        old_Main_Scorer_cdc(mainScorerInstance, hits, misses);
    }
}

// 3. Hook Main_Notes::byz (RVA: 0x20b9758)
// Called whenever a note is missed (fret error or passed by)
void (*old_Main_Notes_byz)(void *instance, void *hm) = nullptr;
void hook_Main_Notes_byz(void *instance, void *hm) {
    if (instance != nullptr) {
        mainNotesInstance = instance;
    }

    if (old_Main_Notes_byz != nullptr) {
        old_Main_Notes_byz(instance, hm);
    }

    // Immediately refresh the HUD display with zero latency
    if (featureNoteCounterHUD && mainScorerInstance != nullptr && old_Main_Scorer_cdc != nullptr) {
        int32_t hits = getNotesHit();
        int32_t misses = getNotesMiss();
        old_Main_Scorer_cdc(mainScorerInstance, hits, misses);
    }
}

// 4. Hook Main_Notes::byv (RVA: 0x20b8350)
// Called when the song starts or resets all counters to 0
void (*old_Main_Notes_byv)(void *instance) = nullptr;
void hook_Main_Notes_byv(void *instance) {
    if (instance != nullptr) {
        mainNotesInstance = instance;
    }

    if (old_Main_Notes_byv != nullptr) {
        old_Main_Notes_byv(instance);
    }

    // Reset HUD display to (0, 0)
    if (featureNoteCounterHUD && mainScorerInstance != nullptr && old_Main_Scorer_cdc != nullptr) {
        old_Main_Scorer_cdc(mainScorerInstance, 0, 0);
    }
}

// 5. Hook Main::Update (RVA: 0x20c50b0)
// Unity per-frame update for the Main controller
void (*old_Main_Update)(void *instance) = nullptr;
void hook_Main_Update(void *instance) {
    if (old_Main_Update != nullptr) {
        old_Main_Update(instance);
    }

    if (instance != nullptr) {
        mainInstance = instance;

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
    }
}

// Thread to install hooks once libil2cpp.so is loaded
void *hack_thread(void *) {
    LOGI(OBFUSCATE("pthread created"));

    // Check if target lib is loaded
    do {
        sleep(1);
    } while (!isLibraryLoaded(targetLibName));

    LOGI(OBFUSCATE("%s has been loaded"), (const char *) targetLibName);

#if defined(__aarch64__)

    // Hook Main_Scorer::cdc (Updates on-screen HUD text)
    HOOK("0x20c38a0", hook_Main_Scorer_cdc, old_Main_Scorer_cdc);

    // Hook Main_Notes::byy (Note Hit)
    HOOK("0x20b8fec", hook_Main_Notes_byy, old_Main_Notes_byy);

    // Hook Main_Notes::byz (Note Miss)
    HOOK("0x20b9758", hook_Main_Notes_byz, old_Main_Notes_byz);

    // Hook Main_Notes::byv (Song Start / Reset)
    HOOK("0x20b8350", hook_Main_Notes_byv, old_Main_Notes_byv);

    // Hook Main::Update (Capture active instances)
    HOOK("0x20c50b0", hook_Main_Update, old_Main_Update);

    LOGI(OBFUSCATE("All Guitar Hero Note Counter hooks installed successfully!"));

#else 

    LOGI(OBFUSCATE("ARM64 only for this target"));
#endif

    return NULL;
}

jobjectArray GetFeatureList(JNIEnv *env, jobject context) {
    jobjectArray ret;

    const char *features[] = {
            OBFUSCATE("Category_Real-Time Note Counter"),
            OBFUSCATE("Toggle_True_Counter on In-Game HUD"),
            OBFUSCATE("Toggle_True_Count Fret Errors as Miss"),
            OBFUSCATE("Button_Check Live Stats (Toast)"),
            OBFUSCATE("Button_Reset Note Counters"),
            OBFUSCATE("Category_HUD Display Info"),
            OBFUSCATE("RichTextView_<small><b>Top Box:</b> Note Masuk (Hits)<br><b>Bottom Box:</b> Note Miss (Misses)</small>"),
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
        case 0: // Toggle Note Counter on HUD
            featureNoteCounterHUD = boolean;
            if (featureNoteCounterHUD) {
                if (mainScorerInstance != nullptr && old_Main_Scorer_cdc != nullptr) {
                    old_Main_Scorer_cdc(mainScorerInstance, getNotesHit(), getNotesMiss());
                }
                Toast(env, obj, OBFUSCATE("Note Counter HUD: Aktif (Top: Masuk | Bottom: Miss)"), ToastLength::LENGTH_SHORT);
            } else {
                Toast(env, obj, OBFUSCATE("Note Counter HUD: Nonaktif (Normal Combo/Score)"), ToastLength::LENGTH_SHORT);
            }
            break;

        case 1: // Toggle Count Fret Errors as Miss
            featureIncludeErradas = boolean;
            if (featureNoteCounterHUD && mainScorerInstance != nullptr && old_Main_Scorer_cdc != nullptr) {
                old_Main_Scorer_cdc(mainScorerInstance, getNotesHit(), getNotesMiss());
            }
            if (boolean) {
                Toast(env, obj, OBFUSCATE("Hitung Miss: Perdidas + Erradas"), ToastLength::LENGTH_SHORT);
            } else {
                Toast(env, obj, OBFUSCATE("Hitung Miss: Perdidas Saja"), ToastLength::LENGTH_SHORT);
            }
            break;

        case 2: { // Button: Check Live Stats (Toast)
            char statsMsg[256];
            if (mainNotesInstance != nullptr) {
                int32_t hit = getNotesHit();
                int32_t hitSP = getNotesHitSP();
                int32_t perd = getNotesPerdidas();
                int32_t err = getNotesErradas();
                int32_t miss = getNotesMiss();
                int32_t total = getNotesTotalCreated();
                snprintf(statsMsg, sizeof(statsMsg),
                         "LIVE NOTE STATS:\nNote Masuk (Hit): %d (SP: %d)\nNote Miss: %d (Perdidas: %d, Erradas: %d)\nTotal Notes: %d",
                         hit, hitSP, miss, perd, err, total);
            } else {
                snprintf(statsMsg, sizeof(statsMsg), "Main_Notes belum aktif.\nSilakan mainkan lagu terlebih dahulu.");
            }
            Toast(env, obj, statsMsg, ToastLength::LENGTH_LONG);
            break;
        }

        case 3: // Button: Reset Note Counters
            resetNoteCounters();
            if (mainScorerInstance != nullptr && old_Main_Scorer_cdc != nullptr) {
                old_Main_Scorer_cdc(mainScorerInstance, 0, 0);
            }
            Toast(env, obj, OBFUSCATE("Counter note di-reset ke 0!"), ToastLength::LENGTH_SHORT);
            break;
    }
}

__attribute__((constructor))
void lib_main() {
    // Create a new thread so it does not block the main thread, means the game would not freeze
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
