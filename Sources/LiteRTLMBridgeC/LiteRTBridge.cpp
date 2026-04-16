// LiteRT-LM Bridge — Implementierung
//
// Kapselt die LiteRT-LM C-API hinter unserer eigenen schlanken C-Schnittstelle.
// Die LiteRT-LM C-API befindet sich unter:
//   $(SRCROOT)/../LiteRT-LM/c/engine.h

#include "LiteRTBridge.h"
#include "c/engine.h"

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
// Internes Handle — kapselt LiteRT-LM Engine + persistente Session
// ---------------------------------------------------------------------------

struct LiteRTEngineHandle {
    LiteRtLmEngine*         lm_engine   = nullptr;
    LiteRtLmEngineSettings* lm_settings = nullptr;

    // Persistente Session — einmal anlegen, pro Inferenz neu befüllen.
    // Erspart ~100–200 ms Session-Init-Overhead je Scan.
    LiteRtLmSession*        lm_session  = nullptr;

    // Serialisiert Inferenz-Aufrufe aus Swift
    std::mutex inference_mutex;

    bool is_initialized = false;

    LiteRTEngineHandle() = default;
    LiteRTEngineHandle(const LiteRTEngineHandle&)            = delete;
    LiteRTEngineHandle& operator=(const LiteRTEngineHandle&) = delete;
};

// ---------------------------------------------------------------------------
// Hilfsfunktion: Backend mit Fallback initialisieren
// Versucht GPU, fällt auf CPU zurück wenn GPU fehlschlägt.
// ---------------------------------------------------------------------------

static LiteRtLmEngineSettings* create_settings_with_best_backend(
    const char* model_path, const char* cache_dir)
{
    // Reihenfolge: GPU (Metal) → CPU
    // GPU ist auf A-Series-Chips 3–10× schneller als CPU.
    const char* backends[] = { "gpu", "cpu" };

    for (const char* backend : backends) {
        LiteRtLmEngineSettings* settings = litert_lm_engine_settings_create(
            model_path,
            backend,
            nullptr,  // Vision-Backend
            nullptr   // Audio-Backend
        );
        if (!settings) continue;

        // Kontext-Fenster: System-Prompt (~500) + OCR-Text (3000) + Antwort (300)
        // 4096 ist ausreichend und hält den KV-Cache kleiner als 8192.
        litert_lm_engine_settings_set_max_num_tokens(settings, 4096);

        if (cache_dir && *cache_dir != '\0') {
            litert_lm_engine_settings_set_cache_dir(settings, cache_dir);
        }

        // Testweise Engine anlegen um Backend-Verfügbarkeit zu prüfen
        LiteRtLmEngine* test_engine = litert_lm_engine_create(settings);
        if (test_engine) {
            // GPU funktioniert — Engine behalten
            fprintf(stderr, "[LiteRTBridge] Backend: %s ✓\n", backend);
            litert_lm_engine_delete(test_engine);
            return settings;
        }

        // Dieses Backend fehlgeschlagen → nächstes versuchen
        fprintf(stderr, "[LiteRTBridge] Backend %s fehlgeschlagen, versuche nächstes …\n", backend);
        litert_lm_engine_settings_delete(settings);
    }

    return nullptr;
}

// ---------------------------------------------------------------------------
// Session-Konfig: Output-Token-Limit für kürzere JSON-Antworten
// ---------------------------------------------------------------------------

static LiteRtLmSessionConfig* create_session_config() {
    LiteRtLmSessionConfig* config = litert_lm_session_config_create();
    if (!config) return nullptr;
    // JSON-Metadaten brauchen max. ~300 Tokens — aggressiveres Limit
    // verhindert unnötiges Generieren und beschleunigt Early-Stopping.
    litert_lm_session_config_set_max_output_tokens(config, 512);
    return config;
}

// ---------------------------------------------------------------------------
// C-API Implementierung
// ---------------------------------------------------------------------------

