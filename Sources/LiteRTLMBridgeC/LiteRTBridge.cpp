// LiteRT-LM Bridge — Implementierung
//
// Kapselt die LiteRT-LM C-API hinter unserer eigenen schlanken C-Schnittstelle.
// Die LiteRT-LM C-API befindet sich unter:
//   $(SRCROOT)/../LiteRT-LM/c/engine.h
//
// Build-Abhängigkeiten (Xcode Build Settings):
//   HEADER_SEARCH_PATHS     += $(SRCROOT)/../LiteRT-LM
//   LIBRARY_SEARCH_PATHS    += $(SRCROOT)/Vendor
//   OTHER_LDFLAGS           += -llitert_lm -lc++

#include "LiteRTBridge.h"

// LiteRT-LM C-API (via HEADER_SEARCH_PATHS auf das Repo-Verzeichnis gesetzt)
#include "c/engine.h"

// Erzwingt dass der Linker engine_impl.o aus liblitert_lm.a einschließt.
// engine_impl.o enthält LITERT_LM_REGISTER_ENGINE(kLiteRTCompiledModel, ...)
// als statischen Initializer. Ohne Referenz auf ein öffentliches Symbol aus
// diesem Object-File würde der Linker es dead-strippen → EngineFactory leer
// → "Engine type not found: 1" beim ersten LLM-Aufruf.
extern "C" void litert_lm_force_register_engine_impl();
static const auto _force_engine_impl = (litert_lm_force_register_engine_impl(), 0);

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <mutex>
#include <string>

// ---------------------------------------------------------------------------
// Internes Handle — kapselt LiteRT-LM Engine + zugehörige Settings
// ---------------------------------------------------------------------------

struct LiteRTEngineHandle {
    LiteRtLmEngine*         lm_engine   = nullptr;
    LiteRtLmEngineSettings* lm_settings = nullptr;

    // Serialisiert Inferenz-Aufrufe aus Swift (defensive double-lock neben dem
    // internen Mutex der LiteRT-LM Session)
    std::mutex inference_mutex;

    bool is_initialized = false;

    LiteRTEngineHandle() = default;

    // Nicht kopierbar
    LiteRTEngineHandle(const LiteRTEngineHandle&)            = delete;
    LiteRTEngineHandle& operator=(const LiteRTEngineHandle&) = delete;
};

// ---------------------------------------------------------------------------
// C-API Implementierung
// ---------------------------------------------------------------------------

