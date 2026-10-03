#!/usr/bin/env python3
"""Build the custom firmware with credentials outside the deliverable folder."""
import argparse
import os
import re
import subprocess
from pathlib import Path

def main():
    project = Path(__file__).resolve().parent
    local_workspace = project.parents[1]
    workspace = local_workspace if (local_workspace / 'work/toolchain/esp-idf').exists() else project
    parser = argparse.ArgumentParser()
    parser.add_argument("--token-file", type=Path, default=workspace / "work/muse-sdk-token.txt")
    parser.add_argument("--idf", type=Path, default=Path(os.environ.get('IDF_PATH', workspace / "work/toolchain/esp-idf")))
    parser.add_argument("--build-dir", type=Path, default=workspace / "work/chromatic-build")
    args = parser.parse_args()
    token = args.token_file.read_text().strip()
    if not re.fullmatch(r"mgst_[A-Za-z0-9_-]+", token):
        raise SystemExit("Expected a Muse SDK token file; its contents are never printed.")
    build = args.build_dir.resolve()
    build.mkdir(parents=True, exist_ok=True)
    build.chmod(0o700)
    config = build / "sdkconfig"
    old = config.read_text() if config.exists() else ""
    old = "\n".join(l for l in old.splitlines() if not l.startswith("CONFIG_GADGET_SDK_TOKEN="))
    config.write_text(old + '\nCONFIG_GADGET_SDK_TOKEN="' + token + '"\n')
    config.chmod(0o600)
    env = dict(os.environ)
    bundled_tools = workspace / "work/toolchain/idf-tools"
    if bundled_tools.exists() and not env.get('IDF_TOOLS_PATH'):
        env["IDF_TOOLS_PATH"] = str(bundled_tools)
    bundled_python = workspace / "work/toolchain/python/bin"
    if bundled_python.exists():
        env["PATH"] = str(bundled_python) + os.pathsep + env.get("PATH", "")
    if not (args.idf / 'export.sh').exists():
        raise SystemExit('Install ESP-IDF v6.0.1 and supply --idf or IDF_PATH.')
    command = ["bash", "-c", '. "$1/export.sh" && shift && exec "$@"', "build",
               str(args.idf.resolve()), "idf.py", "-B", str(build), "-DIDF_TARGET=esp32",
               "-DSDKCONFIG=" + str(config),
               "-DSDKCONFIG_DEFAULTS=sdkconfig.defaults;devices/sdkconfig.ideaspark;devices/sdkconfig.chromatic", "build"]
    subprocess.run(command, cwd=project / "sdk/esp32", env=env, check=True)

if __name__ == "__main__":
    main()
