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
        // Vorkompilierte statische Libs (liblitert_lm.a + libengine_init.a)
        .binaryTarget(
            name: "LiteRTLMVendor",
            url: "https://github.com/StenmannsAr/litert-lm-bridge/releases/download/v1.0.1/LiteRTLMVendor.xcframework.zip",
            checksum: "eb556928a38237577e32a698fe488c22719a1ecfddac00ca605dd17ee7ee1571"
        ),

        // Vorkompilierte dylib (libGemmaModelConstraintProvider) — enthält dSYMs
        .binaryTarget(
            name: "LiteRTLMGemma",
            url: "https://github.com/StenmannsAr/litert-lm-bridge/releases/download/v1.0.4/LiteRTLMGemma.xcframework.zip",
            checksum: "3aac33f546aedeb86e03a5eb0ba8dd2c82a1c938d8825a8b929d2eb217278fd4"
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
