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
        // C++ Bridge — kompiliert LiteRTBridge.cpp und stellt LiteRTBridge.h bereit
        .target(
            name: "LiteRTLMBridgeC",
            path: "Sources/LiteRTLMBridgeC",
            sources: ["LiteRTBridge.cpp"],
            publicHeadersPath: "include",
            cxxSettings: [
                .headerSearchPath("include"),
            ],
            linkerSettings: [
                .linkedLibrary("c++"),
                .unsafeFlags([
                    "-force_load", "Vendor/libengine_init.a",
                    "-L", "Vendor",
                    "-llitert_lm",
                    "Vendor/libGemmaModelConstraintProvider.dylib",
                ], .when(platforms: [.iOS])),
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
