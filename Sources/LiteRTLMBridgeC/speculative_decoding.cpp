// speculative_decoding.cpp
// Struct-Layout-kompatibler Shim für litert::lm::AdvancedSettings.
//
// Hintergrund: LiteRTLMVendor.a ist vorcompiliert — wir können keine neuen
// C++-Funktionen hinzufügen die litert::lm-interne Typen verwenden.
// SetAdvancedSettings() ist eine inline-Methode (kein Symbol in vendor.a).
// Lösung: Layout des LlmExecutorSettings-Structs via Disassemblierung ermitteln
// und advanced_settings_ per Placement New initialisieren.
//
// Verifikation (arm64-Disassemblierung von 712_engine.o in LiteRTLMVendor.a):
//   litert_lm_engine_settings_set_max_num_tokens:  str w1, [x0, #0xF0]
//   → max_num_tokens_ bei Offset 0xF0 = 240 ✓
//
//   SetAdvancedSettings wird über ldrb-Scan nach Byte-Offset 0x140+88 gefunden
//   → advanced_settings_ (optional<AdvancedSettings>) bei Offset 0x140 = 320 ✓
//
//   GetMutableMainExecutorSettings: ret
//   → gibt 'this' zurück → main_executor_settings_ bei Offset 0 ✓

// Kein #include "c/engine.h" direkt — stattdessen LiteRTBridgeExt.h, das
// c/engine.h korrekt einschließt OHNE durch xcframework-Include-Guards blockiert zu werden.
#include "LiteRTBridgeExt.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Stub-Layout für litert::lm::AdvancedSettings (arm64, Apple libc++ ABI)
//
// Wird exakt so angeordnet wie das Original-Struct im vorcompilierten Code.
// std::optional<bool> → 2 Bytes (value + engaged) → hier als 2×uint8_t.
// std::vector<int>    → 24 Bytes auf arm64 (3 Zeiger)
// std::string         → 24 Bytes auf arm64 (SSO-Union)
// ---------------------------------------------------------------------------

namespace litert_stub {

struct AdvancedSettings {
    std::vector<int>  prefill_batch_sizes;                    // +0,  24 B
    int               num_output_candidates              = 1; // +24,  4 B
    bool              configure_magic_numbers            = false; // +28
    bool              verify_magic_numbers               = false; // +29
    bool              clear_kv_cache_before_prefill      = false; // +30
    char              _pad1                              = 0;     // +31 (impl. padding)
    int               num_logits_to_print_after_decode   = 0;     // +32,  4 B
    bool              gpu_madvise_original_shared_tensors= false; // +36
    bool              is_benchmark                       = false; // +37
    char              _pad2[2]                           = {};    // +38 (impl. padding)
    std::string       preferred_device_substr;                    // +40, 24 B
    int               num_threads_to_upload              = 1;     // +64,  4 B
    int               num_threads_to_compile             = 1;     // +68,  4 B
    bool              convert_weights_on_gpu             = false; // +72
    bool              wait_for_weights_conversion_complete= false;// +73
    bool              optimize_shader_compilation        = false; // +74
    bool              cache_compiled_shaders_only        = false; // +75
    bool              share_constant_tensors             = true;  // +76
    bool              sampler_handles_input              = false; // +77
    bool              allow_src_quantized_fc_conv_ops    = false; // +78
    // std::optional<bool> hint_waiting_for_completion → val, engaged
    uint8_t           hint_waiting_for_completion_val    = 0;     // +79
    uint8_t           hint_waiting_for_completion_has    = 0;     // +80
    // std::optional<bool> gpu_context_low_priority → val, engaged
    uint8_t           gpu_context_low_priority_val       = 0;     // +81
    uint8_t           gpu_context_low_priority_has       = 0;     // +82
    bool              enable_speculative_decoding        = false; // +83
    // implicit padding +84..+87 → alignof = 8 (vector, string)
};

// Statische Assertions sichern Layout zur Compile-Zeit ab.
static_assert(offsetof(AdvancedSettings, num_output_candidates)         == 24,
    "AdvancedSettings: num_output_candidates offset mismatch");
static_assert(offsetof(AdvancedSettings, num_logits_to_print_after_decode) == 32,
    "AdvancedSettings: num_logits offset mismatch");
static_assert(offsetof(AdvancedSettings, preferred_device_substr)       == 40,
    "AdvancedSettings: preferred_device_substr offset mismatch");
static_assert(offsetof(AdvancedSettings, num_threads_to_upload)         == 64,
    "AdvancedSettings: num_threads_to_upload offset mismatch");
static_assert(offsetof(AdvancedSettings, share_constant_tensors)        == 76,
    "AdvancedSettings: share_constant_tensors offset mismatch");
static_assert(offsetof(AdvancedSettings, enable_speculative_decoding)   == 83,
    "AdvancedSettings: enable_speculative_decoding offset mismatch");
static_assert(sizeof(AdvancedSettings) == 88,
    "AdvancedSettings: sizeof mismatch — Drafter-Flag nicht erreichbar");

// std::optional<AdvancedSettings> Layout auf arm64 / libc++:
//   [0..87]  : alignas(8) Speicher für AdvancedSettings
//   [88]     : bool __engaged_
//   [89..95] : padding zu nächstem 8-Byte-Align → sizeof = 96
struct OptionalAdvancedSettings {
    alignas(8) char  storage[sizeof(AdvancedSettings)]; // +0, 88 B
    bool             engaged = false;                   // +88
    char             _pad[7] = {};                      // +89..+95
};
static_assert(sizeof(OptionalAdvancedSettings) == 96,
    "OptionalAdvancedSettings: sizeof mismatch");

// Offset von advanced_settings_ in LlmExecutorSettings (arm64).
// Verifiziert via Disassemblierung: ldrb/strb-Sequenzen ab Offset 0x140 + 88 (engaged-Flag).
static constexpr size_t kAdvancedSettingsOffset = 0x140;  // 320

} // namespace litert_stub

