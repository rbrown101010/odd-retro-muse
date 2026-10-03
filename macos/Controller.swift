import SwiftUI
import AVFoundation
import AppKit

@MainActor final class Controller: ObservableObject {
    @Published var connected = false
    @Published var wireless = false
    @Published var armed = false
    @Published var state = "ready"
    @Published var draft = ""
    @Published var reply = ""
    @Published var suggestions: [String] = []
    @Published var message = "Connecting to your Chromatic…"
    @Published var error = ""
    @Published var keyConfigured = false
    @Published var level: Float = 0
    @Published var elapsed: Double = 0
    @Published var museBusy = false
    @Published var settingsVisible = false
    let client: String = {
        let defaults = UserDefaults.standard
        if let saved = defaults.string(forKey: "microphoneClient"), UUID(uuidString: saved) != nil { return saved }
        let id = UUID().uuidString; defaults.set(id, forKey: "microphoneClient"); return id
    }()
    private var cursor = 0
    private var service = ""
    private var generation = 0
    private var recorder: MicrophoneRecorder?
    @Published var microphones = MicrophoneDevice.available()
    @Published var microphoneID: UInt32 = 0
    private var recordingStarted = Date.distantPast
    private var audioURL: URL?
    private var polling: Task<Void, Never>?
    private var processing: Task<Void, Never>?
    private var feedbackTask: Task<Void, Never>?
    private var meter: Timer?
    private var peak: Float = -160
    private var lastHeartbeat = Date.distantPast
    private var restoring = false
    private var retryAfter = Date.distantPast
    private let autoMic = "automaticMicrophone"

