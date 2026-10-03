#!/usr/bin/env python3
"""Provision the local encrypted Wi-Fi bridge once over the USB cable."""
import argparse
import json
import os
from pathlib import Path
import time
import serial

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port',default='/dev/cu.usbmodem0123456783')
    parser.add_argument('--config',required=True,type=Path)
    parser.add_argument('--allow-offline',action='store_true',help='Provision the key before phone Wi-Fi setup')
    args=parser.parse_args()
    args.config.parent.mkdir(parents=True,exist_ok=True,mode=0o700)
    if args.config.exists(): key=json.loads(args.config.read_text())['key']
    else: key=os.urandom(32).hex()
    s=serial.Serial(port=None,baudrate=115200,timeout=1)
    s.dtr=False;s.rts=False;s.port=args.port;s.open()
    def write(packet):
        for i in range(0,len(packet),64):s.write(packet[i:i+64]);time.sleep(.03)
    ok=False;ip=None;deadline=time.monotonic()+40;next_send=0
    try:
        while time.monotonic()<deadline:
            if time.monotonic()>next_send:
                if not ok: write(('@bridge '+json.dumps({'key':key})+'\n').encode())
                write(b'@status\n');next_send=time.monotonic()+2
            line=s.readline().decode(errors='replace').strip()
            try:
                if line.startswith('@bridge '):ok=json.loads(line[8:]).get('ok') is True
                if line.startswith('@status '):
                    j=json.loads(line[8:]);ip=j.get('ip')
                    if ok and j.get('wireless_configured') and (args.allow_offline or (ip and ip!='0.0.0.0')):break
            except ValueError:pass
        if not ok or (not args.allow_offline and (not ip or ip=='0.0.0.0')):raise SystemExit('Wireless provisioning did not finish. Keep USB connected and wait for Wi-Fi.')
        if not ip or ip=='0.0.0.0':ip='255.255.255.255'
        fd=os.open(args.config,os.O_WRONLY|os.O_CREAT|os.O_TRUNC,0o600)
        with os.fdopen(fd,'w') as out:json.dump({'key':key,'ip':ip},out)
        os.chmod(args.config,0o600)
        print('Encrypted wireless bridge provisioned. The key was saved privately.')
    finally:s.close()

if __name__=='__main__':main()
