#!/usr/bin/env python3
"""User-facing launcher, also run once by launchd at login."""
import os
from pathlib import Path
import subprocess
import time
import urllib.request

URL = 'http://127.0.0.1:8765/'

def main():
    subprocess.run(['/bin/launchctl', 'kickstart', f'gui/{os.getuid()}/com.riley.chromatic-muse'],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for _ in range(60):
        try:
            with urllib.request.urlopen(URL + 'api/status', timeout=1) as response:
                if response.status == 200:
                    break
        except OSError:
            time.sleep(1)
    else:
        raise SystemExit('Chromatic Muse companion did not start. Run the installer again to repair it.')
    subprocess.run(['/usr/bin/open', str(Path.home() / 'Applications/Odd Retro Muse.app')], check=True)

if __name__ == '__main__':
    main()