// ---------------------------------------------------------------------------
// Öffentliche C-Funktion (deklariert in c/engine.h)
//
// Aktiviert Speculative Decoding in einem LiteRtLmEngineSettings-Handle.
// Muss VOR litert_lm_engine_create aufgerufen werden.
//
// Ablauf:
//  1. LiteRtLmEngineSettings* → erster Zeiger = main_executor_settings_
//     (GetMutableMainExecutorSettings gibt 'this' zurück → Offset 0 bestätigt)
//  2. advanced_settings_ liegt bei Offset kAdvancedSettingsOffset
//  3. Falls optional noch nicht engaged: Placement New mit Defaults initialisieren
//  4. enable_speculative_decoding = true setzen
//
// Falls das Modell keinen kTfLiteMtpDrafter-Abschnitt enthält, fällt
// litert_lm_engine_create transparent auf AR-Decoding zurück.
// ---------------------------------------------------------------------------

extern "C"
void litert_lm_engine_settings_enable_speculative_decoding(
    LiteRtLmEngineSettings* handle)
{
    if (!handle) return;

    // Das erste Wort von LiteRtLmEngineSettings ist main_executor_settings_
    // (unique_ptr oder raw-ptr auf LlmExecutorSettings — erstes 8-Byte-Wort).
    void* es = *reinterpret_cast<void**>(handle);
    if (!es) return;

    auto* opt = reinterpret_cast<litert_stub::OptionalAdvancedSettings*>(
        static_cast<char*>(es) + litert_stub::kAdvancedSettingsOffset);

    if (!opt->engaged) {
        // Placement New: ruft AdvancedSettings-Default-Konstruktor auf
        // → initialisiert vector, string und alle skalaren Felder korrekt
        new (opt->storage) litert_stub::AdvancedSettings();
        opt->engaged = true;
    }

    auto* adv = reinterpret_cast<litert_stub::AdvancedSettings*>(opt->storage);
    adv->enable_speculative_decoding = true;

    fprintf(stderr, "[LiteRTBridge] Speculative Decoding (MTP Drafter) aktiviert\n");
}
