// swift-tools-version: 6.3
import PackageDescription

let package = Package(
    name: "LiteRTLMBridge",
    platforms: [.iOS(.v26), .macCatalyst(.v26)],
    products: [
        .library(
            name: "LiteRTLMBridge",
            targets: ["LiteRTLMBridge"]
        ),
    ],
    targets: [
        // Vorkompilierte statische Libs — engine_impl.o mit GPU-Support +
        // Registration-Hook (litert_lm_force_register_engine_impl)
        .binaryTarget(
            name: "LiteRTLMVendor",
            url: "https://github.com/StenmannsAr/litert-lm-bridge/releases/download/v2.0.1/LiteRTLMVendor.xcframework.zip",
            checksum: "1d00bff5d9b5d51c9d74483102a08c39d7b49c8093bcaf59c4299bad6fc3934b"
        ),

        // Statische Lib (libGemmaModelConstraintProvider) — stub, kein dylib → keine ITMS-Fehler
        .binaryTarget(
            name: "LiteRTLMGemma",
            url: "https://github.com/StenmannsAr/litert-lm-bridge/releases/download/v2.0.1/LiteRTLMGemma.xcframework.zip",
            checksum: "9bff056da8a9c10acf25f2da93ce2a0275f4d887678b3125c3dcaba6e5ff932f"
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
