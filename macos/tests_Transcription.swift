import Foundation
final class FakeProtocol: URLProtocol {
    static var code = 200
    static var response = "{\"text\":\"Muse, build something interesting.\"}"
    static var requests = 0
    static var hold = false
    override class func canInit(with request: URLRequest) -> Bool { true }
    override class func canonicalRequest(for request: URLRequest) -> URLRequest { request }
    override func startLoading() {
        FakeProtocol.requests += 1
        if FakeProtocol.hold { return }
        let reply = HTTPURLResponse(url: request.url!, statusCode: FakeProtocol.code, httpVersion: "HTTP/1.1", headerFields: nil)!
        client?.urlProtocol(self, didReceive: reply, cacheStoragePolicy: .notAllowed)
        client?.urlProtocol(self, didLoad: Data(FakeProtocol.response.utf8))
        client?.urlProtocolDidFinishLoading(self)
    }
    override func stopLoading() {}
}
@main struct TranscriptionTests {
    static func main() async throws {
        let config = URLSessionConfiguration.ephemeral; config.protocolClasses = [FakeProtocol.self]
        let session = URLSession(configuration: config)
        let fixture = Data([0,1,2,13,10,255,0])
        let body = Transcription.body(audio: fixture, boundary: "test-boundary")
        precondition(body.range(of: fixture) != nil)
        precondition(body.range(of: Data("name=\"model\"\r\n\r\ngpt-transcribe".utf8)) != nil)
        precondition(body.suffix(19) == Data("--test-boundary--\r\n".utf8).suffix(19))
        let value = try await Transcription.transcribe(fixture, key: "test-key", session: session)
        precondition(value == "Muse, build something interesting.")
        for (code, word) in [(401,"API key"),(429,"balance"),(500,"HTTP 500")] {
            FakeProtocol.code = code
            do { _ = try await Transcription.transcribe(fixture,key:"test-key",session:session); fatalError("Expected failure") }
            catch { precondition(error.localizedDescription.contains(word)) }
        }
        FakeProtocol.code = 200; FakeProtocol.response = "{\"text\":\"  \"}"
        do { _ = try await Transcription.transcribe(fixture,key:"test-key",session:session);fatalError("Expected empty-transcript failure") }
        catch { precondition(error.localizedDescription.contains("No speech")) }
        let count = FakeProtocol.requests
        do { _ = try await Transcription.transcribe(Data(),key:"test-key",session:session);fatalError("Expected empty-audio rejection") }
        catch { precondition(FakeProtocol.requests == count) }
        FakeProtocol.hold = true
        let pending = Task { try await Transcription.transcribe(fixture,key:"test-key",session:session) }
        try await Task.sleep(nanoseconds: 50_000_000)
        pending.cancel()
        do { _ = try await pending.value;fatalError("Cancelled transcription produced text") }
        catch { precondition((error as? URLError)?.code == .cancelled || error is CancellationError) }
        print("Native transcription tests passed: multipart audio integrity, successful transcript, authentication/balance/server errors, and empty-input handling.")
    }
}
