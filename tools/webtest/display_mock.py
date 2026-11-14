#!/usr/bin/env python3
"""Mock CarMetrics Display (the touch screen's own web page) for the browser test.
usage: display_mock.py <index.html> <port> [width height profile]"""
import base64, json, sys, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PAGE, PORT = sys.argv[1], int(sys.argv[2])
SCR_W, SCR_H, PROFILE = (int(sys.argv[3]), int(sys.argv[4]), sys.argv[5]) if len(sys.argv) > 5 else (320, 480, "JC3248W535")
SENSORS = [dict(id="coolant_temp", name="Coolant", label="CLT", unit="°C", decimals=0, lo=40, hi=130, wh=100, ch=108, board=0),
           dict(id="egt", name="Exhaust gas", label="EGT", unit="°C", decimals=0, lo=0, hi=900, wh=700, ch=780, board=0),
           dict(id="boost", name="Boost", label="BST", unit="bar", decimals=2, lo=-0.8, hi=2, wh=1.4, ch=1.6, board=0),
           dict(id="oil_press", name="Oil pressure", label="OIL", unit="bar", decimals=1, lo=0, hi=8, wl=1, cl=0.5, board=0)]
RELAYS = [dict(ref="r:0:0", name="Fan", board=0), dict(ref="r:1:0", name="Engine bay Pump", board=1)]
layout = dict(default=True, alarmPopup=True, relaysPage=True, pages=[dict(name="Main", cols=2, rows=3, tiles=[dict(ref=s["id"], w="dial") for s in SENSORS])])
settings = dict(name="Dash", brightness=80, rotation=255, dimAfterMin=0, panel="ili9341", wifi=True, mainSsid="",
                touch=dict(xMin=300, xMax=3800, yMin=300, yMax=3800, swapXY=False, flipX=False, flipY=False),
                mainPass="", mainAdmin="", apPass="", adminPass="")
fw = dict(bytes=0)

class H(BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def send_json(self, code, d, hdr=None):
        b = json.dumps(d).encode()
        self.send_response(code); self.send_header("Content-Type", "application/json"); self.send_header("Content-Length", str(len(b)))
        for k, v in (hdr or {}).items(): self.send_header(k, v)
        self.end_headers(); self.wfile.write(b)
    def authed(self):
        a = self.headers.get("Authorization", "")
        if a.startswith("Basic ") and base64.b64decode(a[6:]).decode() == "admin:carmetrics": return True
        self.send_json(401, {"ok": False, "message": "auth"}, {"WWW-Authenticate": 'Basic realm="CarMetrics Display"'}); return False
    def body(self):
        n = int(self.headers.get("Content-Length", 0)); return self.rfile.read(n) if n else b""
    def do_GET(self):
        p = self.path.split("?")[0]
        if p == "/":
            b = open(PAGE, "rb").read()
            self.send_response(200); self.send_header("Content-Type", "text/html"); self.send_header("Content-Length", str(len(b))); self.end_headers(); self.wfile.write(b); return
        if p == "/api/state":
            return self.send_json(200, dict(name=settings["name"], firmware="5.1.0-beta.11", profile=PROFILE, chip="ESP32-S3" if "JC" in PROFILE else "ESP32", width=SCR_W, height=SCR_H,
                rotation=255, brightness=settings["brightness"], heap=180000, psram=7900000 if "JC" in PROFILE else 0, uptimeS=640, main="carmetrics", mainFirmware="5.1.0-beta.11",
                catalogRev=4660, level=1, muted=False,
                net=dict(link="espnow", espnow=dict(paired=True, id=2, up=True, inRange=True, channel=6, encrypted=False, rx=900, tx=880, valuesAgeMs=300),
                         wifi=dict(enabled=True, ssid="CarMetrics", connected=True, ip="192.168.4.3", rssi=-44, valuesAgeMs=-1),
                         ap=dict(name="CarMetrics-Display-0A0B", ip="192.168.5.1", clients=1))))
        if p == "/api/catalog": return self.send_json(200, dict(rev=4660, sensors=SENSORS, relays=RELAYS))
        if p == "/api/values":
            t = time.time()
            return self.send_json(200, dict(rev=4660, level=1, muted=False, link=1, v=[[92, 2, 0], [655, 2, 0], [1.47, 2, 1], [3.4, 2, 0], [1, 2, 0], [None, 6, 0]]))
        if p == "/api/layout": return self.send_json(200, layout)
        if p == "/api/settings": return self.send_json(200, settings)
        if p == "/api/touch": return self.send_json(200, dict(touched=True, x=1234, y=2345))
        return self.send_json(404, {"ok": False, "message": "Not found"})
    def do_POST(self):
        global layout
        p = self.path.split("?")[0]
        if p == "/api/identify": self.body(); return self.send_json(200, {"ok": True, "message": "Flashing the screen"})
        if not self.authed(): self.body(); return
        b = self.body()
        if p == "/api/layout":
            j = json.loads(b)
            for pg in j["pages"]:
                if not (1 <= pg["cols"] <= 4 and 1 <= pg["rows"] <= 6): return self.send_json(400, {"ok": False, "message": "grid: 1-4 columns, 1-6 rows"})
            j["default"] = False; layout = j
            return self.send_json(200, {"ok": True, "message": "Layout saved"})
        if p == "/api/settings":
            j = json.loads(b)
            for k in ("mainPass", "mainAdmin", "apPass", "adminPass"): j[k] = ""
            settings.update(j); return self.send_json(200, {"ok": True, "message": "Saved"})
        if p == "/api/update":
            fw["bytes"] = len(b)
            if not b or b[0] != 0xE9: return self.send_json(400, {"ok": False, "message": "Not a firmware file (use carmetrics-display-...-firmware.bin)"})
            return self.send_json(200, {"ok": True, "message": "Firmware installed, restarting"})
        if p == "/api/layout-reset": return self.send_json(200, {"ok": True, "message": "Back to the automatic layout"})
        if p == "/api/reboot": return self.send_json(200, {"ok": True, "message": "Restarting"})
        return self.send_json(404, {"ok": False, "message": "Not found"})

ThreadingHTTPServer(("127.0.0.1", PORT), H).serve_forever()
