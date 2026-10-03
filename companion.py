#!/usr/bin/env python3
"""Local computer input bridge. Credentials stay in the handheld firmware."""
import argparse
import json
import threading
import time
import uuid
from collections import deque
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlsplit, parse_qs
import serial
from wireless_client import WirelessLink

ROOT = Path(__file__).resolve().parent
events = deque(maxlen=100)
event_lock = threading.Lock()
event_changed = threading.Condition(event_lock)
serial_lock = threading.Lock()
status = {"usb": False, "wireless": False, "muse_connected": False, "board": "ModRetro Chromatic", "service_session": str(uuid.uuid4())}
connection = None
wireless = None
counter = 0
voice_lock = threading.RLock()
voice_owner = None
voice_deadline = 0
voice_state = "off"

def serial_send(packet):
    # UART0's default console reads a 128-byte hardware FIFO without a driver
    # receive ring. Pace whole commands so its 20 ms input loop can drain it.
    # Hold the lock across all chunks to keep status and microphone commands
    # from being inserted into a chat JSON line.
    with serial_lock:
        device = connection
        if not device:
            raise serial.SerialException("USB disconnected")
        for offset in range(0, len(packet), 64):
            device.write(packet[offset:offset + 64])
            if offset + 64 < len(packet):
                time.sleep(0.03)

def voice_feedback(state):
    if wireless and wireless.connected:
        wireless.set_voice(state)
    elif connection:
        serial_send(("@voice " + json.dumps({"state": state}) + "\n").encode())

def connected():
    return bool((wireless and wireless.connected) or (status["usb"] and connection))

def wireless_status(info):
    status["wireless"] = info is not None
    if info is not None:
        status["muse_connected"] = info.get("muse_connected") is True
        status["transport"] = "wifi"
        status["busy"] = info.get("busy") is True
    elif not status["usb"]:
        status["muse_connected"] = False
    expire_voice()
    if wireless:
        wireless.set_voice(voice_state)

def expire_voice():
    global voice_owner, voice_state
    with voice_lock:
        if voice_owner and time.monotonic() > voice_deadline:
            voice_owner = None
            voice_state = "off"
            voice_feedback("off")

def control_voice(body):
    global voice_owner, voice_deadline, voice_state
    try:
        client = str(uuid.UUID(body.get("client_id", "")))
    except (ValueError, TypeError, AttributeError):
        return 400, {"error": "Invalid microphone client"}
    op = body.get("op")
    with voice_lock:
        expire_voice()
        if op == "arm":
            if not connected() or not status["muse_connected"]:
                return 503, {"error": "Connect your Chromatic to Muse first"}
            if voice_owner and voice_owner != client:
                return 409, {"error": "Microphone is enabled in another companion tab"}
            voice_owner, voice_state = client, "ready"
            voice_deadline = time.monotonic() + 90
            voice_feedback("ready")
        elif voice_owner != client:
            return 409, {"error": "Enable the handheld microphone again"}
        elif op == "disarm":
            voice_owner, voice_state = None, "off"
            voice_feedback("off")
        elif op == "heartbeat":
            voice_deadline = time.monotonic() + 90
        elif op == "state" and body.get("state") in ("ready", "listening", "stopping"):
            voice_state = body["state"]
            voice_deadline = time.monotonic() + 90
            voice_feedback(voice_state)
        else:
            return 400, {"error": "Invalid microphone action"}
        with event_lock:
            cursor = counter
        return 200, {"armed": voice_owner == client, "state": voice_state, "cursor": cursor}

def event(kind, text):
    global counter
    with event_lock:
        counter += 1
        events.append({"id": counter, "type": kind, "text": text})
        event_changed.notify_all()

def serial_reader(port):
    global connection
    while True:
        try:
            s = serial.Serial(port=None, baudrate=115200, timeout=1)
            s.dtr = False
            s.rts = False
            s.port = port
            s.open()
            connection = s
            status["usb"] = True
            with voice_lock:
                if voice_owner:
                    voice_feedback(voice_state)
            next_check = 0
            while True:
                expire_voice()
                if time.monotonic() >= next_check:
                    serial_send(b"@status\n")
                    next_check = time.monotonic() + 3
                line = s.readline().decode("utf-8", errors="replace").strip()
                if line.startswith("@muse ") and not (wireless and wireless.connected):
                    try:
                        item = json.loads(line[6:])
                        event(item.get("type", "error"), item.get("text", ""))
                    except ValueError:
                        pass
                elif line.startswith("@status "):
                    try:
                        item = json.loads(line[8:])
                        if not (wireless and wireless.connected):
                            status["muse_connected"] = item.get("muse_connected") is True
                            status["transport"] = "usb"
                            with voice_lock:
                                if voice_owner:
                                    voice_feedback(voice_state)
                    except ValueError:
                        pass
        except (serial.SerialException, OSError):
            status["usb"] = False
            if not (wireless and wireless.connected):
                status["muse_connected"] = False
            connection = None
            time.sleep(2)
        finally:
            if connection:
                connection.close()
                connection = None