extern "C" {

LiteRTEngineRef litert_engine_create(const char* model_path, const char* cache_dir) {
    if (!model_path) {
        fprintf(stderr, "[LiteRTBridge] litert_engine_create: model_path ist NULL\n");
        return nullptr;
    }

    LiteRTEngineHandle* handle = nullptr;
    try {
        handle = new LiteRTEngineHandle();

        // 1. Settings mit CPU-Backend erstellen
        handle->lm_settings = litert_lm_engine_settings_create(
            model_path,
            "cpu",   // Backend: CPU (kein GPU, kein NPU)
            nullptr, // Vision-Backend: nicht benötigt
            nullptr  // Audio-Backend: nicht benötigt
        );

        if (!handle->lm_settings) {
            fprintf(stderr, "[LiteRTBridge] litert_lm_engine_settings_create fehlgeschlagen\n");
            delete handle;
            return nullptr;
        }

        // Maximale Token-Anzahl: System-Prompt (~500) + OCR-Text (3000) + Antwort (200)
        litert_lm_engine_settings_set_max_num_tokens(handle->lm_settings, 4096);

        // Optionaler Cache-Pfad (XNNPack, Tokenizer-Cache usw.)
        // Muss ein schreibbares Verzeichnis sein — im App-Bundle ist alles read-only.
        if (cache_dir && *cache_dir != '\0') {
            litert_lm_engine_settings_set_cache_dir(handle->lm_settings, cache_dir);
            fprintf(stderr, "[LiteRTBridge] Cache-Dir: %s\n", cache_dir);
        }

        // 2. Engine instanziieren und Modell laden (blockierend, evtl. mehrere Sekunden)
        handle->lm_engine = litert_lm_engine_create(handle->lm_settings);

        if (!handle->lm_engine) {
            fprintf(stderr, "[LiteRTBridge] litert_lm_engine_create fehlgeschlagen — Modellpfad: %s\n",
                    model_path);
            litert_lm_engine_settings_delete(handle->lm_settings);
            delete handle;
            return nullptr;
        }

        handle->is_initialized = true;
        fprintf(stderr, "[LiteRTBridge] Engine initialisiert: %s\n", model_path);

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

    // Reihenfolge: Session → Engine → Settings
    if (handle->lm_engine)   litert_lm_engine_delete(handle->lm_engine);
    if (handle->lm_settings) litert_lm_engine_settings_delete(handle->lm_settings);

    delete handle;
}

const char* litert_engine_send_message(LiteRTEngineRef engine, const char* message) {
    if (!engine) {
        fprintf(stderr, "[LiteRTBridge] litert_engine_send_message: engine ist NULL\n");
        return nullptr;
    }
    if (!message) {
        fprintf(stderr, "[LiteRTBridge] litert_engine_send_message: message ist NULL\n");
        return nullptr;
    }

    LiteRTEngineHandle* handle = static_cast<LiteRTEngineHandle*>(engine);
    std::lock_guard<std::mutex> lock(handle->inference_mutex);

    if (!handle->is_initialized || !handle->lm_engine) {
        fprintf(stderr, "[LiteRTBridge] litert_engine_send_message: Engine nicht initialisiert\n");
        return nullptr;
    }

    LiteRtLmSession*  session   = nullptr;
    LiteRtLmResponses* responses = nullptr;

    try {
        // Pro Inferenz eine neue Session: stateless, kein Kontext aus vorherigen
        // Dokumenten. Vermeidet KV-Cache-Überlauf bei Serienscans.
        session = litert_lm_engine_create_session(handle->lm_engine, nullptr);
        if (!session) {
            fprintf(stderr, "[LiteRTBridge] litert_lm_engine_create_session fehlgeschlagen\n");
            return nullptr;
        }

        // Eingabe als Text-InputData vorbereiten
        InputData input;
        input.type = kInputText;
        input.data = static_cast<const void*>(message);
        input.size = strlen(message);

        // Synchrone Inferenz (blockiert inference_queue in LiteRTEngine.swift)
        responses = litert_lm_session_generate_content(session, &input, 1);

        if (!responses) {
            fprintf(stderr, "[LiteRTBridge] litert_lm_session_generate_content fehlgeschlagen\n");
            litert_lm_session_delete(session);
            return nullptr;
        }

        // Ersten Kandidaten extrahieren
        const int num_candidates = litert_lm_responses_get_num_candidates(responses);
        if (num_candidates == 0) {
            fprintf(stderr, "[LiteRTBridge] Keine Antwort-Kandidaten\n");
            litert_lm_responses_delete(responses);
            litert_lm_session_delete(session);
            return nullptr;
        }

        const char* text = litert_lm_responses_get_response_text_at(responses, 0);
        if (!text) {
            fprintf(stderr, "[LiteRTBridge] Antwort-Text ist NULL\n");
            litert_lm_responses_delete(responses);
            litert_lm_session_delete(session);
            return nullptr;
        }

        // Text aus `responses`-Lifetime heraus kopieren (Aufrufer ist verantwortlich
        // für Free via litert_free_string)
        const size_t len    = strlen(text);
        char*        result = static_cast<char*>(malloc(len + 1));
        if (!result) {
            fprintf(stderr, "[LiteRTBridge] malloc fehlgeschlagen\n");
            litert_lm_responses_delete(responses);
            litert_lm_session_delete(session);
            return nullptr;
        }
        memcpy(result, text, len + 1);

        litert_lm_responses_delete(responses);
        litert_lm_session_delete(session);

        return result;

    } catch (const std::exception& e) {
        fprintf(stderr, "[LiteRTBridge] litert_engine_send_message Exception: %s\n", e.what());
        if (responses) litert_lm_responses_delete(responses);
        if (session)   litert_lm_session_delete(session);
        return nullptr;
    } catch (...) {
        fprintf(stderr, "[LiteRTBridge] litert_engine_send_message: unbekannte Exception\n");
        if (responses) litert_lm_responses_delete(responses);
        if (session)   litert_lm_session_delete(session);
        return nullptr;
    }
}

void litert_free_string(const char* str) {
    // Symmetrisch zu malloc in litert_engine_send_message
    free(const_cast<char*>(str));
}

} // extern "C"
