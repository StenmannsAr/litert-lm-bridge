#ifndef LITERT_BRIDGE_EXT_H
#define LITERT_BRIDGE_EXT_H

// LiteRTBridgeExt.h — Bridge-Erweiterungen der LiteRT-LM C-API
//
// Diese Funktionen SIND NICHT in der vorcompilierten LiteRTLMVendor.xcframework.
// Sie werden in den Bridge-Source-Dateien (speculative_decoding.cpp) implementiert.
// Deklaration hier (statt in c/engine.h) um Include-Guard-Konflikte mit der
// xcframework-Version von c/engine.h zu vermeiden.

#include "c/engine.h"

#ifdef __cplusplus
extern "C" {
#endif

// Aktiviert Speculative Decoding (MTP Drafter) in den Engine-Settings.
// Implementiert in speculative_decoding.cpp via Struct-Layout-Shim.
// Muss VOR litert_lm_engine_create auf dem Settings-Handle aufgerufen werden.
// Falls das Modell keinen kTfLiteMtpDrafter-Abschnitt enthält, fällt
// litert_lm_engine_create transparent auf AR-Decoding zurück.
void litert_lm_engine_settings_enable_speculative_decoding(
    LiteRtLmEngineSettings* settings);

#ifdef __cplusplus
}
#endif

#endif /* LITERT_BRIDGE_EXT_H */
