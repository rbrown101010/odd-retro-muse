#!/usr/bin/env python3
"""Scan reachable Git history without printing matched credential values."""
import hashlib
import re
import subprocess
import sys

PATTERNS = {
    "Muse SDK token": rb"mgst_[A-Za-z0-9_-]{20,}",
    "OpenAI key": rb"\bsk-(?:proj-|svcacct-)?[A-Za-z0-9_-]{20,}",
    "GitHub token": rb"\b(?:gh[pousr]_[A-Za-z0-9]{30,}|github_pat_[A-Za-z0-9_]{40,})",
    "AWS access key": rb"\b(?:AKIA|ASIA)[A-Z0-9]{16}\b",
    "Google API key": rb"AIza[A-Za-z0-9_-]{30,}",
    "private key": rb"-----BEGIN (?:RSA |EC |OPENSSH |DSA |ENCRYPTED )?PRIVATE KEY-----",
    "credential URL": rb"://[^\s/:]+:[^\s/@]{8,}@",
}
DEVELOPMENT_KEY = "e57311281ea0478a80be57036768ef71481804eab73182b2b70255311a2ded6a"
PRIVATE_PATH = re.compile(
    r"(^|/)(?:\.env(?:\..*)?|wireless\.json|sdkconfig(?:\.old)?|muse-sdk-token\.txt)$"
    r"|(^|/)(?:work|dist|build|managed_components|\.venv|\.secrets|__pycache__)(/|$)"
    r"|\.(?:wav|m4a|mp3|bin|elf|pem|key|p12|pfx|keystore|mobileprovision|log|pyc)$"
    r"|\.app(/|$)"
)


def git(*args):
    return subprocess.check_output(["git", *args])


def allowed(path, rule, match, data):
    if rule == "private key" and path == "sdk/esp32/dev_signing_key.pem":
        return hashlib.sha256(data).hexdigest() == DEVELOPMENT_KEY
    return (rule == "Muse SDK token"
            and path == "sdk/esp32/tests/link_pairing_handshake_harness.c"
            and match == b"mgst_" + b"A" * 43)


def main():
    rows = git("rev-list", "--objects", "--all").decode().splitlines()
    findings = set()
    count = 0
    # One blob may occur under several names. Inspect every historical tree so
    # a private artifact cannot hide by sharing content with an allowed file.
    for commit in git("rev-list", "--all").decode().splitlines():
        paths = git("ls-tree", "-r", "-z", "--name-only", commit).decode(errors="replace").split("\0")
        for path in paths:
            if path != "sdk/esp32/dev_signing_key.pem" and PRIVATE_PATH.search(path):
                findings.add((path, "private artifact"))
    # Batch reads avoid spawning a Git process for each revision of each file.
    process = subprocess.Popen(["git", "cat-file", "--batch"],
                               stdin=subprocess.PIPE, stdout=subprocess.PIPE)
    try:
        for row in rows:
            oid, _, path = row.partition(" ")
            process.stdin.write((oid + "\n").encode())
            process.stdin.flush()
            header = process.stdout.readline().split()
            data = process.stdout.read(int(header[2]))
            process.stdout.read(1)
            if header[1] not in (b"blob", b"commit"):
                continue
            count += 1
            label = path or "commit " + oid[:12]
            for rule, pattern in PATTERNS.items():
                for match in re.finditer(pattern, data):
                    if not allowed(path, rule, match.group(), data):
                        findings.add((label, rule))
    finally:
        process.stdin.close()
        process.stdout.close()
        process.wait()
    for path, rule in sorted(findings):
        print(f"BLOCKED: {path}: {rule}")
    print(f"Scanned {count} Git blobs/commits; {len(findings)} finding(s).")
    return 1 if findings else 0


if __name__ == "__main__":
    sys.exit(main())
