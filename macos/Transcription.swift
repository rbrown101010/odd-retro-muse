import Foundation

struct Transcription {
    static let model = "gpt-transcribe"
    static func body(audio: Data, boundary: String) -> Data {
        var data = Data()
        func append(_ value: String) { data.append(Data(value.utf8)) }
        for (name, value) in [("model", model), ("response_format", "json"),
            ("prompt", "Vocabulary: Muse, ModRetro, Chromatic, Odd Retro Muse, GitHub, OpenAI.")] {
            append("--\(boundary)\r\nContent-Disposition: form-data; name=\"\(name)\"\r\n\r\n\(value)\r\n")
        }
        append("--\(boundary)\r\nContent-Disposition: form-data; name=\"file\"; filename=\"question.m4a\"\r\nContent-Type: audio/mp4\r\n\r\n")
        data.append(audio)
        append("\r\n--\(boundary)--\r\n")
        return data
    }
    static func transcribe(_ audio: Data, key: String, session: URLSession = .shared) async throws -> String {
        guard !audio.isEmpty, audio.count < 24_000_000 else { throw AppError("Recording is empty or too large.") }
        let boundary = "OddRetroMuse-" + UUID().uuidString
        var request = URLRequest(url: URL(string: "https://api.openai.com/v1/audio/transcriptions")!)
        request.httpMethod = "POST"
        request.timeoutInterval = 90
        request.setValue("Bearer " + key, forHTTPHeaderField: "Authorization")
        request.setValue("multipart/form-data; boundary=" + boundary, forHTTPHeaderField: "Content-Type")
        request.httpBody = body(audio: audio, boundary: boundary)
        let (data, response) = try await session.data(for: request)
        let code = (response as? HTTPURLResponse)?.statusCode ?? 0
        switch code {
        case 200..<300: break
        case 401: throw AppError("OpenAI rejected the API key. Update it in Settings.")
        case 429: throw AppError("OpenAI rate limit or API balance reached. Check your OpenAI account and try again.")
        default: throw AppError("OpenAI transcription failed (HTTP \(code)). Your recording was not sent to Muse.")
        }
        struct Reply: Decodable { let text: String }
        let text = try JSONDecoder().decode(Reply.self, from: data).text.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !text.isEmpty else { throw AppError("No speech detected. Press A+B and try again.") }
        return text
    }
}
