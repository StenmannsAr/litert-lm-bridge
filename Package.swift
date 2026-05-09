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
            url: "https://github.com/StenmannsAr/litert-lm-bridge/releases/download/v2.0.0/LiteRTLMVendor.xcframework.zip",
            checksum: "f4b24d84553425f5a8bcd40b90372e75b88abea1cf7c21c25e1570efc73e8fa4"
        ),

        // Statische Lib (libGemmaModelConstraintProvider) — stub, kein dylib → keine ITMS-Fehler
        .binaryTarget(
            name: "LiteRTLMGemma",
            url: "https://github.com/StenmannsAr/litert-lm-bridge/releases/download/v2.0.0/LiteRTLMGemma.xcframework.zip",
            checksum: "506eb412c21dd8d405a51a43cf2483b38f6d27795ebd9d3efd5dc024388c7ecc"
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
