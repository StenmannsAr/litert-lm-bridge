#ifndef LITERT_BRIDGE_H
#define LITERT_BRIDGE_H

#ifdef __cplusplus
extern "C" {
#endif

/// Opakes Handle auf eine LiteRT-LM Engine-Instanz.
/// Intern ein Zeiger auf LiteRTEngineHandle (siehe LiteRTBridge.cpp).
typedef void* LiteRTEngineRef;

/// Erstellt eine neue Engine-Instanz und lädt das Modell von `model_path`.
/// `cache_dir` ist ein optionaler Pfad zu einem schreibbaren Verzeichnis für
/// XNNPack-Caches (z. B. Library/Caches der App). NULL = kein externer Cache.
/// Gibt NULL zurück wenn das Modell nicht geladen werden konnte.
/// Der Aufrufer ist verantwortlich, die Instanz mit litert_engine_destroy freizugeben.
LiteRTEngineRef litert_engine_create(const char* model_path, const char* cache_dir);

/// Gibt alle Ressourcen der Engine-Instanz frei.
/// Sicher auf NULL zu rufen.
void litert_engine_destroy(LiteRTEngineRef engine);

/// Sendet eine Nachricht an das Modell und gibt die Antwort als C-String zurück.
/// Der zurückgegebene String muss mit litert_free_string freigegeben werden.
/// Gibt NULL zurück bei einem Fehler.
const char* litert_engine_send_message(LiteRTEngineRef engine, const char* message);

/// Gibt einen String frei der von litert_engine_send_message alloziert wurde.
/// Sicher auf NULL zu rufen.
void litert_free_string(const char* str);

#ifdef __cplusplus
}
#endif

#endif /* LITERT_BRIDGE_H */
