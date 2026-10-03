import Foundation

struct BridgeStatus: Decodable {
    let wireless: Bool
    let muse_connected: Bool
    let service_session: String
    let device_connected: Bool
    let busy: Bool?
}
struct BridgeEvent: Decodable { let id: Int; let type: String; let text: String }
struct VoiceLease: Decodable { let cursor: Int }
struct Bridge {
    static let base = "http://localhost:8765"
    static func request(_ path: String, body: [String: String]? = nil) async throws -> Data {
        var request = URLRequest(url: URL(string: base + path)!)
        request.timeoutInterval = 8
        if let body = body {
            request.httpMethod = "POST"
            request.setValue("application/json", forHTTPHeaderField: "Content-Type")
            request.setValue(base, forHTTPHeaderField: "Origin")
            request.httpBody = try JSONSerialization.data(withJSONObject: body)
        }
        let (data, response) = try await URLSession.shared.data(for: request)
        let code = (response as? HTTPURLResponse)?.statusCode ?? 0
        guard (200..<300).contains(code) else {
            let obj = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any]
            throw AppError(obj?["error"] as? String ?? "Companion connection failed (\(code)).")
        }
        return data
    }
    static func get<T: Decodable>(_ path: String, as type: T.Type) async throws -> T {
        try JSONDecoder().decode(type, from: await request(path))
    }
}
