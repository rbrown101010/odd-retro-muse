# Security and privacy

This is an experimental development project. The source audit is not a penetration test or a guarantee of production security.

## Credentials and private artifacts

- OpenAI credentials stay in macOS Keychain. The app sends them only to OpenAI's HTTPS transcription endpoint; the browser bridge and handheld never receive them.
- Muse SDK tokens belong in an ignored, private token file. Firmware binaries and generated sdkconfig embed this token and must never be published, attached to issues, or uploaded as releases.
- Keep wireless.json, flash backups, recordings, service logs, and build outputs private. The installer creates a user-only support directory and restricts the bridge key file to its owner.
- Audio goes to OpenAI only after stop-and-send. The app deletes local temporary recordings after completion or cancellation and clears abandoned recordings at startup. Transcripts and replies pass through the companion's in-memory event buffer and the user's Muse.

## Development signing key

`sdk/esp32/dev_signing_key.pem` is the intentionally public **shared development key** from Meta's Muse Gadgets SDK. It is not an owner credential. Its SHA-256 is `e57311281ea0478a80be57036768ef71481804eab73182b2b70255311a2ded6a`. Treat it as compromised by design: anybody can sign with it. Never use it for release signing or to establish a production trust boundary. The bundled SDK also contains a deliberately fake all-A token in its pairing test harness.

The Chromatic profile disables OTA, hardware secure boot, flash encryption, NVS encryption, and manufacturing eFuse attestation. USB flashing is a trusted physical operation. Pairing and wireless credentials in ordinary NVS can be recovered by someone with physical access. Do not enable irreversible eFuse protection or replace the board's manufacturing identity as a routine update.

## Network and local trust boundary

The HTTP bridge binds to `127.0.0.1` only. It checks the Host header, requires a matching loopback Origin and JSON content type for writes, and does not enable cross-origin reads. Replies are rendered as text. Do not expose port 8765 through forwarding, a public tunnel, or a reverse proxy.

Loopback is a trusted-computer boundary, not authentication against other programs or users on the same Mac. Local software can read the bridge's current conversation buffer or submit requests. Use this app on a trusted personal Mac.

Wi-Fi bridge packets use AES-256-GCM with a USB-provisioned random key, fresh session challenges, and ordered commands. Keep that key secret and provision a new one if exposed. The development firmware does not claim resistance to physical extraction, traffic disruption, or denial of service.

## Before publishing changes

Run `python3 scripts/check_secrets.py`. The dependency-free guard scans every reachable Git revision, including other branches and deleted files, for project tokens, common provider credentials, private keys, and forbidden private artifacts. Only the exact public development-key hash and the exact fake pairing token are allowlisted. CI runs this check on pushes and pull requests with read-only permissions. Pattern matching has limits; manually review changes and GitHub secret alerts too.

If a real credential is exposed, revoke or rotate it immediately. Removing a file does not remove it from Git history. Avoid posting keys, recordings, or private logs in public issues. Use GitHub's private vulnerability reporting for security reports when available.
