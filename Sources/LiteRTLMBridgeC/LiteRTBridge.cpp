// LiteRT-LM Bridge — Implementierung
//
// Kapselt die LiteRT-LM C-API hinter unserer eigenen schlanken C-Schnittstelle.
// Die LiteRT-LM C-API befindet sich unter:
//   $(SRCROOT)/../LiteRT-LM/c/engine.h

#include "LiteRTBridge.h"

#include <TargetConditionals.h>

#if TARGET_OS_SIMULATOR
// ---------------------------------------------------------------------------
// Simulator-Stub (#109)
//
// Im iOS-Simulator gibt es KEINE native LiteRT-LM-Binary (die ~185 MB Vendor-
// Slice + Rust-Closure werden nur fürs Gerät gebaut). Der Swift-Wrapper
// LiteRTEngine nutzt im Simulator ohnehin einen reinen Mock und ruft KEINE
// dieser C-Funktionen auf. Damit der C-Bridge-Code für den Simulator linkt,
// OHNE Vendor-Symbole zu referenzieren, liefern wir hier triviale Stubs.
// → CI/Tests können die Pipeline (über den Swift-Mock) im Simulator ausführen.
// ---------------------------------------------------------------------------
#include <cstdlib>

extern "C" {

LiteRTEngineRef litert_engine_create(const char*, const char*, bool) {
    return nullptr;  // wird im Simulator nie aufgerufen (Swift-Mock)
}

void litert_engine_destroy(LiteRTEngineRef) {}

const char* litert_engine_send_message(LiteRTEngineRef, const char*) {
    return nullptr;  // wird im Simulator nie aufgerufen (Swift-Mock)
}

void litert_free_string(const char* str) {
    free(const_cast<char*>(str));
}

} // extern "C"

#else  // !TARGET_OS_SIMULATOR — echte Geräte-Implementierung

#include "LiteRTBridgeExt.h"

// Erzwingt dass der Linker engine_impl.o aus LiteRTLMVendor.a einschließt.
// engine_impl.o enthält LITERT_LM_REGISTER_ENGINE(kLiteRTCompiledModel, ...)
// als statischen Initializer — ohne Referenz auf ein öffentliches Symbol würde
// der Linker es dead-strippen → EngineFactory leer → "Engine type not found: 1".
extern "C" void litert_lm_force_register_engine_impl();
static const auto _force_engine_impl = (litert_lm_force_register_engine_impl(), 0);

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <mutex>
#include <string>

// ---------------------------------------------------------------------------
// Internes Handle — kapselt LiteRT-LM Engine
// ---------------------------------------------------------------------------
//
// Die Session wird bewusst NICHT persistent gehalten:
// `SessionBasic` ruft `executor_.Reset()` erst im Destruktor auf
// (siehe LiteRT-LM/runtime/core/session_basic.cc:132). `GenerateContent`
// hängt an den bestehenden KV-Cache an, statt ihn zu leeren. Für unsere
// unabhängigen Dokument-Scans (und Retry-Versuche innerhalb eines Scans)
// bedeutet eine wiederverwendete Session: der Kontext läuft nach wenigen
// Aufrufen voll und die Ausgabe wird auf wenige Tokens abgeschnitten.

struct LiteRTEngineHandle {
    LiteRtLmEngine*         lm_engine   = nullptr;
    LiteRtLmEngineSettings* lm_settings = nullptr;

    // Serialisiert Inferenz-Aufrufe aus Swift
    std::mutex inference_mutex;

    bool is_initialized = false;

    LiteRTEngineHandle() = default;
    LiteRTEngineHandle(const LiteRTEngineHandle&)            = delete;
    LiteRTEngineHandle& operator=(const LiteRTEngineHandle&) = delete;
};

// ---------------------------------------------------------------------------
// Hilfsfunktion: Engine mit bestem verfügbaren Backend initialisieren
// Versucht GPU, fällt auf CPU zurück wenn GPU fehlschlägt.
// ---------------------------------------------------------------------------
//
// WICHTIG (Crash-Fix): Die Engine wird hier GENAU EINMAL erzeugt.
// Frühere Versionen legten zur Backend-Probe eine "Test-Engine" an, löschten
// sie wieder und erzeugten danach die echte Engine ein zweites Mal aus DENSELBEN
// Settings. Dieser Create→Delete→Create-Zyklus lud das ~2 GB-Modell doppelt und
// verdoppelte die Crash-Exposition in litert_lm_engine_create (EngineSettings-/
// AdvancedSettings-Copy, std::set<int>). Jetzt: ein Create, Engine wird behalten.
//
// Bei Erfolg wird die Engine zurückgegeben und *out_settings auf die zugehörigen
// (am Leben gehaltenen) Settings gesetzt — der Aufrufer übernimmt beide ins Handle.

