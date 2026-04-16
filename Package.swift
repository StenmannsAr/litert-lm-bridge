// swift-tools-version: 6.3
import PackageDescription

let package = Package(
    name: "LiteRTLMBridge",
    platforms: [.iOS(.v26)],
    products: [
        .library(
            name: "LiteRTLMBridge",
            targets: ["LiteRTLMBridge"]
        ),
    ],
    targets: [
        // Vorkompilierte statische Libs — engine_impl.o mit Registration-Hook
        // (litert_lm_force_register_engine_impl verhindert Dead-Stripping)
        .binaryTarget(
            name: "LiteRTLMVendor",
            url: "https://github.com/StenmannsAr/litert-lm-bridge/releases/download/v1.0.2/LiteRTLMVendor.xcframework.zip",
            checksum: "200ce1b46ca3bda9c366fa98157c7d1be5e3f8f77ccd0a73a81655b9d537006a"
        ),

        // Statische Lib (libGemmaModelConstraintProvider) — stub, kein dylib → keine ITMS-Fehler
        .binaryTarget(
            name: "LiteRTLMGemma",
            url: "https://github.com/StenmannsAr/litert-lm-bridge/releases/download/v1.0.5/LiteRTLMGemma.xcframework.zip",
            checksum: "2f070cfa2eca9f3440604491d0e65e246455f6b5894f0873a3717a6c8b284d1c"
        ),

        // C++ Bridge — kompiliert LiteRTBridge.cpp
        .target(
            name: "LiteRTLMBridgeC",
            dependencies: ["LiteRTLMVendor", "LiteRTLMGemma"],
            path: "Sources/LiteRTLMBridgeC",
            sources: ["LiteRTBridge.cpp"],
            publicHeadersPath: "include",
            cxxSettings: [
                .headerSearchPath("include"),
            ],
            linkerSettings: [
                .linkedLibrary("c++"),
            ]
        ),

        // Swift-Wrapper — öffentliche API für App-Targets
        .target(
            name: "LiteRTLMBridge",
            dependencies: ["LiteRTLMBridgeC"],
            path: "Sources/LiteRTLMBridge"
        ),
    ],
    cxxLanguageStandard: .cxx17
)
