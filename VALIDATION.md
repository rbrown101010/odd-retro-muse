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

A live OpenAI recording → transcription → Muse test still requires the user's valid OpenAI API key in the native app's Settings. The API key available during development was rejected with HTTP 401. Mock tests verify request/error/cancellation handling, not live transcription quality. The earlier browser microphone workflow was tested on hardware; this update replaces that workflow in the installed native app.

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