class Handler(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def respond(self, code, data, content_type="application/json"):
        if isinstance(data, (dict, list)):
            data = json.dumps(data).encode()
        self.send_response(code)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.end_headers()
        self.wfile.write(data)

    def valid_host(self):
        return self.headers.get("Host") in (f"127.0.0.1:{self.server.server_port}", f"localhost:{self.server.server_port}")

    def do_GET(self):
        if not self.valid_host():
            return self.respond(403, {"error": "Local requests only"})
        u = urlsplit(self.path)
        if u.path == "/":
            return self.respond(200, (ROOT / "companion.html").read_bytes(), "text/html; charset=utf-8")
        if u.path == "/preview.png":
            return self.respond(200, (ROOT / "preview.png").read_bytes(), "image/png")
        if u.path == "/voice-controller.mjs":
            return self.respond(200, (ROOT / "voice-controller.mjs").read_bytes(), "text/javascript; charset=utf-8")
        if u.path == "/api/voice/events":
            query = parse_qs(u.query)
            client = query.get("client_id", [""])[0]
            try:
                after = int(query.get("after", ["0"])[0])
            except ValueError:
                return self.respond(400, {"error": "Invalid event cursor"})
            with voice_lock:
                if not voice_owner or client != voice_owner:
                    return self.respond(409, {"error": "Microphone is not enabled"})
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            try:
                while True:
                    with voice_lock:
                        if voice_owner != client or time.monotonic() > voice_deadline:
                            break
                    with event_changed:
                        rows = [e for e in events if e["id"] > after]
                        if not rows:
                            event_changed.wait(timeout=10)
                            rows = [e for e in events if e["id"] > after]
                    data = ": keepalive\n\n"
                    for e in rows:
                        after = max(after, e["id"])
                        if e["type"] in ("voice_toggle", "voice_cancel"):
                            data += "data: " + json.dumps(e) + "\n\n"
                    self.wfile.write(data.encode())
                    self.wfile.flush()
            except (BrokenPipeError, ConnectionResetError, OSError):
                pass
            return
        if u.path == "/api/status":
            return self.respond(200, dict(status, device_connected=connected()))
        if u.path == "/api/events":
            try:
                after = int(parse_qs(u.query).get("after", [0])[0])
            except ValueError:
                return self.respond(400, {"error": "Invalid event cursor"})
            with event_lock:
                result = [e for e in events if e["id"] > after]
            return self.respond(200, result)
        self.respond(404, {"error": "Not found"})

    def do_POST(self):
        origin = self.headers.get("Origin", "")
        allowed = (f"http://127.0.0.1:{self.server.server_port}", f"http://localhost:{self.server.server_port}")
        if not self.valid_host() or origin not in allowed:
            return self.respond(403, {"error": "Use the local companion page"})
        if self.path not in ("/api/ask", "/api/voice") or self.headers.get("Content-Type") != "application/json":
            return self.respond(400, {"error": "Expected a JSON chat message"})
        try:
            size = int(self.headers.get("Content-Length", "0"))
            if not 0 < size <= 4096:
                raise ValueError()
            body = json.loads(self.rfile.read(size))
            if not isinstance(body, dict):
                raise ValueError()
            if self.path == "/api/voice":
                try:
                    code, result = control_voice(body)
                except (serial.SerialException, OSError):
                    return self.respond(503, {"error": "USB disconnected"})
                return self.respond(code, result)
            text = body.get("text", "").strip()
            if not isinstance(text, str) or not 0 < len(text.encode()) < 1024:
                raise ValueError()
            if any(ord(c) < 32 and c not in "\n\r\t" for c in text):
                raise ValueError()
        except (ValueError, TypeError, AttributeError):
            return self.respond(400, {"error": "Message must be 1 to 1023 UTF-8 bytes"})
        if not connected():
            return self.respond(503, {"error": "Connect and power on the Chromatic"})
        if not status["muse_connected"]:
            return self.respond(503, {"error": "Pair the Chromatic in the Muse phone app first"})
        try:
            if wireless and wireless.connected:
                wireless.ask(text)
            else:
                serial_send(("@ask " + json.dumps({"text": text}, ensure_ascii=False) + "\n").encode())
        except (serial.SerialException, OSError) as exc:
            return self.respond(503, {"error": str(exc) or "Handheld disconnected"})
        self.respond(202, {"queued": True})

def main():
    global wireless
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", default="/dev/cu.usbmodem0123456783")
    parser.add_argument("--http-port", type=int, default=8765)
    parser.add_argument("--preview-only", action="store_true")
    parser.add_argument("--wireless-only", action="store_true")
    parser.add_argument("--wireless-key", type=Path, default=Path.home() / 'Library/Application Support/Chromatic Muse/wireless.json')
    args = parser.parse_args()
    if not args.preview_only and args.wireless_key.exists():
        wireless=WirelessLink(args.wireless_key,wireless_status,event)
        wireless.start()
    if not args.preview_only and not args.wireless_only:
        threading.Thread(target=serial_reader, args=(args.port,), daemon=True).start()
    print(f"Chromatic Muse companion: http://127.0.0.1:{args.http_port}", flush=True)
    ThreadingHTTPServer(("127.0.0.1", args.http_port), Handler).serve_forever()

if __name__ == "__main__":
    main()
