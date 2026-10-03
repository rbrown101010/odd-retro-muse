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

/// Capture the chosen microphone independently of the system playback route.
/// Virtual output devices must not prevent a physical microphone recording.
final class MicrophoneRecorder: NSObject, AVCaptureAudioDataOutputSampleBufferDelegate {
    private let session = AVCaptureSession()
    private let captureQueue = DispatchQueue(label: "OddRetroMuse.microphone")
    private let lock = NSLock()
    private var file: AVAudioFile?
    private var frames: AVAudioFramePosition = 0
    private var sampleRate: Double = 1
    private var average: Float = -160
    private var peak: Float = -160
    private var failure: String?
    private var runtimeObserver: NSObjectProtocol?

    func start(url: URL, device: AudioDeviceID) throws {
        let selected: AVCaptureDevice?
        if device == 0 { selected = AVCaptureDevice.default(for: .audio) }
        else {
            var address = AudioObjectPropertyAddress(mSelector: kAudioDevicePropertyDeviceUID,
                mScope: kAudioObjectPropertyScopeGlobal, mElement: kAudioObjectPropertyElementMain)
            var uid: Unmanaged<CFString>?
            var bytes = UInt32(MemoryLayout<Unmanaged<CFString>?>.size)
            guard AudioObjectGetPropertyData(device, &address, 0, nil, &bytes, &uid) == noErr,
                  let uid = uid?.takeRetainedValue() else {
                throw AppError("This microphone disconnected. Select an available input.")
            }
            selected = AVCaptureDevice(uniqueID: uid as String)
        }
        guard let selected = selected else { throw AppError("The selected microphone is unavailable. Select another input.") }
        guard let physical = CMAudioFormatDescriptionGetStreamBasicDescription(selected.activeFormat.formatDescription),
              physical.pointee.mSampleRate > 0 else {
            throw AppError("This microphone has no active audio format. Reconnect it or select another input.")
        }
        sampleRate = physical.pointee.mSampleRate
        let input = try AVCaptureDeviceInput(device: selected)
        let output = AVCaptureAudioDataOutput()
        output.audioSettings = [AVFormatIDKey: kAudioFormatLinearPCM,
            AVSampleRateKey: sampleRate, AVNumberOfChannelsKey: 1,
            AVLinearPCMBitDepthKey: 32, AVLinearPCMIsFloatKey: true,
            AVLinearPCMIsBigEndianKey: false, AVLinearPCMIsNonInterleaved: true]
        output.setSampleBufferDelegate(self, queue: captureQueue)
        session.beginConfiguration()
        guard session.canAddInput(input), session.canAddOutput(output) else {
            session.commitConfiguration()
            throw AppError("Could not connect the selected microphone to the recorder.")
        }
        session.addInput(input); session.addOutput(output)
        session.commitConfiguration()
        file = try AVAudioFile(forWriting: url, settings: [
            AVFormatIDKey: kAudioFormatLinearPCM, AVSampleRateKey: sampleRate,
            AVNumberOfChannelsKey: 1, AVLinearPCMBitDepthKey: 16,
            AVLinearPCMIsFloatKey: false, AVLinearPCMIsBigEndianKey: false,
            AVLinearPCMIsNonInterleaved: false], commonFormat: .pcmFormatFloat32, interleaved: false)
        runtimeObserver = NotificationCenter.default.addObserver(forName: .AVCaptureSessionRuntimeError,
            object: session, queue: nil) { [weak self] notification in
            guard let self = self else { return }
            let code = (notification.userInfo?[AVCaptureSessionErrorKey] as? NSError)?.code ?? 0
            self.lock.lock(); self.failure = "Microphone capture stopped (audio error \(code)). Reconnect it and try again."; self.lock.unlock()
        }
        session.startRunning()
        guard session.isRunning else { stop(); throw AppError("Could not start microphone capture. Choose another input and try again.") }
    }
    func captureOutput(_ output: AVCaptureOutput, didOutput sampleBuffer: CMSampleBuffer, from connection: AVCaptureConnection) {
        lock.lock(); defer { lock.unlock() }
        guard let file = file, failure == nil, CMSampleBufferDataIsReady(sampleBuffer),
              let description = CMSampleBufferGetFormatDescription(sampleBuffer) else { return }
        let format = AVAudioFormat(cmAudioFormatDescription: description)
        let count = CMSampleBufferGetNumSamples(sampleBuffer)
        guard count > 0, count <= Int(Int32.max), format.commonFormat == .pcmFormatFloat32,
              format.channelCount == 1, format.sampleRate == sampleRate,
              let buffer = AVAudioPCMBuffer(pcmFormat: format, frameCapacity: AVAudioFrameCount(count)) else {
            failure = "The microphone returned an unsupported recording format. Select another input."; return
        }
        buffer.frameLength = AVAudioFrameCount(count)
        let code = CMSampleBufferCopyPCMDataIntoAudioBufferList(sampleBuffer, at: 0,
            frameCount: Int32(count), into: buffer.mutableAudioBufferList)
        guard code == noErr else { failure = "Could not read microphone audio (error \(code))."; return }
        do { try file.write(from: buffer) }
        catch { failure = "Microphone recording failed: \(error.localizedDescription)"; return }
        frames += AVAudioFramePosition(count)
        if let samples = buffer.floatChannelData {
            var sum: Float = 0, maximum: Float = 0
            for i in 0..<count {
                let value = abs(samples[0][i]); maximum = max(maximum, value); sum += value * value
            }
            average = 20 * log10(max(0.00000001, sqrt(sum / Float(count))))
            peak = max(peak, 20 * log10(max(0.00000001, maximum)))
        }
    }
    func metrics() -> (elapsed: Double, level: Float, peak: Float, failure: String?) {
        lock.lock(); defer { lock.unlock() }
        return (Double(frames) / sampleRate, max(0, min(1, pow(10, average / 30))), peak, failure)
    }
    func stop() {
        session.stopRunning()
        captureQueue.sync {} // Finish pending writes before the WAV is read/deleted.
        lock.lock(); file = nil; lock.unlock()
        if let observer = runtimeObserver { NotificationCenter.default.removeObserver(observer); runtimeObserver = nil }
    }
    deinit { stop() }
}