    init() {
        keyConfigured = Keychain.exists()
        let saved = UInt32(UserDefaults.standard.integer(forKey: "microphoneDevice"))
        microphoneID = microphones.first(where: { $0.id == saved })?.id
            ?? microphones.first(where: { $0.builtIn })?.id ?? 0
        // Clear only stale recordings created by this app after an unexpected exit.
        let directory = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("Chromatic Muse/Recordings")
        for file in (try? FileManager.default.contentsOfDirectory(at: directory, includingPropertiesForKeys: nil)) ?? [] {
            if ["m4a", "wav"].contains(file.pathExtension), UUID(uuidString: file.deletingPathExtension().lastPathComponent) != nil {
                try? FileManager.default.removeItem(at: file)
            }
        }
        let start = Process()
        start.executableURL = URL(fileURLWithPath: "/bin/launchctl")
        start.arguments = ["kickstart", "gui/\(getuid())/com.riley.chromatic-muse"]
        start.standardOutput = FileHandle.nullDevice; start.standardError = FileHandle.nullDevice
        try? start.run()
        polling = Task { [weak self] in
            while !Task.isCancelled {
                await self?.poll()
                try? await Task.sleep(nanoseconds: 250_000_000)
            }
        }
    }
    func saveKey(_ key: String) {
        let value = key.trimmingCharacters(in: .whitespacesAndNewlines)
        guard value.hasPrefix("sk-"), !value.contains("\n"), value.count > 20 else {
            error = "Enter a valid OpenAI API key."; return
        }
        do { try Keychain.save(value); keyConfigured = true; error = ""; settingsVisible = false }
        catch { self.error = error.localizedDescription }
    }
    private func voice(_ op: String, _ value: String? = nil) async throws -> Data {
        var body = ["op": op, "client_id": client]
        if let value = value { body["state"] = value }
        return try await Bridge.request("/api/voice", body: body)
    }
    private func feedback(_ value: String) {
        let previous = feedbackTask
        feedbackTask = Task { [weak self] in
            await previous?.value
            guard let self = self, self.armed else { return }
            do { _ = try await self.voice("state", value) }
            catch { self.error = error.localizedDescription }
        }
    }
    func enableMicrophone() async {
        guard connected else { error = "Power on the Chromatic and wait for Muse to connect."; return }
        guard keyConfigured else { settingsVisible = true; return }
        guard !restoring else { return }
        restoring = true; defer { restoring = false }
        let allowed = await AVCaptureDevice.requestAccess(for: .audio)
        guard allowed else {
            error = "Allow microphone access for Odd Retro Muse in System Settings → Privacy & Security → Microphone."
            return
        }
        do {
            let data = try await voice("arm")
            let lease = try JSONDecoder().decode(VoiceLease.self, from: data)
            cursor = max(cursor, lease.cursor)
            armed = true; UserDefaults.standard.set(true, forKey: autoMic)
            lastHeartbeat = Date(); error = ""
            message = "Ready. A+B starts recording; A+B again sends. B cancels."
        } catch { self.error = error.localizedDescription; retryAfter = Date().addingTimeInterval(5) }
    }
    func disableMicrophone() async {
        cancelRecording()
        UserDefaults.standard.set(false, forKey: autoMic)
        if armed { _ = try? await voice("disarm") }
        armed = false; message = "Microphone disabled."
    }
    private func poll() async {
        do {
            let status = try await Bridge.get("/api/status", as: BridgeStatus.self)
            let changed = service != status.service_session
            if changed {
                service = status.service_session; cursor = 0
                if armed { cancelRecording(); armed = false }
            }
            let wasConnected = connected
            connected = status.muse_connected && status.device_connected
            wireless = status.wireless; museBusy = status.busy ?? museBusy
            if connected && !wasConnected && state == "ready" && !museBusy {
                message = armed ? "Ready. A+B starts recording; A+B again sends. B cancels."
                    : "Connected. Type a question or enable the Mac microphone to use A+B."
            }
            if !connected && armed { cancelRecording(); armed = false }
            if armed && Date().timeIntervalSince(lastHeartbeat) > 10 {
                _ = try await voice("heartbeat"); lastHeartbeat = Date()
            }
            let rows = try await Bridge.get("/api/events?after=\(cursor)", as: [BridgeEvent].self)
            for event in rows where event.id > cursor {
                cursor = event.id
                switch event.type {
                case "voice_toggle": if armed { toggleRecording() }
                case "voice_cancel": if armed { cancelRecording() }
                case "busy": suggestions = []; museBusy = true; message = "Muse is thinking…"
                case "sent": message = "Muse received your question. Waiting for the reply…"
                case "reply", "push": suggestions = []; reply = event.text; museBusy = false; message = "Muse replied. Read it here or on your Chromatic."
                case "suggestions":
                    if let rows = try? JSONDecoder().decode([String].self, from: Data(event.text.utf8)), rows.count == 20 {
                        suggestions = rows
                    }
                case "error": error = event.text; museBusy = false
                default: break
                }
            }
            if !armed && connected && keyConfigured && Date() > retryAfter &&
                UserDefaults.standard.bool(forKey: autoMic) && AVCaptureDevice.authorizationStatus(for: .audio) == .authorized {
                await enableMicrophone()
            }
            if !connected && state == "ready" { message = "Chromatic offline. Reconnecting when power and Wi-Fi return…" }
        } catch {
            connected = false
            if armed { cancelRecording(); armed = false }
            message = "Reconnecting the background companion…"
        }
    }
    func selectMicrophone(_ id: UInt32) {
        guard state == "ready" else { return }
        microphoneID = id
        UserDefaults.standard.set(Int(id), forKey: "microphoneDevice")
        error = ""
    }
    func toggleRecording() {
        if state == "ready" { startRecording() }
        else if state == "listening" { stopRecording() }
    }
    private func startRecording() {
        guard armed, !museBusy, state == "ready" else { return }
        generation += 1; error = ""; draft = ""; peak = -160; elapsed = 0
        do {
            let folder = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
                .appendingPathComponent("Chromatic Muse/Recordings", isDirectory: true)
            try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true,
                attributes: [.posixPermissions: 0o700])
            let url = folder.appendingPathComponent(UUID().uuidString + ".wav")
            audioURL = url
            let r = MicrophoneRecorder()
            recorder = r
            try r.start(url: url, device: microphoneID)
            try FileManager.default.setAttributes([.posixPermissions: 0o600], ofItemAtPath: url.path)
            recordingStarted = Date(); state = "listening"
            message = "Listening. Speak near your Mac. Press A+B again to stop and send."
            feedback("listening")
            meter = Timer.scheduledTimer(withTimeInterval: 0.1, repeats: true) { [weak self] _ in
                Task { @MainActor in self?.updateMeter() }
            }
        } catch { self.error = error.localizedDescription; removeRecording(); state = "ready" }
    }
    private func updateMeter() {
        guard let r = recorder, state == "listening" else { return }
        let metrics = r.metrics()
        elapsed = metrics.elapsed; peak = metrics.peak; level = metrics.level
        if let failure = metrics.failure { cancelRecording(); error = failure; return }
        if elapsed == 0 && Date().timeIntervalSince(recordingStarted) > 4 {
            cancelRecording(); error = "No audio is arriving from this microphone. Choose another input and try again."; return
        }
        if elapsed >= 120 { cancelRecording(); error = "Two-minute recording limit reached. Start a new question." }
    }
    private func stopRecording() {
        guard state == "listening", let url = audioURL else { return }
        meter?.invalidate(); meter = nil; recorder?.stop(); recorder = nil; level = 0
        guard elapsed > 0.3, peak > -55 else {
            cancelRecording(); error = "No speech detected. Speak near the Mac microphone and try again."; return
        }
        let gen = generation
        state = "transcribing"; message = "Transcribing with OpenAI…"; feedback("stopping")
        processing = Task { [weak self] in
            guard let self = self else { return }
            defer {
                try? FileManager.default.removeItem(at: url)
                if self.generation == gen { self.audioURL = nil; self.state = "ready"; self.feedback("ready") }
            }
            do {
                let loaded = await Task.detached { Keychain.load() }.value
                guard self.generation == gen, !Task.isCancelled else { return }
                guard let key = loaded else { throw AppError("Allow the app to use your saved OpenAI key, or add a key in Settings.") }
                let text = try await Transcription.transcribe(Data(contentsOf: url), key: key)
                guard self.generation == gen, !Task.isCancelled else { return }
                self.draft = text
                self.state = "sending"
                self.message = "Transcribed. Sending your question to Muse…"
                try await self.send(text)
            } catch {
                guard self.generation == gen, !Task.isCancelled else { return }
                self.error = error.localizedDescription
            }
        }
    }
    private func removeRecording() {
        recorder?.stop(); recorder = nil; meter?.invalidate(); meter = nil
        if let url = audioURL { try? FileManager.default.removeItem(at: url) }
        audioURL = nil; level = 0
    }
    func cancelRecording() {
        if state == "sending" { return }
        generation += 1; processing?.cancel(); processing = nil; removeRecording(); elapsed = 0
        if state != "ready" { message = "Recording cancelled." }
        state = "ready"; feedback("ready")
    }
    func sendSuggestion(_ text: String) async {
        guard suggestions.contains(text), state == "ready", !museBusy else { return }
        draft = text
        await sendDraft()
    }
    func sendDraft() async {
        guard state == "ready", !museBusy else { return }
        state = "sending"
        defer { state = "ready" }
        do { try await send(draft) }
        catch { self.error = error.localizedDescription }
    }
    private func send(_ text: String) async throws {
        let value = text.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !value.isEmpty, value.utf8.count < 1024 else {
            throw AppError("Question must fit 1,023 UTF-8 bytes. Your transcript is kept above—shorten it and press Send.")
        }
        guard connected else { throw AppError("Wait for the Chromatic to reconnect before sending.") }
        guard !museBusy else { throw AppError("Muse is still working. Wait for its reply.") }
        error = ""
        _ = try await Bridge.request("/api/ask", body: ["text": value])
        museBusy = true; message = "Question accepted by the handheld. Waiting for Muse…"
    }
}
