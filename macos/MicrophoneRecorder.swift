import AVFoundation
import CoreAudio
import AudioToolbox
import Foundation

struct MicrophoneDevice: Identifiable, Hashable {
    let id: AudioDeviceID
    let name: String
    let builtIn: Bool
    static func available() -> [MicrophoneDevice] {
        var address = AudioObjectPropertyAddress(mSelector: kAudioHardwarePropertyDevices,
            mScope: kAudioObjectPropertyScopeGlobal, mElement: kAudioObjectPropertyElementMain)
        var size: UInt32 = 0
        guard AudioObjectGetPropertyDataSize(AudioObjectID(kAudioObjectSystemObject), &address, 0, nil, &size) == noErr else { return [] }
        var ids = [AudioDeviceID](repeating: 0, count: Int(size) / MemoryLayout<AudioDeviceID>.size)
        guard AudioObjectGetPropertyData(AudioObjectID(kAudioObjectSystemObject), &address, 0, nil, &size, &ids) == noErr else { return [] }
        return ids.compactMap { id in
            var input = AudioObjectPropertyAddress(mSelector: kAudioDevicePropertyStreams,
                mScope: kAudioDevicePropertyScopeInput, mElement: kAudioObjectPropertyElementMain)
            var bytes: UInt32 = 0
            guard AudioObjectGetPropertyDataSize(id, &input, 0, nil, &bytes) == noErr, bytes > 0 else { return nil }
            var nameAddress = AudioObjectPropertyAddress(mSelector: kAudioObjectPropertyName,
                mScope: kAudioObjectPropertyScopeGlobal, mElement: kAudioObjectPropertyElementMain)
            var name: Unmanaged<CFString>?
            bytes = UInt32(MemoryLayout<Unmanaged<CFString>?>.size)
            guard AudioObjectGetPropertyData(id, &nameAddress, 0, nil, &bytes, &name) == noErr,
                  let name = name?.takeRetainedValue() else { return nil }
            var transportAddress = AudioObjectPropertyAddress(mSelector: kAudioDevicePropertyTransportType,
                mScope: kAudioObjectPropertyScopeGlobal, mElement: kAudioObjectPropertyElementMain)
            var transport: UInt32 = 0; bytes = UInt32(MemoryLayout<UInt32>.size)
            _ = AudioObjectGetPropertyData(id, &transportAddress, 0, nil, &bytes, &transport)
            return MicrophoneDevice(id: id, name: name as String, builtIn: transport == kAudioDeviceTransportTypeBuiltIn)
        }.sorted { $0.name.localizedStandardCompare($1.name) == .orderedAscending }
    }
}

/// Capture the selected physical input at its actual format. WAV avoids AAC
/// encoder preparation failures when Bluetooth changes the system input format.
final class MicrophoneRecorder {
    private let engine = AVAudioEngine()
    private let lock = NSLock()
    private var file: AVAudioFile?
    private var frames: AVAudioFramePosition = 0
    private var sampleRate: Double = 1
    private var average: Float = -160
    private var peak: Float = -160
    private var failure: String?
    private var tapped = false

    func start(url: URL, device: AudioDeviceID) throws {
        let input = engine.inputNode
        if device != 0, let unit = input.audioUnit {
            var id = device
            let code = AudioUnitSetProperty(unit, kAudioOutputUnitProperty_CurrentDevice,
                kAudioUnitScope_Global, 0, &id, UInt32(MemoryLayout<AudioDeviceID>.size))
            guard code == noErr else { throw AppError("Could not open the selected microphone (audio error \(code)). Choose another input.") }
        }
        let format = input.outputFormat(forBus: 0)
        guard format.sampleRate > 0, format.channelCount > 0, format.channelCount <= 2 else {
            throw AppError("This microphone has no usable audio input. Select your Mac microphone.")
        }
        sampleRate = format.sampleRate
        file = try AVAudioFile(forWriting: url, settings: [
            AVFormatIDKey: kAudioFormatLinearPCM, AVSampleRateKey: format.sampleRate,
            AVNumberOfChannelsKey: format.channelCount, AVLinearPCMBitDepthKey: 16,
            AVLinearPCMIsFloatKey: false, AVLinearPCMIsBigEndianKey: false,
            AVLinearPCMIsNonInterleaved: false], commonFormat: .pcmFormatFloat32, interleaved: false)
        input.installTap(onBus: 0, bufferSize: 1024, format: format) { [weak self] buffer, _ in
            self?.consume(buffer)
        }
        tapped = true
        do { engine.prepare(); try engine.start() }
        catch { stop(); throw AppError("Could not start the selected microphone: \(error.localizedDescription)") }
    }
    private func consume(_ buffer: AVAudioPCMBuffer) {
        lock.lock(); defer { lock.unlock() }
        guard let file = file, failure == nil else { return }
        do { try file.write(from: buffer) }
        catch { failure = "Microphone recording failed: \(error.localizedDescription)"; return }
        frames += AVAudioFramePosition(buffer.frameLength)
        if let samples = buffer.floatChannelData, buffer.frameLength > 0 {
            var sum: Float = 0, maximum: Float = 0
            for i in 0..<Int(buffer.frameLength) {
                let value = abs(samples[0][i]); maximum = max(maximum, value); sum += value * value
            }
            average = 20 * log10(max(0.00000001, sqrt(sum / Float(buffer.frameLength))))
            peak = max(peak, 20 * log10(max(0.00000001, maximum)))
        }
    }
    func metrics() -> (elapsed: Double, level: Float, peak: Float, failure: String?) {
        lock.lock(); defer { lock.unlock() }
        return (Double(frames) / sampleRate, max(0, min(1, pow(10, average / 30))), peak, failure)
    }
    func stop() {
        engine.stop()
        if tapped { engine.inputNode.removeTap(onBus: 0); tapped = false }
        lock.lock(); file = nil; lock.unlock()
    }
    deinit { stop() }
}