static LiteRtLmEngine* create_engine_with_best_backend(
    const char* model_path, const char* cache_dir, bool enable_speculative_decoding,
    LiteRtLmEngineSettings** out_settings)
{
    *out_settings = nullptr;

    // Reihenfolge: GPU (Metal) → CPU
    // GPU ist auf A-Series-Chips 3–10× schneller als CPU.
    const char* backends[] = { "gpu", "cpu" };

    // Zwei Pässe wenn Speculative Decoding angefordert:
    //   Pass 0: mit SD (GPU → CPU)
    //   Pass 1: ohne SD (GPU → CPU) — Fallback falls Modell keinen Drafter hat
    // Ohne SD: nur ein Pass.
    const int num_passes = enable_speculative_decoding ? 2 : 1;

    for (int pass = 0; pass < num_passes; pass++) {
        const bool use_sd = enable_speculative_decoding && (pass == 0);
        if (pass == 1) {
            fprintf(stderr, "[LiteRTBridge] SD-Fallback: Speculative Decoding deaktiviert "
                            "(kein Drafter im Modell?)\n");
        }

        for (const char* backend : backends) {
            LiteRtLmEngineSettings* settings = litert_lm_engine_settings_create(
                model_path,
                backend,
                nullptr,  // Vision-Backend
                nullptr   // Audio-Backend
            );
            if (!settings) continue;

            // Kontext-Fenster (KV-Cache): System-Prompt + OCR-Text/Tag-Liste + Antwort.
            // MUSS der kompilierten Kontextlänge des Modells entsprechen: gemma-4-E2B ist
            // auf 4096 kompiliert (magic_number target_number=4096, Signaturen prefill_128/1024).
            // Ein höherer Wert (8192, #136) ließ generate_content über die KV-Cache-Buffer
            // hinaus indexieren → EXC_BAD_ACCESS am Gerät. Daher fest auf 4096.
            constexpr int kMaxNumTokens = 4096;
            litert_lm_engine_settings_set_max_num_tokens(settings, kMaxNumTokens);

            if (cache_dir && *cache_dir != '\0') {
                litert_lm_engine_settings_set_cache_dir(settings, cache_dir);
            }

            if (use_sd) {
                litert_lm_engine_settings_enable_speculative_decoding(settings);
            }

            // Engine GENAU EINMAL erzeugen. Erfolg → behalten (kein zweiter Create).
            // Misserfolg (NULL, z.B. Backend/Drafter nicht verfügbar) → nächstes Backend.
            LiteRtLmEngine* engine = litert_lm_engine_create(settings);
            if (engine) {
                fprintf(stderr, "[LiteRTBridge] Backend: %s%s ✓\n",
                        backend, use_sd ? " + MTP-Drafter" : "");
                *out_settings = settings;  // Settings am Leben halten (Handle übernimmt)
                return engine;
            }

            // Dieses Backend fehlgeschlagen → Settings freigeben, nächstes versuchen
            fprintf(stderr, "[LiteRTBridge] Backend %s%s fehlgeschlagen, versuche nächstes …\n",
                    backend, use_sd ? " + SD" : "");
            litert_lm_engine_settings_delete(settings);
        }
    }

    return nullptr;
}

// ---------------------------------------------------------------------------
// Session-Konfig: Output-Token-Limit für kürzere JSON-Antworten
// ---------------------------------------------------------------------------

static LiteRtLmSessionConfig* create_session_config() {
    LiteRtLmSessionConfig* config = litert_lm_session_config_create();
    if (!config) return nullptr;
    // #136: 512 → 1024. Metadaten-JSON braucht ~300 Tokens, aber der Tag-Merge
    // gibt JSON mit mehreren Gruppen aus, das bei 512 mitten im Objekt abschnitt
    // (siehe #138). Early-Stopping greift weiterhin über die Stop-Tokens.
    constexpr int kMaxOutputTokens = 1024;
    litert_lm_session_config_set_max_output_tokens(config, kMaxOutputTokens);
    return config;
}

// ---------------------------------------------------------------------------
// C-API Implementierung
// ---------------------------------------------------------------------------