extern "C" {

LiteRTEngineRef litert_engine_create(const char* model_path, const char* cache_dir) {
    if (!model_path) {
        fprintf(stderr, "[LiteRTBridge] litert_engine_create: model_path ist NULL\n");
        return nullptr;
    }

    fprintf(stderr, "[LiteRTBridge] Cache-Dir: %s\n", cache_dir ? cache_dir : "(keiner)");

    LiteRTEngineHandle* handle = nullptr;
    try {
        handle = new LiteRTEngineHandle();

        // 1. Settings mit bestem verfügbaren Backend (GPU → CPU Fallback)
        handle->lm_settings = create_settings_with_best_backend(model_path, cache_dir);
        if (!handle->lm_settings) {
            fprintf(stderr, "[LiteRTBridge] Alle Backends fehlgeschlagen\n");
            delete handle;
            return nullptr;
        }

        // 2. Engine instanziieren (blockierend — Modell wird geladen)
        handle->lm_engine = litert_lm_engine_create(handle->lm_settings);
        if (!handle->lm_engine) {
            fprintf(stderr, "[LiteRTBridge] litert_lm_engine_create fehlgeschlagen — %s\n",
                    model_path);
            litert_lm_engine_settings_delete(handle->lm_settings);
            delete handle;
            return nullptr;
        }

        // 3. Persistente Session anlegen (wird bei jeder Inferenz wiederverwendet)
        LiteRtLmSessionConfig* session_config = create_session_config();
        handle->lm_session = litert_lm_engine_create_session(
            handle->lm_engine, session_config);
        if (session_config) litert_lm_session_config_delete(session_config);

        if (!handle->lm_session) {
            fprintf(stderr, "[LiteRTBridge] Session-Init fehlgeschlagen\n");
            litert_lm_engine_delete(handle->lm_engine);
            litert_lm_engine_settings_delete(handle->lm_settings);
            delete handle;
            return nullptr;
        }

        handle->is_initialized = true;
        fprintf(stderr, "[LiteRTBridge] Engine + Session bereit: %s\n", model_path);
        return static_cast<LiteRTEngineRef>(handle);

    } catch (const std::exception& e) {
        fprintf(stderr, "[LiteRTBridge] litert_engine_create Exception: %s\n", e.what());
        if (handle) {
            if (handle->lm_session)  litert_lm_session_delete(handle->lm_session);
            if (handle->lm_engine)   litert_lm_engine_delete(handle->lm_engine);
            if (handle->lm_settings) litert_lm_engine_settings_delete(handle->lm_settings);
            delete handle;
        }
        return nullptr;
    } catch (...) {
        fprintf(stderr, "[LiteRTBridge] litert_engine_create: unbekannte Exception\n");
        if (handle) {
            if (handle->lm_session)  litert_lm_session_delete(handle->lm_session);
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
    if (handle->lm_session)  litert_lm_session_delete(handle->lm_session);
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

    if (!handle->is_initialized || !handle->lm_engine || !handle->lm_session) {
        fprintf(stderr, "[LiteRTBridge] litert_engine_send_message: Engine nicht initialisiert\n");
        return nullptr;
    }

    LiteRtLmResponses* responses = nullptr;

    try {
        // Persistente Session direkt nutzen — kein Create/Delete-Overhead.
        // Die Session ist stateless zwischen unabhängigen Dokument-Scans:
        // jeder Aufruf startet bei leerem KV-Cache (kein Kontext-Übertrag).
        InputData input;
        input.type = kInputText;
        input.data = static_cast<const void*>(message);
        input.size = strlen(message);

        responses = litert_lm_session_generate_content(handle->lm_session, &input, 1);
        if (!responses) {
            fprintf(stderr, "[LiteRTBridge] litert_lm_session_generate_content fehlgeschlagen\n");
            return nullptr;
        }

        const int num_candidates = litert_lm_responses_get_num_candidates(responses);
        if (num_candidates == 0) {
            fprintf(stderr, "[LiteRTBridge] Keine Antwort-Kandidaten\n");
            litert_lm_responses_delete(responses);
            return nullptr;
        }

        const char* text = litert_lm_responses_get_response_text_at(responses, 0);
        if (!text) {
            litert_lm_responses_delete(responses);
            return nullptr;
        }

        const size_t len    = strlen(text);
        char*        result = static_cast<char*>(malloc(len + 1));
        if (!result) {
            litert_lm_responses_delete(responses);
            return nullptr;
        }
        memcpy(result, text, len + 1);
        litert_lm_responses_delete(responses);
        return result;

    } catch (const std::exception& e) {
        fprintf(stderr, "[LiteRTBridge] send_message Exception: %s\n", e.what());
        if (responses) litert_lm_responses_delete(responses);
        return nullptr;
    } catch (...) {
        if (responses) litert_lm_responses_delete(responses);
        return nullptr;
    }
}

void litert_free_string(const char* str) {
    free(const_cast<char*>(str));
}

} // extern "C"
