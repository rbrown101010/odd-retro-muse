#!/usr/bin/env python3
"""Install/update or remove the user's automatic Chromatic Muse companion."""
import argparse
import json
import os
from pathlib import Path
import plistlib
import shutil
import subprocess
import sys
import urllib.request

SOURCE = Path(__file__).resolve().parent
SUPPORT = Path.home() / 'Library/Application Support/Chromatic Muse'
AGENTS = Path.home() / 'Library/LaunchAgents'
APP = Path.home() / 'Applications/Chromatic Muse.app'
NATIVE_APP = Path.home() / 'Applications/Odd Retro Muse.app'
LABEL = 'com.riley.chromatic-muse'
LOGIN_LABEL = LABEL + '.open-at-login'
DOMAIN = f'gui/{os.getuid()}'

def launch(*args, check=False):
    return subprocess.run(['/bin/launchctl', *args], check=check, capture_output=True, text=True)

def write_plist(path, data):
    path.write_bytes(plistlib.dumps(data))
    os.chmod(path, 0o644)
    subprocess.run(['/usr/bin/plutil', '-lint', str(path)], check=True)

def clear_finder_metadata(app):
    # Finder can attach directory metadata after a build in Documents.
    # These unsigned attributes are unrelated to quarantine or permissions.
    for attr in ('com.apple.FinderInfo', 'com.apple.ResourceFork'):
        subprocess.run(['/usr/bin/xattr', '-dr', attr, str(app)],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

def install(wireless_config=None,native_app=None):
    if SUPPORT.exists() and not (SUPPORT / 'installed.json').exists():
        raise SystemExit('Installation folder already exists without an ownership manifest; refusing to overwrite it.')
    if SUPPORT.exists() and json.loads((SUPPORT / 'installed.json').read_text()).get('managed_by') != LABEL:
        raise SystemExit('Ownership manifest mismatch; refusing update.')
    if APP.exists():
        info = plistlib.loads((APP / 'Contents/Info.plist').read_bytes())
        if info.get('CFBundleIdentifier') != LABEL + '.launcher':
            raise SystemExit('An unrelated app already uses this name; refusing overwrite.')
    if NATIVE_APP.exists():
        info = plistlib.loads((NATIVE_APP / 'Contents/Info.plist').read_bytes())
        if info.get('CFBundleIdentifier') != 'com.riley.odd-retro-muse':
            raise SystemExit('An unrelated app already uses this name; refusing overwrite.')
    SUPPORT.mkdir(parents=True, exist_ok=True, mode=0o700)
    os.chmod(SUPPORT, 0o700)
    # Public runtime only; the local bridge key is copied separately below.
    for name in ('companion.py', 'companion.html', 'voice-controller.mjs', 'preview.png', 'open_companion.py', 'wireless_client.py'):
        shutil.copy2(SOURCE / name, SUPPORT / name)
    (SUPPORT / 'installed.json').write_text(json.dumps({'managed_by': LABEL, 'source': str(SOURCE)}, indent=2))
    runtime = SUPPORT / 'runtime'
    python = runtime / 'bin/python'
    if not python.exists():
        subprocess.run([sys._base_executable, '-m', 'venv', str(runtime)], check=True)
    subprocess.run([str(python), '-m', 'pip', 'install', '--disable-pip-version-check', 'pyserial==3.5', 'cryptography==50.0.2'], check=True)
    if wireless_config:
        data = json.loads(wireless_config.read_text())
        if len(bytes.fromhex(data['key'])) != 32:
            raise SystemExit('Invalid wireless configuration')
        dest = SUPPORT / 'wireless.json'
        fd = os.open(dest, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        with os.fdopen(fd, 'w') as out: json.dump(data, out)
        os.chmod(dest, 0o600)
    service_args = [str(python), '-u', str(SUPPORT / 'companion.py')]
    if (SUPPORT / 'wireless.json').exists(): service_args.append('--wireless-only')
    if native_app is None:
        native_app = SUPPORT / 'native-build/Odd Retro Muse.app'
        subprocess.run([sys.executable,str(SOURCE / 'build_macos.py'),'--output',str(native_app)],check=True)
    native_info = plistlib.loads((native_app / 'Contents/Info.plist').read_bytes())
    if native_info.get('CFBundleIdentifier') != 'com.riley.odd-retro-muse':
        raise SystemExit('Unexpected native app identity')
    clear_finder_metadata(native_app)
    subprocess.run(['/usr/bin/codesign','--verify','--deep','--strict',str(native_app)],check=True)
    if NATIVE_APP.exists(): shutil.rmtree(NATIVE_APP)
    shutil.copytree(native_app,NATIVE_APP,copy_function=shutil.copy)
    clear_finder_metadata(NATIVE_APP)
    subprocess.run(['/usr/bin/codesign','--verify','--deep','--strict',str(NATIVE_APP)],check=True)
    for name in ('service.log', 'service-error.log', 'login.log', 'login-error.log'):
        path = SUPPORT / name
        path.touch(exist_ok=True)
        os.chmod(path, 0o600)
    AGENTS.mkdir(parents=True, exist_ok=True)
    for label in (LOGIN_LABEL, LABEL):
        launch('bootout', f'{DOMAIN}/{label}')
    common = {'WorkingDirectory': str(SUPPORT), 'LimitLoadToSessionType': 'Aqua', 'RunAtLoad': True}
    service = dict(common, Label=LABEL, ProgramArguments=service_args,
                   KeepAlive=True, ThrottleInterval=5, ProcessType='Background',
                   StandardOutPath=str(SUPPORT / 'service.log'), StandardErrorPath=str(SUPPORT / 'service-error.log'))
    login = dict(common, Label=LOGIN_LABEL, ProgramArguments=[str(python), str(SUPPORT / 'open_companion.py')],
                 StandardOutPath=str(SUPPORT / 'login.log'), StandardErrorPath=str(SUPPORT / 'login-error.log'))
    write_plist(AGENTS / (LABEL + '.plist'), service)
    write_plist(AGENTS / (LOGIN_LABEL + '.plist'), login)
    contents = APP / 'Contents'
    (contents / 'MacOS').mkdir(parents=True, exist_ok=True)
    launcher = contents / 'MacOS/ChromaticMuse'
    # Paths are shell-quoted rather than interpolated as shell code.
    import shlex
    launcher.write_text('#!/bin/sh\nexec ' + shlex.quote(str(python)) + ' ' + shlex.quote(str(SUPPORT / 'open_companion.py')) + '\n')
    os.chmod(launcher, 0o755)
    write_plist(contents / 'Info.plist', {'CFBundleExecutable': 'ChromaticMuse', 'CFBundleIdentifier': LABEL + '.launcher',
                'CFBundleName': 'Chromatic Muse', 'CFBundleDisplayName': 'Chromatic Muse',
                'CFBundlePackageType': 'APPL', 'CFBundleVersion': '1', 'CFBundleShortVersionString': '1.0'})
    launch('enable', f'{DOMAIN}/{LABEL}', check=True)
    launch('enable', f'{DOMAIN}/{LOGIN_LABEL}', check=True)
    launch('bootstrap', DOMAIN, str(AGENTS / (LABEL + '.plist')), check=True)
    launch('bootstrap', DOMAIN, str(AGENTS / (LOGIN_LABEL + '.plist')), check=True)
    print('Installed automatic companion:', SUPPORT)
    print('Native app:',NATIVE_APP)
    print('Compatibility shortcut:',APP)

def uninstall():
    if SUPPORT.exists():
        manifest = json.loads((SUPPORT / 'installed.json').read_text())
        if manifest.get('managed_by') != LABEL:
            raise SystemExit('Ownership manifest mismatch; refusing removal.')
    for label in (LOGIN_LABEL, LABEL):
        launch('bootout', f'{DOMAIN}/{label}')
        (AGENTS / (label + '.plist')).unlink(missing_ok=True)
    if APP.exists():
        info = plistlib.loads((APP / 'Contents/Info.plist').read_bytes())
        if info.get('CFBundleIdentifier') == LABEL + '.launcher':
            shutil.rmtree(APP)
    if NATIVE_APP.exists():
        info=plistlib.loads((NATIVE_APP/'Contents/Info.plist').read_bytes())
        if info.get('CFBundleIdentifier') == 'com.riley.odd-retro-muse': shutil.rmtree(NATIVE_APP)
    if SUPPORT.exists():
        shutil.rmtree(SUPPORT)
    print('Automatic companion removed. Handheld firmware and Muse pairing are unchanged.')

def status():
    for label in (LABEL, LOGIN_LABEL):
        result = launch('print', f'{DOMAIN}/{label}')
        print(label + ': ' + ('registered' if result.returncode == 0 else 'not registered'))
    try:
        with urllib.request.urlopen('http://127.0.0.1:8765/api/status', timeout=3) as response:
            print(json.load(response))
    except OSError:
        print('Companion is not responding.')

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument('--uninstall', action='store_true')
    mode.add_argument('--status', action='store_true')
    parser.add_argument('--wireless-config', type=Path)
    parser.add_argument('--native-app',type=Path)
    args = parser.parse_args()
    if args.uninstall: uninstall()
    elif args.status: status()
    else: install(args.wireless_config,args.native_app)
