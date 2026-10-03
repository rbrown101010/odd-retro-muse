"""Encrypted, acknowledged local Wi-Fi transport for the Chromatic.

No cloud credentials. A random 256-bit key is provisioned by physical USB once.
Fresh device challenges bind each connection; ordered requests prevent replay,
and retrying the same request receives its cached response without re-execution.
"""
from collections import deque
import json
import os
from pathlib import Path
import socket
import threading
import time
import uuid
from cryptography.hazmat.primitives.ciphers.aead import AESGCM

PORT = 39077

def seal(key, body, direction=b'C'):
    header=b'CMW1'+direction
    nonce=os.urandom(12)
    data=json.dumps(body,separators=(',', ':'),ensure_ascii=False).encode()
    if len(data)>3072: raise ValueError('Wireless request is too large')
    return header+nonce+AESGCM(key).encrypt(nonce,data,header)

def unseal(key, packet, direction=b'D'):
    header=b'CMW1'+direction
    if len(packet)<33 or packet[:5]!=header: raise ValueError('Invalid wireless packet')
    return json.loads(AESGCM(key).decrypt(packet[5:17],packet[17:],header))

class WirelessLink:
    def __init__(self, config_path, on_status, on_event):
        self.path=Path(config_path)
        config=json.loads(self.path.read_text())
        self.key=bytes.fromhex(config['key'])
        if len(self.key)!=32: raise ValueError('Invalid wireless key')
        self.ip=config.get('ip','255.255.255.255')
        self.config=config
        self.on_status=on_status;self.on_event=on_event
        self.sock=socket.socket(socket.AF_INET,socket.SOCK_DGRAM)
        self.sock.setsockopt(socket.SOL_SOCKET,socket.SO_BROADCAST,1)
        self.sock.bind(('',0))
        self.lock=threading.Lock();self.stop=threading.Event()
        self.session='';self.seq=0;self.boot='';self.ack=0
        self.connected=False;self.voice_state='off'

    def start(self):
        threading.Thread(target=self.run,name='Chromatic Wi-Fi',daemon=True).start()

    def set_voice(self,state):
        self.voice_state=state

    def accept(self,response,ip):
        info=response.get('status')
        if not isinstance(info,dict): return
        self.connected=True
        if ip!=self.ip:
            self.ip=ip;self.config['ip']=ip
            temp=self.path.with_suffix('.tmp')
            fd=os.open(temp,os.O_WRONLY|os.O_CREAT|os.O_TRUNC,0o600)
            with os.fdopen(fd,'w') as out: json.dump(self.config,out)
            os.chmod(temp,0o600);os.replace(temp,self.path)
        if info.get('boot')!=self.boot:
            self.boot=info['boot'];self.ack=0
        self.on_status(info)
        item=response.get('event')
        if isinstance(item,dict) and isinstance(item.get('id'),int) and item['id']>self.ack:
            self.ack=item['id'];self.on_event(item.get('type','error'),item.get('text',''))

    def exchange(self,op,**fields):
        with self.lock:
            request={'op':op,'id':uuid.uuid4().hex,**fields}
            if op!='hello':
                if not self.session: raise OSError('Wireless connection is not ready')
                self.seq+=1;request.update(session=self.session,seq=self.seq)
            destinations=[self.ip]
            if op=='hello' and self.ip!='127.0.0.1' and self.ip!='255.255.255.255': destinations.append('255.255.255.255')
            for attempt in range(3):
                packet=seal(self.key,request)
                for ip in destinations: self.sock.sendto(packet,(ip,PORT))
                deadline=time.monotonic()+.6
                while time.monotonic()<deadline:
                    self.sock.settimeout(max(.01,deadline-time.monotonic()))
                    try: packet,peer=self.sock.recvfrom(4096)
                    except socket.timeout: break
                    if peer[1]!=PORT: continue
                    try: response=unseal(self.key,packet)
                    except Exception: continue
                    if not isinstance(response,dict) or response.get('id')!=request['id']: continue
                    if not response.get('ok'): raise OSError(response.get('error','Wireless request failed'))
                    if op=='hello':
                        session=response.get('session','')
                        if len(session)!=32: raise OSError('Invalid wireless session')
                        self.session=session;self.seq=0;self.ack=0
                    self.accept(response,peer[0])
                    return response
            raise OSError('Handheld did not acknowledge the request. Check Muse before retrying.')

    def ask(self,text):
        return self.exchange('ask',text=text,boot=self.boot,ack=self.ack)

    def run(self):
        while not self.stop.is_set():
            try:
                if not self.session: self.exchange('hello')
                self.exchange('poll',state=self.voice_state,boot=self.boot,ack=self.ack)
            except (OSError,ValueError):
                self.connected=False;self.session='';self.on_status(None)
            self.stop.wait(.2 if self.connected and self.voice_state!='off' else 1)

    def close(self):
        self.stop.set();self.sock.close()
