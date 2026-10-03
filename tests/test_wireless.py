import json, os, socket, sys, tempfile, threading, unittest, uuid
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
import wireless_client as w

class CodecTest(unittest.TestCase):
    def setUp(self): self.key=os.urandom(32)
    def test_authenticated_utf8_roundtrip(self):
        message={'op':'ask','text':'A long spoken question ☀ '*35}
        packet=w.seal(self.key,message,b'C')
        self.assertEqual(w.unseal(self.key,packet,b'C'),message)
        self.assertNotIn(b'A long spoken question',packet)
    def test_wrong_key_tamper_and_direction_rejected(self):
        packet=w.seal(self.key,{'op':'ask','text':'hello'},b'C')
        for key,p,direction in [(os.urandom(32),packet,b'C'),(self.key,packet[:-1]+bytes([packet[-1]^1]),b'C'),(self.key,packet,b'D')]:
            with self.assertRaises(Exception): w.unseal(key,p,direction)
    def test_nonce_is_fresh_even_for_same_request(self):
        self.assertNotEqual(w.seal(self.key,{'op':'poll'}),w.seal(self.key,{'op':'poll'}))

class RetryTest(unittest.TestCase):
    def test_lost_ack_retries_once_without_double_ask_and_event_is_not_duplicated(self):
        key=os.urandom(32); stop=threading.Event(); asks=[]; errors=[]
        sock=socket.socket(socket.AF_INET,socket.SOCK_DGRAM);sock.bind(('127.0.0.1',0));sock.settimeout(.1)
        oldport=w.PORT;w.PORT=sock.getsockname()[1]
        session=uuid.uuid4().hex;last=0;cached=None;event={'id':1,'type':'voice_toggle','text':'A+B'}
        def device():
            nonlocal last,cached
            try:
                while not stop.is_set():
                    try: packet,peer=sock.recvfrom(4096)
                    except socket.timeout: continue
                    request=w.unseal(key,packet,b'C');seq=request.get('seq',0)
                    if request['op']=='hello':
                        reply={'id':request['id'],'ok':True,'session':session,'status':{'boot':'boot-1','muse_connected':True}}
                    elif request.get('session')!=session:
                        reply={'id':request['id'],'ok':False,'error':'Session expired'}
                    elif seq==last:
                        reply=cached
                    else:
                        self.assertGreater(seq,last);last=seq
                        reply={'id':request['id'],'ok':True,'status':{'boot':'boot-1','muse_connected':True},'event':event}
                        cached=reply
                        if request['op']=='ask':asks.append(request['text']);continue # deliberately lose first ACK
                    sock.sendto(w.seal(key,reply,b'D'),peer)
            except Exception as e: errors.append(e)
        thread=threading.Thread(target=device);thread.start()
        try:
            with tempfile.TemporaryDirectory() as d:
                path=Path(d)/'key.json';path.write_text(json.dumps({'key':key.hex(),'ip':'127.0.0.1'}))
                delivered=[];link=w.WirelessLink(path,lambda _:None,lambda k,t:delivered.append((k,t)))
                link.exchange('hello');link.ask('Please tell me what to do tomorrow and what video to make. '*8)
                link.exchange('poll',ack=0,boot='boot-1',state='ready')
                self.assertEqual(len(asks),1);self.assertEqual(delivered,[('voice_toggle','A+B')])
                link.session='old-session'
                with self.assertRaisesRegex(OSError,'Session expired'):link.exchange('poll')
                self.assertEqual(len(asks),1);link.close()
        finally:
            stop.set();thread.join(timeout=2);sock.close();w.PORT=oldport
        self.assertEqual(errors,[])

if __name__=='__main__':unittest.main()
