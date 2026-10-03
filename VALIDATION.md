# Validation

Checked on an Apple Silicon Mac and one ESP32-U4WDH Chromatic on October 3, 2026.

- Custom MCU firmware built successfully; application-only flash completed with a verified hash.
- Restart captured a healthy boot with zero panics, followed by Wi-Fi and encrypted Muse reconnection.
- Installed native Mac app visibly reported Wi-Fi connected and sent a typed question to Muse. Muse delivered a 951-byte answer back to the app and handheld bridge.
- Compact handheld renderer removed the branded bar, retained the character, and supports 25 columns × 11 rows per page. Address/undefined-behavior sanitizers passed for rendering and paging up to the 2,400-byte limit.
- Muse SDK host suite: 143 tests run, one skipped, passing.
- Local bridge/wireless tests: 20 passing.
- Native Swift transcription tests passed for binary multipart preservation, successful returned text, API authentication/rate/server failures, empty audio/text, and cancelled uploads.
- Native app signature verified after installation. The installed bridge and native app launcher are registered as macOS login agents. A full Mac logout/reboot was not performed during this update.

A live OpenAI recording → transcription → Muse round trip is now verified. The original AAC recorder failed to start on the active Bluetooth input. The replacement AVAudioEngine path used the built-in Mac microphone, captured WAV audio, returned a 204-byte transcript, and received a 193-byte Muse reply. No audio or private conversation content is included here.

## Subsequent microphone update

The earlier AVAudioEngine path later reported a zero-channel output format for the already-selected Mac microphone. Using its valid hardware input format passed that check but the engine still failed to initialize with error -10875. The current recorder uses a microphone-only AVCaptureSession, selects the exact Core Audio device UID, and requests mono floating-point PCM at the microphone's native sample rate before writing 16-bit WAV. It has no playback connection. Existing microphone authorization is used directly; first-time access still requires the macOS prompt.

The updated native app builds and its installed signature verifies. A synthetic 48 kHz PCM sample-buffer → WAV → decoded-sample round trip passed, as did native transcription tests and all 20 bridge/wireless tests. The update is installed on the test Mac; live capture with this version is pending macOS microphone permission. The earlier successful live voice test above applies to the previous recorder, not this new capture backend.

## Reproduce focused tests

```sh
.venv/bin/python -m unittest discover -s tests -p 'test_*.py'
xcrun swiftc -swift-version 5 -parse-as-library \
  macos/tests_Transcription.swift macos/Transcription.swift macos/Keychain.swift \
  -o work/transcription-tests
work/transcription-tests
cc -fsanitize=address,undefined -g -Isdk/esp32/main \
  tests/render_boundaries.c sdk/esp32/main/chromatic_ui.c sdk/esp32/main/pixel_font.c \
  -o work/render-tests
work/render-tests
```
