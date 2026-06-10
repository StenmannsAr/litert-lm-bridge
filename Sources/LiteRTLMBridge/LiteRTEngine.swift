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

    // MARK: Simulator: LM Studio (localhost) mit Canned-Fallback (#140)
    //
    // Im Simulator gibt es keine native LiteRT-Runtime. Statt nur hardcodiertem JSON
    // wird die Anfrage an LM Studios lokalen OpenAI-kompatiblen Server auf dem Host-Mac
    // geschickt (der Simulator teilt das Host-Netzwerk → 127.0.0.1 = Mac). So lassen
    // sich KI-Funktionen (Tag-Merge, Metadaten-Extraktion) mit echter Gemma-4-E2B-
    // Inferenz im Simulator testen. Läuft kein Server, greift deterministisch die
    // Canned-Antwort — Unit-Tests injizieren ohnehin eigene Mocks und treffen diesen
    // Pfad nicht. Strikt simulator-only, wird fürs Gerät wegkompiliert.

    #if targetEnvironment(simulator)

    public init(modelPath: String, cacheDir: String? = nil,
                enableSpeculativeDecoding: Bool = false) throws {
        // Kein echtes Modell im Simulator
    }

    /// Deterministische Fallback-Antwort (bisheriger Mock), wenn kein LM-Studio-Server läuft.
    private static let cannedResponse = """
    {"korrespondent":"Stadtwerke Musterstadt","titel":"Stromrechnung März 2026",\
    "dokumenttyp":"rechnung","tags":["strom","energie"],"datum":"2026-03-31","konfidenz":0.94}
    """

    public func sendMessage(_ message: String) async throws -> String {
        let env = ProcessInfo.processInfo.environment
        let urlString = env["LITERT_SIM_LLM_URL"] ?? "http://127.0.0.1:1234/v1/chat/completions"
        let model = env["LITERT_SIM_LLM_MODEL"] ?? "google/gemma-4-e2b"
        guard let url = URL(string: urlString) else { return Self.cannedResponse }

        var request = URLRequest(url: url)
        request.httpMethod = "POST"
        request.setValue("application/json", forHTTPHeaderField: "Content-Type")
        // LM Studio kann Authentifizierung verlangen ("Require API token") —
        // Token optional via Env mitgeben, sonst lehnt der Server mit invalid_api_key ab.
        if let token = env["LITERT_SIM_LLM_TOKEN"], !token.isEmpty {
            request.setValue("Bearer \(token)", forHTTPHeaderField: "Authorization")
        }
        request.timeoutInterval = 120
        // Sampler-Parameter analog zur On-Device-Engine (TOP_P 0.95, temperature 1.0)
        let body: [String: Any] = [
            "model": model,
            "messages": [["role": "user", "content": message]],
            "temperature": 1.0,
            "top_p": 0.95,
            "stream": false
        ]
        request.httpBody = try? JSONSerialization.data(withJSONObject: body)

        do {
            let (data, response) = try await URLSession.shared.data(for: request)
            guard let http = response as? HTTPURLResponse, (200...299).contains(http.statusCode),
                  let json = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
                  let choices = json["choices"] as? [[String: Any]],
                  let messageObj = choices.first?["message"] as? [String: Any],
                  let content = messageObj["content"] as? String,
                  !content.isEmpty
            else {
                // Server erreichbar, aber Antwort unbrauchbar (kein Modell geladen o. ä.)
                return Self.cannedResponse
            }
            return content
        } catch {
            // LM Studio läuft nicht → deterministischer Fallback, kein Fehler/Crash
            return Self.cannedResponse
        }
    }

    // MARK: Echte Engine (iOS-Gerät)

    #else

    private let inferenceQueue = DispatchQueue(
        label: "de.litert-lm-bridge.inference",
        qos: .userInitiated
    )

    private nonisolated(unsafe) let engineRef: LiteRTEngineRef

    public init(modelPath: String, cacheDir: String? = nil,
                enableSpeculativeDecoding: Bool = false) throws {
        guard let ref = litert_engine_create(modelPath, cacheDir, enableSpeculativeDecoding) else {
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
