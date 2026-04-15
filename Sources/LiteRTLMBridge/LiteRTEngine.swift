import Foundation
import LiteRTLMBridgeC

// MARK: - Fehler

public enum LiteRTError: Error, LocalizedError {
    case engineInitFailed(modelPath: String)
    case inferenceFailed
    case invalidResponse

    public var errorDescription: String? {
        switch self {
        case .engineInitFailed(let path):
            return "LiteRT-LM Engine konnte nicht initialisiert werden: \(path)"
        case .inferenceFailed:
            return "Inferenz fehlgeschlagen"
        case .invalidResponse:
            return "Ungültige Antwort vom Modell"
        }
    }
}

// MARK: - Engine

/// Thread-sichere Wrapper-Klasse um die LiteRT-LM C-Bridge.
/// Führt Inferenz auf einem dedizierten Hintergrund-Thread durch.
public final class LiteRTEngine: @unchecked Sendable {

    // MARK: Simulator-Mock

    #if targetEnvironment(simulator)

    public init(modelPath: String, cacheDir: String? = nil) throws {
        // Kein echtes Modell im Simulator
    }

    public func sendMessage(_ message: String) async throws -> String {
        try await Task.sleep(for: .seconds(0.5))
        return """
        {"korrespondent":"Stadtwerke Musterstadt","titel":"Stromrechnung März 2026",\
        "dokumenttyp":"rechnung","tags":["strom","energie"],"datum":"2026-03-31","konfidenz":0.94}
        """
    }

    // MARK: Echte Engine (iOS-Gerät)

    #else

    private let inferenceQueue = DispatchQueue(
        label: "de.litert-lm-bridge.inference",
        qos: .userInitiated
    )

    private nonisolated(unsafe) let engineRef: LiteRTEngineRef

    public init(modelPath: String, cacheDir: String? = nil) throws {
        guard let ref = litert_engine_create(modelPath, cacheDir) else {
            throw LiteRTError.engineInitFailed(modelPath: modelPath)
        }
        self.engineRef = ref
    }

    deinit {
        litert_engine_destroy(engineRef)
    }

    public func sendMessage(_ message: String) async throws -> String {
        struct Ref: @unchecked Sendable { let r: LiteRTEngineRef }
        let ref = Ref(r: engineRef)
        return try await withCheckedThrowingContinuation { continuation in
            inferenceQueue.async {
                guard let cResponse = litert_engine_send_message(ref.r, message) else {
                    continuation.resume(throwing: LiteRTError.inferenceFailed)
                    return
                }
                let response = String(cString: cResponse)
                litert_free_string(cResponse)

                guard !response.isEmpty else {
                    continuation.resume(throwing: LiteRTError.invalidResponse)
                    return
                }
                continuation.resume(returning: response)
            }
        }
    }

    #endif
}
