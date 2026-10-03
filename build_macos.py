#!/usr/bin/env python3
"""Build the native macOS companion. No credentials are embedded."""
import argparse
import os
import platform
from pathlib import Path
import plistlib
import subprocess

def main():
    root=Path(__file__).resolve().parent
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,default=root/'dist/Odd Retro Muse.app')
    parser.add_argument('--arch',choices=['arm64','x86_64'],default=platform.machine())
    args=parser.parse_args()
    app=args.output.resolve();contents=app/'Contents';binary=contents/'MacOS/OddRetroMuse'
    binary.parent.mkdir(parents=True,exist_ok=True);(contents/'Resources').mkdir(exist_ok=True)
    sdk=subprocess.check_output(['xcrun','--sdk','macosx','--show-sdk-path'],text=True).strip()
    sources=[str(root/'macos'/name) for name in ('App.swift','Bridge.swift','Controller.swift','MicrophoneRecorder.swift','Keychain.swift','Transcription.swift')]
    subprocess.run(['xcrun','swiftc','-swift-version','5','-parse-as-library','-O','-sdk',sdk,
                    '-target',args.arch+'-apple-macosx14.0',*sources,'-o',str(binary)],check=True)
    info={'CFBundleExecutable':'OddRetroMuse','CFBundleIdentifier':'com.riley.odd-retro-muse',
          'CFBundleName':'Odd Retro Muse','CFBundleDisplayName':'Odd Retro Muse',
          'CFBundlePackageType':'APPL','CFBundleVersion':'2','CFBundleShortVersionString':'2.0',
          'LSMinimumSystemVersion':'14.0','NSHighResolutionCapable':True,
          'NSMicrophoneUsageDescription':'Use the Mac microphone when you press A+B on the Chromatic. OpenAI transcribes the recording after you stop and send.',
          'NSAppTransportSecurity':{'NSAllowsLocalNetworking':True}}
    (contents/'Info.plist').write_bytes(plistlib.dumps(info))
    for attr in ('com.apple.FinderInfo','com.apple.ResourceFork'):
        subprocess.run(['/usr/bin/xattr','-dr',attr,str(app)],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
    subprocess.run(['/usr/bin/codesign','--force','--sign','-','--identifier','com.riley.odd-retro-muse',str(app)],check=True)
    print('Built native Mac app:',app)
if __name__=='__main__':main()
