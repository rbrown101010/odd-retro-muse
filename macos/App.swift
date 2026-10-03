import SwiftUI
import AppKit

@main struct OddRetroMuseApp: App {
    @StateObject private var controller = Controller()
    @NSApplicationDelegateAdaptor(AppDelegate.self) var delegate
    var body: some Scene {
        WindowGroup("Odd Retro Muse", id: "main") {
            CompanionView(controller: controller)
                .frame(minWidth: 600, idealWidth: 750, minHeight: 590)
                .preferredColorScheme(.dark)
        }
        .defaultSize(width: 760, height: 650)
        .commands {
            CommandGroup(replacing: .appSettings) {
                Button("OpenAI Settings…") { controller.settingsVisible = true }.keyboardShortcut(",")
            }
        }
        MenuBarExtra("Odd Retro Muse", systemImage: "gamecontroller.fill") {
            MenuView(controller: controller)
        }
    }
}
final class AppDelegate: NSObject, NSApplicationDelegate {
    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { false }
}
struct MenuView: View {
    @ObservedObject var controller: Controller
    @Environment(\.openWindow) var openWindow
    var body: some View {
        Text(controller.connected ? "Chromatic connected" : "Chromatic reconnecting")
        Text(controller.armed ? "A+B microphone ready" : "Microphone disabled")
        Divider()
        Button("Open Odd Retro Muse") { openWindow(id: "main"); NSApp.activate(ignoringOtherApps: true) }
        Button(controller.armed ? "Disable microphone" : "Enable microphone") {
            Task { if controller.armed { await controller.disableMicrophone() } else { await controller.enableMicrophone() } }
        }
        Divider()
        Button("Quit Odd Retro Muse") { NSApp.terminate(nil) }
    }
}
struct CompanionView: View {
    @ObservedObject var controller: Controller
    private let mint = Color(red: 0.43, green: 0.97, blue: 0.67)
    var body: some View {
        VStack(alignment: .leading, spacing: 20) {
            HStack(alignment: .center) {
                HStack(spacing: 12) {
                    Image(systemName: "gamecontroller.fill").font(.system(size: 26)).foregroundStyle(mint)
                    VStack(alignment: .leading, spacing: 3) {
                        Text("Odd Retro Muse").font(.system(size: 25, weight: .semibold, design: .rounded))
                        Text("Your pocket companion. Your Mac's voice.").font(.subheadline).foregroundStyle(.secondary)
                    }
                }
                Spacer()
                Text(controller.connected ? (controller.wireless ? "● Wi-Fi connected" : "● USB connected") : "○ Reconnecting")
                    .font(.caption.weight(.medium)).foregroundStyle(controller.connected ? mint : .secondary)
                    .padding(.horizontal, 10).padding(.vertical, 7).background(.white.opacity(0.05), in: Capsule())
                Button { controller.settingsVisible = true } label: { Image(systemName: "gearshape") }
                    .buttonStyle(.borderless).help("OpenAI API key and transcription settings")
            }
            VStack(alignment: .leading, spacing: 10) {
                Text("ASK YOUR MUSE").font(.caption.weight(.semibold)).tracking(1.5).foregroundStyle(.secondary)
                TextEditor(text: $controller.draft).font(.system(size: 16)).scrollContentBackground(.hidden)
                    .padding(8).frame(minHeight: 90, maxHeight: 125)
                    .background(.black.opacity(0.17), in: RoundedRectangle(cornerRadius: 10))
                    .overlay(RoundedRectangle(cornerRadius: 10).stroke(.white.opacity(0.1)))
                    .accessibilityLabel("Message to Muse")
                HStack {
                    Button {
                        Task { await controller.sendDraft() }
                    } label: { Label("Send to Muse", systemImage: "arrow.up") }
                        .buttonStyle(.borderedProminent).tint(mint).foregroundStyle(.black)
                        .disabled(!controller.connected || controller.draft.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty || controller.state != "ready" || controller.museBusy)
                    Button {
                        if controller.armed { controller.toggleRecording() }
                        else { Task { await controller.enableMicrophone() } }
                    } label: {
                        Label(controller.state == "listening" ? "Stop & send" : controller.state == "transcribing" ? "Transcribing…" : controller.armed ? "Start recording" : "Enable microphone",
                              systemImage: controller.state == "listening" ? "stop.fill" : "mic.fill")
                    }.buttonStyle(.bordered).disabled((controller.state == "transcribing" || controller.state == "sending") || controller.museBusy)
                    if controller.state != "ready" && controller.state != "sending" {
                        Button("Cancel") { controller.cancelRecording() }.buttonStyle(.bordered)
                    }
                    Spacer()
                    if controller.armed && controller.state == "ready" {
                        Button("Disable mic") { Task { await controller.disableMicrophone() } }.buttonStyle(.borderless).font(.caption)
                    }
                }
                if controller.state == "listening" {
                    HStack {
                        ProgressView(value: Double(controller.level)).tint(mint)
                        Text(String(format: "%02d:%02d", Int(controller.elapsed)/60, Int(controller.elapsed)%60))
                            .font(.system(.caption, design: .monospaced)).frame(width: 48)
                    }
                }
                HStack {
                    Text("Microphone").font(.caption).foregroundStyle(.secondary)
                    Picker("Microphone", selection: Binding(get: { controller.microphoneID }, set: { controller.selectMicrophone($0) })) {
                        Text("System default").tag(UInt32(0))
                        ForEach(controller.microphones) { device in Text(device.name).tag(device.id) }
                    }.labelsHidden().disabled(controller.state != "ready")
                }
                Text(controller.message).font(.subheadline).foregroundStyle(.secondary).textSelection(.enabled)
                if !controller.error.isEmpty {
                    Text(controller.error).font(.subheadline).foregroundStyle(.orange).textSelection(.enabled)
                }
            }.padding(20).background(.white.opacity(0.035), in: RoundedRectangle(cornerRadius: 16))
            VStack(alignment: .leading, spacing: 10) {
                HStack {
                    Text("MUSE REPLY").font(.caption.weight(.semibold)).tracking(1.5).foregroundStyle(mint)
                    Spacer()
                    if controller.museBusy { ProgressView().controlSize(.small) }
                }
                ScrollView {
                    Text(controller.reply.isEmpty ? "Your Muse's reply will appear here and on the Chromatic." : controller.reply)
                        .font(.system(size: 17)).lineSpacing(5).frame(maxWidth: .infinity, alignment: .leading)
                        .foregroundStyle(controller.reply.isEmpty ? .secondary : .primary).textSelection(.enabled)
                }.frame(minHeight: 110, maxHeight: .infinity)
            }.padding(20).background(.white.opacity(0.035), in: RoundedRectangle(cornerRadius: 16))
            HStack(alignment: .top, spacing: 14) {
                Image(systemName: "waveform").foregroundStyle(mint)
                VStack(alignment: .leading, spacing: 4) {
                    Text("A+B to talk. A+B again to send. B to cancel.").font(.subheadline.weight(.medium))
                    Text("Speak near your Mac. Audio is transcribed by OpenAI, then deleted locally. Your phone can stay closed. The app keeps working from the menu bar when this window is closed.")
                        .font(.caption).foregroundStyle(.secondary)
                }
            }
        }.padding(24).background(Color(red: 0.035, green: 0.075, blue: 0.085))
        .sheet(isPresented: $controller.settingsVisible) { SettingsView(controller: controller) }
    }
}
struct SettingsView: View {
    @ObservedObject var controller: Controller
    @State private var key = ""
    var body: some View {
        VStack(alignment: .leading, spacing: 18) {
            Text("OpenAI transcription").font(.title2.weight(.semibold))
            Text("Model: \(Transcription.model)").font(.system(.body, design: .monospaced))
            Text("Enter your own OpenAI API key. It stays in macOS Keychain and is never placed in source code, the handheld, or GitHub.")
                .foregroundStyle(.secondary)
            SecureField("OpenAI API key", text: $key).textFieldStyle(.roundedBorder)
            Link("Get an OpenAI API key ↗", destination: URL(string: "https://platform.openai.com/api-keys")!)
            Text("OpenAI API usage is billed to the key's account. Recordings are sent only after you stop and send; cancellation deletes the recording.").font(.caption).foregroundStyle(.secondary)
            if !controller.error.isEmpty { Text(controller.error).font(.caption).foregroundStyle(.orange) }
            HStack {
                Text(controller.keyConfigured ? "API key saved" : "No API key saved").font(.caption).foregroundStyle(.secondary)
                Spacer()
                Button("Cancel") { controller.settingsVisible = false }
                Button("Save key") { controller.saveKey(key); key = "" }.buttonStyle(.borderedProminent).disabled(key.isEmpty)
            }
        }.padding(28).frame(width: 480)
    }
}
