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

## Twenty-follow-up update

The follow-up feature is built but has not been installed on the physical handheld: its USB update connection was not attached during development. The currently installed Mac app retains the verified microphone fix.

- Firmware and native Mac app builds pass with the new `display.reply` command and 20-option UI.
- Sanitizer tests pass for full-frame/striped rendering of the options list, including first/last selection and maximum item lengths.
- Option tests pass for exact counts, duplicate rejection, ASCII/length constraints, round-trip serialization, and encrypted-packet size bounds.
- The updated firmware needs an application-only USB flash, a healthy boot capture, and a real Muse-generated reply with 20 options before hardware completion can be claimed.

Do not share compiled handheld firmware: it embeds the owner's Muse SDK token.