extern "C" {

LiteRTEngineRef litert_engine_create(const char* model_path, const char* cache_dir,
                                     bool enable_speculative_decoding) {
    if (!model_path) {
        fprintf(stderr, "[LiteRTBridge] litert_engine_create: model_path ist NULL\n");
        return nullptr;
    }

    fprintf(stderr, "[LiteRTBridge] Cache-Dir: %s | SD: %s\n",
            cache_dir ? cache_dir : "(keiner)",
            enable_speculative_decoding ? "an" : "aus");

    LiteRTEngineHandle* handle = nullptr;
    try {
        handle = new LiteRTEngineHandle();

        // Engine + Settings mit bestem verfügbaren Backend (GPU → CPU Fallback,
        // bei SD-Anforderung zusätzlich Fallback ohne Drafter). Die Engine wird
        // dabei nur EINMAL erzeugt (kein separater Test-Create mehr → Modell wird
        // nicht doppelt geladen, halbe Crash-Exposition).
        handle->lm_engine = create_engine_with_best_backend(
            model_path, cache_dir, enable_speculative_decoding, &handle->lm_settings);
        if (!handle->lm_engine) {
            fprintf(stderr, "[LiteRTBridge] Alle Backends fehlgeschlagen — %s\n", model_path);
            if (handle->lm_settings) litert_lm_engine_settings_delete(handle->lm_settings);
            delete handle;
            return nullptr;
        }

        handle->is_initialized = true;
        fprintf(stderr, "[LiteRTBridge] Engine bereit: %s\n", model_path);
        return static_cast<LiteRTEngineRef>(handle);

    } catch (const std::exception& e) {
        fprintf(stderr, "[LiteRTBridge] litert_engine_create Exception: %s\n", e.what());
        if (handle) {
            if (handle->lm_engine)   litert_lm_engine_delete(handle->lm_engine);
            if (handle->lm_settings) litert_lm_engine_settings_delete(handle->lm_settings);
            delete handle;
        }
        return nullptr;
    } catch (...) {
        fprintf(stderr, "[LiteRTBridge] litert_engine_create: unbekannte Exception\n");
        if (handle) {
            if (handle->lm_engine)   litert_lm_engine_delete(handle->lm_engine);
            if (handle->lm_settings) litert_lm_engine_settings_delete(handle->lm_settings);
            delete handle;
        }
        return nullptr;
    }
}

void litert_engine_destroy(LiteRTEngineRef engine) {
    if (!engine) return;
    LiteRTEngineHandle* handle = static_cast<LiteRTEngineHandle*>(engine);
    if (handle->lm_engine)   litert_lm_engine_delete(handle->lm_engine);
    if (handle->lm_settings) litert_lm_engine_settings_delete(handle->lm_settings);
    delete handle;
}

const char* litert_engine_send_message(LiteRTEngineRef engine, const char* message) {
    if (!engine || !message) {
        fprintf(stderr, "[LiteRTBridge] litert_engine_send_message: NULL-Argument\n");
        return nullptr;
    }

    LiteRTEngineHandle* handle = static_cast<LiteRTEngineHandle*>(engine);
    std::lock_guard<std::mutex> lock(handle->inference_mutex);

    if (!handle->is_initialized || !handle->lm_engine) {
        fprintf(stderr, "[LiteRTBridge] litert_engine_send_message: Engine nicht initialisiert\n");
        return nullptr;
    }

    LiteRtLmSessionConfig* session_config = nullptr;
    LiteRtLmSession*       session        = nullptr;
    LiteRtLmResponses*     responses      = nullptr;

    try {
        // Frische Session pro Aufruf — SessionBasic ruft executor_.Reset()
        // nur im Destruktor auf, eine wiederverwendete Session würde also
        // den KV-Cache zwischen Aufrufen behalten und nach wenigen Scans
        // den 4096-Token-Kontext sprengen (→ abgeschnittene Ausgabe).
        session_config = create_session_config();
        session = litert_lm_engine_create_session(handle->lm_engine, session_config);
        if (!session) {
            fprintf(stderr, "[LiteRTBridge] Session-Init fehlgeschlagen\n");
            if (session_config) litert_lm_session_config_delete(session_config);
            return nullptr;
        }

        InputData input;
        input.type = kInputText;
        input.data = static_cast<const void*>(message);
        input.size = strlen(message);

        responses = litert_lm_session_generate_content(session, &input, 1);
        if (!responses) {
            fprintf(stderr, "[LiteRTBridge] litert_lm_session_generate_content fehlgeschlagen\n");
            litert_lm_session_delete(session);
            if (session_config) litert_lm_session_config_delete(session_config);
            return nullptr;
        }

        const int num_candidates = litert_lm_responses_get_num_candidates(responses);
        if (num_candidates == 0) {
            fprintf(stderr, "[LiteRTBridge] Keine Antwort-Kandidaten\n");
            litert_lm_responses_delete(responses);
            litert_lm_session_delete(session);
            if (session_config) litert_lm_session_config_delete(session_config);
            return nullptr;
        }

        const char* text = litert_lm_responses_get_response_text_at(responses, 0);
        if (!text) {
            litert_lm_responses_delete(responses);
            litert_lm_session_delete(session);
            if (session_config) litert_lm_session_config_delete(session_config);
            return nullptr;
        }

        const size_t len    = strlen(text);
        char*        result = static_cast<char*>(malloc(len + 1));
        if (!result) {
            litert_lm_responses_delete(responses);
            litert_lm_session_delete(session);
            if (session_config) litert_lm_session_config_delete(session_config);
            return nullptr;
        }
        memcpy(result, text, len + 1);

        litert_lm_responses_delete(responses);
        litert_lm_session_delete(session);
        if (session_config) litert_lm_session_config_delete(session_config);
        return result;

    } catch (const std::exception& e) {
        fprintf(stderr, "[LiteRTBridge] send_message Exception: %s\n", e.what());
        if (responses)      litert_lm_responses_delete(responses);
        if (session)        litert_lm_session_delete(session);
        if (session_config) litert_lm_session_config_delete(session_config);
        return nullptr;
    } catch (...) {
        if (responses)      litert_lm_responses_delete(responses);
        if (session)        litert_lm_session_delete(session);
        if (session_config) litert_lm_session_config_delete(session_config);
        return nullptr;
    }
}

void litert_free_string(const char* str) {
    free(const_cast<char*>(str));
}

} // extern "C"

#endif // TARGET_OS_SIMULATOR
