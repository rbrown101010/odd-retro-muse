import importlib.util, json, threading, unittest, urllib.request, urllib.error, sys
from unittest.mock import patch
from http.server import ThreadingHTTPServer
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
spec=importlib.util.spec_from_file_location('companion',Path(__file__).resolve().parents[1] / 'companion.py'); m=importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
class Wire:
    def __init__(self): self.sent=[]
    def write(self,b): self.sent.append(b)
class BridgeTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.server=ThreadingHTTPServer(('127.0.0.1',0),m.Handler); cls.origin='http://127.0.0.1:'+str(cls.server.server_port)
        threading.Thread(target=cls.server.serve_forever,daemon=True).start()
    @classmethod
    def tearDownClass(cls): cls.server.shutdown(); cls.server.server_close()
    def setUp(self):
        m.connection=Wire(); m.status.update(usb=True,muse_connected=True)
        m.voice_owner=None; m.voice_state='off'; m.voice_deadline=0
        self.client='00000000-0000-4000-8000-000000000001'
    def voice(self,op,client=None,origin=None,**kwargs):
        req=urllib.request.Request(self.origin+'/api/voice',json.dumps({'op':op,'client_id':client or self.client,**kwargs}).encode(),{'Content-Type':'application/json','Origin':origin or self.origin})
        try:
            with urllib.request.urlopen(req) as r: return r.status,json.load(r)
        except urllib.error.HTTPError as e: return e.code,json.load(e)
    def post(self,text,origin=None):
        r=urllib.request.Request(self.origin+'/api/ask',json.dumps({'text':text}).encode(),{'Content-Type':'application/json','Origin':origin or self.origin})
        try:
            with urllib.request.urlopen(r) as response: return response.status,json.load(response)
        except urllib.error.HTTPError as e: return e.code,json.load(e)
    def test_valid_utf8_routes_to_device(self):
        code,data=self.post('Hello, Muse ☀'); self.assertEqual(code,202); self.assertEqual(len(m.connection.sent),1)
        self.assertEqual(json.loads(m.connection.sent[0].decode()[5:])['text'],'Hello, Muse ☀')
    def test_long_transcript_survives_polled_uart_fifo(self):
        class Fifo:
            def __init__(self): self.pending=bytearray();self.received=bytearray();self.dropped=0
            def write(self,b):
                space=128-len(self.pending);self.pending.extend(b[:space]);self.dropped+=max(0,len(b)-space)
            def drain(self,seconds):
                if seconds>=.02:self.received.extend(self.pending);self.pending.clear()
        fifo=Fifo();m.connection=fifo
        # Just below the supported UTF-8 limit; much larger than UART0's FIFO.
        text='Please tell me what I need to do tomorrow. '+'☀'*320
        with patch.object(m.time,'sleep',side_effect=fifo.drain): code,data=self.post(text)
        fifo.drain(.02)
        self.assertEqual(code,202);self.assertEqual(fifo.dropped,0)
        self.assertEqual(json.loads(fifo.received.decode()[5:])['text'],text)
    def test_paced_chat_cannot_interleave_status(self):
        chat=b'@ask '+json.dumps({'text':'A full spoken question '*30}).encode()+b'\n'
        thread=threading.Thread(target=m.serial_send,args=(chat,));thread.start()
        m.serial_send(b'@status\n');thread.join()
        wire=b''.join(m.connection.sent)
        self.assertIn(wire,(chat+b'@status\n',b'@status\n'+chat))
    def test_unpaired_never_sends(self):
        m.status['muse_connected']=False; self.assertEqual(self.post('hi')[0],503); self.assertEqual(m.connection.sent,[])
    def test_foreign_origin_never_sends(self):
        self.assertEqual(self.post('hi','https://example.org')[0],403); self.assertEqual(m.connection.sent,[])
    def test_size_counts_bytes_not_characters(self):
        self.assertEqual(self.post('☀'*400)[0],400); self.assertEqual(m.connection.sent,[])
    def test_control_character_rejected(self): self.assertEqual(self.post('Hello\u0000there')[0],400)
    def test_empty_rejected(self): self.assertEqual(self.post('')[0],400)
    def test_plain_text_cannot_submit(self):
        req=urllib.request.Request(self.origin+'/api/ask',b'text=hi',{'Content-Type':'text/plain','Origin':self.origin})
        with self.assertRaises(urllib.error.HTTPError) as cm: urllib.request.urlopen(req)
        self.assertEqual(cm.exception.code,400)
    def test_voice_arm_and_state_reach_device(self):
        self.assertEqual(self.voice('arm')[0],200)
        self.assertEqual(self.voice('state',state='listening')[0],200)
        self.assertEqual(json.loads(m.connection.sent[-1].decode()[7:])['state'],'listening')
    def test_second_microphone_tab_cannot_take_over(self):
        self.voice('arm');n=len(m.connection.sent)
        self.assertEqual(self.voice('arm',client='00000000-0000-4000-8000-000000000002')[0],409)
        self.assertEqual(len(m.connection.sent),n)
    def test_foreign_origin_cannot_arm_microphone(self):
        self.assertEqual(self.voice('arm',origin='https://example.org')[0],403)
        self.assertIsNone(m.voice_owner);self.assertEqual(m.connection.sent,[])
    def test_unarmed_state_is_rejected(self):
        self.assertEqual(self.voice('state',state='listening')[0],409);self.assertEqual(m.connection.sent,[])
    def test_expired_microphone_turns_device_off(self):
        self.voice('arm')
        with patch.object(m.time,'monotonic',return_value=m.voice_deadline+1): m.expire_voice()
        self.assertIsNone(m.voice_owner)
        self.assertEqual(json.loads(m.connection.sent[-1].decode()[7:])['state'],'off')
    def test_invalid_voice_state_never_reaches_device(self):
        self.voice('arm');n=len(m.connection.sent)
        self.assertEqual(self.voice('state',state='anything')[0],400);self.assertEqual(len(m.connection.sent),n)
    def test_voice_stream_delivers_button_events(self):
        code,data=self.voice('arm');self.assertEqual(code,200)
        url=self.origin+'/api/voice/events?client_id='+self.client+'&after='+str(data['cursor'])
        with urllib.request.urlopen(url,timeout=3) as response:
            m.event('voice_toggle','A+B')
            for _ in range(8):
                line=response.readline().decode()
                if line.startswith('data: '):
                    self.assertEqual(json.loads(line[6:])['type'],'voice_toggle');break
            else: self.fail('No voice event arrived')
if __name__=='__main__': unittest.main()
