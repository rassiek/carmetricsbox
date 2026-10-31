#!/usr/bin/env python3
"""Mock CarMetrics ESP32 API for UI testing. Serves web/ and fakes the REST + SSE endpoints."""
import base64, json, math, os, random, sys, threading, time
from http.server import ThreadingHTTPServer, BaseHTTPRequestHandler

WEB = sys.argv[1]
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 8099
ADMIN = "carmetrics"
T0 = time.time()

def ds(i, n, l, st, gmin, gmax, w, c, addr=""):
    return dict(id=i, name=n, unit="°C", type="ds18b20", enabled=True, address=addr, pin=-1, sckPin=-1, soPin=-1,
                adsAddress=72, adsChannel=0, bmpField="temperature", divider=1, inMin=0, inMax=1, outMin=0, outMax=1,
                faultLowV=None, faultHighV=None, offset=0, smoothing=1, decimals=0, gaugeMin=gmin, gaugeMax=gmax,
                oledStyle=st, oledLabel=l, warnHigh=w, critHigh=c, warnLow=None, critLow=None, hysteresis=1, engineRunningOnly=False)

def an(i, n, u, pin, div, i0, i1, o0, o1, **kw):
    d = ds(i, n, "", "bar", 0, 100, None, None)
    d.update(unit=u, type="adc", pin=pin, divider=div, inMin=i0, inMax=i1, outMin=o0, outMax=o1, smoothing=0.3)
    d.update(kw)
    return d

cfg = {
    "version": "4.0.0",
    "system": dict(board="auto", apSsid="CarMetrics", apPass="", hostname="carmetrics", adminPass="",
                   networks=[dict(ssid="Phone", **{"pass": ""}, passSet=True, priority=8)], scanWhileApInUse=True,
                   ignitionPin=-1, ignitionActiveLow=True, sleepAfterMinutes=0, gpsRxPin=-1, gpsTxPin=-1, gpsBaud=9600,
                   i2cSda=21, i2cScl=22, oneWirePin=4, oneWireResolution=11, oledType="ssd1327_128x128", oledFlip=False,
                   oledClock=True, oledPageSeconds=4, oledContrast=255, buzzerPin=18, buzzerActiveLow=False, ledPin=19,
                   ledActiveLow=False, engineSensorId="bat_voltage", engineRunningAbove=13.2, engineDelaySeconds=5,
                   seaLevelHpa=1013.25, historyIntervalSeconds=5, tzOffsetMinutes=180),
    "sensors": [
        ds("coolant_temp", "Coolant", "CT", "bar", 15, 120, 100, 105, "28FF641E8C160321"),
        ds("intake_temp", "Intake air", "I", "text", 0, 80, 55, 65, "28FF0A2B8C1603AA"),
        ds("gearbox_temp", "Gearbox", "G", "text", 0, 120, 90, 105),
        ds("transfer_case_temp", "Transfer case", "T", "text", 0, 120, 90, 105),
        dict(ds("egt", "EGT", "EGT", "bar", 0, 800, 600, 700), type="max6675", sckPin=5, pin=17, soPin=16, offset=-8, hysteresis=10),
        an("boost_pressure", "Boost", "psi", 32, 1.5, 0.5, 4.5, 0, 30, decimals=1, gaugeMax=25, warnHigh=17, critHigh=20, faultLowV=0.2, faultHighV=4.8, oledLabel="BST"),
        an("oil_pressure", "Oil pressure", "psi", 34, 1.5, 0.5, 4.5, 0, 100, warnLow=15, critLow=7, warnHigh=85, faultLowV=0.2, faultHighV=4.8, engineRunningOnly=True, oledLabel="OIL"),
        an("bat_voltage", "Starter battery", "V", 35, 5, 0, 1, 0, 1, decimals=1, gaugeMin=8, gaugeMax=16, warnLow=11.8, critLow=11.2, warnHigh=14.8, critHigh=15.2, oledStyle="text", oledLabel="B"),
        an("aux_voltage", "Aux battery", "V", 33, 5, 0, 1, 0, 1, enabled=False, decimals=1, gaugeMin=8, gaugeMax=16, oledStyle="text", oledLabel="A"),
        dict(ds("cabin_temp", "Cabin", "C", "text", 0, 50, 40, None), type="bmp"),
        dict(ds("altitude", "Altitude", "^", "text", 0, 3000, None, None), type="bmp", unit="m", bmpField="altitude"),
    ],
    "weather": dict(enabled=True, refTown="Nairobi", autoSeaLevel=True, refreshMinutes=20,
                    towns=[dict(name="Nairobi", lat=-1.2864, lon=36.8172), dict(name="Naivasha", lat=-0.7167, lon=36.431), dict(name="Mombasa", lat=-4.0435, lon=39.6682)]),
    "cloud": dict(enabled=False, protocol="zabbix", intervalMs=60000, uploadEverySec=60, bufferKB=256, onlyEngineRunning=True,
                  zbxServer="", zbxPort=10051, zbxHost="carmetrics", zbxPacked=True, zbxPskIdentity="", zbxPsk="", zbxPskSet=False,
                  influxUrl="", influxAuth="", influxAuthSet=False, influxMeasurement="carmetrics"),
    "turbo": dict(enabled=True, egtId="egt", safeEgt=200, minIdleSeconds=60, rpmId="", idleRpm=1000, speedId="", beep=True),
    "layout": dict(tiles=[], oled=[]),
    "blackbox": dict(enabled=True, keepEvents=10),
    "relays": [dict(id="relay1", name="Relay 1", pin=26, mode="on_off", activeLow=True, defaultOn=False, blinkMs=500, enabled=True),
               dict(id="relay2", name="Fog lights", pin=27, mode="blink", activeLow=True, defaultOn=False, blinkMs=500, enabled=True)],
}
rev = 1
relay_state = {r["id"]: dict(on=False, out=False, blinkMs=r["blinkMs"]) for r in cfg["relays"]}
muted_until = 0
clock_set = False   # like a freshly booted ESP32: no time until the web app sends it
lock = threading.Lock()

def value(s, t):
    base = {"coolant_temp": 88 + 18 * math.sin(t / 20), "intake_temp": 38, "egt": 420 + 200 * math.sin(t / 15),
            "boost_pressure": 9 + 8 * math.sin(t / 7), "oil_pressure": 45 + 20 * math.sin(t / 11), "bat_voltage": 14.1,
            "cabin_temp": 24.5, "altitude": 1661}.get(s["id"])
    if s["id"] == "transfer_case_temp": return None, "unassigned"
    if s["id"] == "gearbox_temp": return None, "not_found"
    if base is None: return 21.0 + random.random(), "ok"
    return base + random.random() * 0.4, "ok"

def level(s, v, st):
    if st in ("fault", "not_found"): return "fault"
    if v is None: return "ok"
    for key, lv in (("critHigh", "crit"), ("critLow", "crit"), ("warnHigh", "warn"), ("warnLow", "warn")):
        th = s.get(key)
        if th is None: continue
        if ("High" in key and v >= th) or ("Low" in key and v <= th): return lv
    return "ok"

def state():
    t = time.time() - T0
    out = {"t": int(t * 1000), "rev": rev, "engine": True, "sensors": {}, "relays": relay_state}
    order = ["ok", "warn", "fault", "crit"]
    worst = "ok"
    for s in cfg["sensors"]:
        if not s["enabled"]:
            out["sensors"][s["id"]] = {"v": None, "st": "disabled", "lvl": "ok"}; continue
        v, st = value(s, t)
        lv = level(s, v, st)
        if order.index(lv) > order.index(worst): worst = lv
        e = {"v": None if v is None else round(v, s["decimals"] + 1), "st": st, "lvl": lv}
        if s["type"] in ("adc", "ads1115") and v is not None:
            e["volts"] = round(s["inMin"] + (v - s["outMin"]) * (s["inMax"] - s["inMin"]) / (s["outMax"] - s["outMin"]), 3)
        out["sensors"][s["id"]] = e
    out["level"] = worst
    egt = out["sensors"].get("egt", {}).get("v")
    if cfg["turbo"]["enabled"] and egt is not None:
        out["turbo"] = {"phase": "cooling" if egt > cfg["turbo"]["safeEgt"] else "ok", "leftS": 25, "safe": cfg["turbo"]["safeEgt"], "egt": round(egt)}
    if not clock_set: out["clock"] = False
    else: out["epoch"] = int(time.time()); out["clockSrc"] = "browser"
    out["muted"] = time.time() < muted_until
    if out["muted"]: out["muteLeftS"] = int(muted_until - time.time())
    return out

history = {s["id"]: [] for s in cfg["sensors"]}
def hist_fill():
    for k in range(120):
        t = (time.time() - T0) - (120 - k) * 5
        for s in cfg["sensors"]:
            v, st = value(s, t)
            history[s["id"]].append(None if v is None else round(v, 2))
hist_fill()

class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def log_message(self, *a): pass
    def send_json(self, code, obj, extra=None):
        b = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json"); self.send_header("Content-Length", str(len(b)))
        for k, v in (extra or {}).items(): self.send_header(k, v)
        self.end_headers(); self.wfile.write(b)
    def authed(self):
        a = self.headers.get("Authorization", "")
        if a.startswith("Basic ") and base64.b64decode(a[6:]).decode() == "admin:" + ADMIN: return True
        self.send_json(401, {"ok": False, "message": "auth"}, {"WWW-Authenticate": 'Basic realm="CarMetrics"'})
        return False
    def body(self):
        n = int(self.headers.get("Content-Length", 0)); return self.rfile.read(n) if n else b""
    def do_GET(self):
        global rev
        p = self.path.split("?")[0]
        if p == "/ws" and self.headers.get("Upgrade", "").lower() == "websocket":
            return self.websocket()
        if p == "/events":
            self.send_response(200); self.send_header("Content-Type", "text/event-stream"); self.send_header("Cache-Control", "no-cache")
            self.end_headers()
            i = 0
            try:
                while True:
                    i += 1
                    self.wfile.write(f"id: {i}\nevent: state\ndata: {json.dumps(state())}\n\n".encode()); self.wfile.flush()
                    time.sleep(1)
            except Exception: return
        if p == "/api/config": return self.send_json(200, dict(cfg, revision=rev))
        if p == "/api/state": return self.send_json(200, state())
        if p == "/api/history": return self.send_json(200, {"interval": 5, "len": 360, "count": 120, "ageMs": 1000, "series": history})
        if p == "/api/info": return self.send_json(200, dict(version="4.0.0", build="Oct 30 2026", uptimeS=int(time.time() - T0) + 3700, heapFree=180000, heapMin=150000,
                fsUsed=60000, fsTotal=589824, sketchSize=1037125, sketchFree=1703936, apSsid="CarMetrics", apIp="192.168.4.1", staSsid="", ip="",
                rssi=0, hostname="carmetrics", stationHpa=834.2, seaLevelHpa=1016.4, seaLevelSource="weather: Nairobi", internet=True, ntp=True, bmp="BMP180", oled="ok", timeSynced=clock_set, timeSource="browser", resetReason=1, sseClients=1))
        if p == "/api/v2/board":
            r = lambda a, b: list(range(a, b + 1))
            valid = [x for x in r(0, 39) if x not in r(6, 11) + [20, 24] + r(28, 31)]
            dev = dict(id="esp32_devkit", name="ESP32 DevKit (WROOM-32, 4 MB)", chip="esp32", flashMB=4, psramMB=0, valid=valid,
                       output=[x for x in valid if x < 34 and x not in (0, 12)], adc1=r(32, 39), caution=[1, 2, 3, 5, 15],
                       defaults=dict(i2cSda=21, i2cScl=22, oneWirePin=4, buzzerPin=18, ledPin=19, egtSck=5, egtCs=17, egtSo=16),
                       notes="Your current board. GPIO34-39 are input only. Analog inputs: GPIO32-39.")
            wro = dict(dev, id="esp32_wrover", name="ESP32 WROVER (4-8 MB flash, PSRAM)", psramMB=8, notes="GPIO16/17 are used by the PSRAM.")
            return self.send_json(200, dict(chip="esp32", setting=cfg["system"].get("board", "auto"), detected="esp32_devkit", current=dev, profiles=[dev, wro]))
        if p == "/api/v2/system":
            up = int(time.time() - T0) + 3700
            return self.send_json(200, dict(chip=dict(model="ESP32-D0WD-V3", revision=3, cores=2, mhz=240, flashMB=4, psramKB=0, target="esp32", tempC=48.5),
                board=dict(id="esp32_devkit", name="ESP32 DevKit (WROOM-32, 4 MB)", setting="auto"),
                firmware=dict(version="5.0.0-dev", build="Oct 31 2026", core="2.0.17", idf="v4.4.7", slot="app0", state="confirmed", rollbackAvailable=True, sketchKB=1255, slotKB=1664),
                uptime=dict(s=up, boots=42, resetReason="power on", history=["power on", "restart", "power on", "low voltage (brown-out)"]),
                safeMode=False, crashCount=0, memory=dict(free=182000, min=151000, largest=110592, psramFree=0, historyKB=10, historyIn="RAM", fsUsed=60000, fsTotal=589824),
                cpu=[18, 9], loop=dict(avgUs=840, maxUs=6200), tasks=[dict(name="loop", stackFree=4920), dict(name="oled", stackFree=2810), dict(name="net", stackFree=7400)],
                wifi=dict(connected=True, ssid="Phone", rssi=-61, channel=6, apClients=1), flash=dict(configWrites=1, bufferWrites=0, bufferKB=0),
                clients=dict(events=1, websocket=0)))
        if p == "/api/v2/wifi":
            up = int(time.time() - T0) + 3700
            return self.send_json(200, dict(uptimeS=up, state="connected", ssid="HouseLAN", channel=6, saved=2, apClients=1, rssi=-58, ip="192.168.1.40",
                remembered=dict(ssid="HouseLAN", channel=6, bssid="A4:2B:B0:11:22:33"), scanAgeS=95, scanTotal=7,
                scan=[dict(ssid="HouseLAN", rssi=-58, ch=6, security="WPA2", saved=True), dict(ssid="Neighbour", rssi=-80, ch=11, security="WPA2/WPA3", saved=False),
                      dict(ssid="(hidden)", rssi=-85, ch=1, security="WPA2", saved=False)],
                log=[dict(t=up - 90, event="joined", ssid="HouseLAN", bssid="A4:2B:B0:11:22:33", ch=6, rssi=-58, **{"try": 2}, ms=2400, note="192.168.1.40"),
                     dict(t=up - 95, event="failed", ssid="HouseLAN", bssid="A4:2B:B0:11:22:33", ch=6, rssi=-60, **{"try": 1}, reason=15, reasonText="4-way handshake timeout (wrong password?)", ms=5100),
                     dict(t=up - 400, event="no saved network in range", note="5 network(s) seen", ms=2600)]))
        if p == "/api/blackbox":
            return self.send_json(200, dict(enabled=True, keep=10, events=[dict(seq=3, bytes=5200, epoch=int(time.time()) - 3600, uptimeS=1820, reason="coolant_temp warn"),
                                                                        dict(seq=2, bytes=5100, epoch=0, uptimeS=640, reason="hot shutdown: EGT 412 C")]))
        if p == "/api/blackbox/event":
            ids = [x["id"] for x in cfg["sensors"] if x["enabled"]]
            rows = [[round(95 + 0.25 * k + (0.5 if i == 0 else i), 2) if i < 8 else None for i in range(len(ids))] for k in range(61)]
            return self.send_json(200, dict(seq=3, epoch=int(time.time()) - 3600, uptimeS=1820, reason="coolant_temp warn", interval=1, before=30, ids=ids, rows=rows))
        if p == "/api/v2/points":
            st = state()
            pts = [dict(a="local." + s["id"], k="sensor", n=s["name"], u=s["unit"], v=st["sensors"][s["id"]]["v"]) for s in cfg["sensors"]]
            return self.send_json(200, dict(board="local", rev=rev, points=pts))
        if p == "/api/weather":
            mk = lambda n, t, msl, sfc, el: dict(name=n, time="2026-10-31T11:45", code=2, temp=t, feels=t-1, rh=55, precip=0.0, cloud=40, msl=msl, sfc=sfc, wind=14, windDir=60, gust=28, elevation=el)
            return self.send_json(200, dict(enabled=True, sta=True, ageS=240, error="", seaLevel=dict(hpa=1016.4, source="weather", town="Nairobi"),
                towns=[mk("Nairobi", 23.4, 1016.4, 832.1, 1661), mk("Naivasha", 24.9, 1015.8, 813.0, 1890), mk("Mombasa", 30.2, 1012.9, 1010.8, 17)]))
        if p == "/api/cloud":
            return self.send_json(200, dict(enabled=cfg["cloud"]["enabled"], sta=True, internet=True, ntp=True, timeSynced=True, recording=True,
                ramChunks=1, backlogFiles=3, backlogBytes=11264, pendingRecords=88, sentRecords=1520, droppedRecords=0, lastOkAgeS=42, lastTryAgeS=42, lastError="", recordBytes=142, fsFree=480000))
        if p == "/api/onewire": return self.send_json(200, {"lastScanAgeMs": 3000, "scanPending": False, "devices": [
                {"address": "28FF641E8C160321", "tempC": 88.1, "assignedTo": "coolant_temp"},
                {"address": "28FF0A2B8C1603AA", "tempC": 38.4, "assignedTo": "intake_temp"},
                {"address": "28FFB2C18C160377", "tempC": 61.0, "assignedTo": ""}]})
        if p in ("/api/auth/check", "/api/config/export"):
            if self.authed(): self.send_json(200, {"ok": True, "message": "ok"} if p.endswith("check") else cfg)
            return
        f = os.path.join(WEB, "index.html" if p == "/" else p.lstrip("/"))
        if os.path.isfile(f):
            b = open(f, "rb").read()
            ct = {"html": "text/html", "js": "application/javascript", "css": "text/css", "png": "image/png"}.get(f.rsplit(".", 1)[-1], "application/octet-stream")
            self.send_response(200); self.send_header("Content-Type", ct + ("; charset=utf-8" if "text" in ct or "javascript" in ct else "")); self.send_header("Content-Length", str(len(b))); self.end_headers(); self.wfile.write(b)
        else:
            self.send_json(404, {"ok": False, "message": "Not found"})
    # Minimal WebSocket (RFC 6455) like the ESP32's /ws: state every second, commands answered with an ack.
    def websocket(self):
        global muted_until
        import hashlib, struct
        key = self.headers["Sec-WebSocket-Key"]
        acc = base64.b64encode(hashlib.sha1((key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest()).decode()
        self.send_response(101); self.send_header("Upgrade", "websocket"); self.send_header("Connection", "Upgrade")
        self.send_header("Sec-WebSocket-Accept", acc); self.end_headers()
        wlock = threading.Lock(); alive = [True]
        def send(obj):
            b = json.dumps(obj).encode()
            hdr = bytes([0x81, len(b)]) if len(b) < 126 else bytes([0x81, 126]) + struct.pack(">H", len(b)) if len(b) < 65536 else bytes([0x81, 127]) + struct.pack(">Q", len(b))
            with wlock: self.wfile.write(hdr + b); self.wfile.flush()
        def reader():
            try:
                while alive[0]:
                    h = self.rfile.read(2)
                    if len(h) < 2: break
                    op, n = h[0] & 15, h[1] & 127
                    if n == 126: n = struct.unpack(">H", self.rfile.read(2))[0]
                    elif n == 127: n = struct.unpack(">Q", self.rfile.read(8))[0]
                    mask = self.rfile.read(4); data = bytes(c ^ mask[i % 4] for i, c in enumerate(self.rfile.read(n)))
                    if op == 8: break
                    if op != 1: continue
                    m = json.loads(data)
                    if m.get("type") == "cmd" and m.get("cmd") == "mute":
                        mins = m.get("minutes", 5); muted_until = time.time() + 60 * mins if mins > 0 else 0
                        send({"type": "ack", "id": m.get("id", 0), "ok": True})
                    else: send({"type": "ack", "id": m.get("id", 0), "ok": False, "message": "Password required"})
            except Exception: pass
            alive[0] = False
        threading.Thread(target=reader, daemon=True).start()
        try:
            send({"type": "hello", "board": "local", "fw": "5.0.0-dev", "rev": rev})
            while alive[0]:
                send(state()); time.sleep(1)
        except Exception: pass
        alive[0] = False
        self.close_connection = True

    def do_POST(self):
        global rev, muted_until, clock_set
        p = self.path.split("?")[0]
        raw = self.body()
        j = json.loads(raw) if raw and self.headers.get("Content-Type", "").startswith("application/json") else None
        if p == "/api/alerts/mute":
            muted_until = time.time() + j.get("minutes", 5) * 60 if j.get("minutes", 5) > 0 else 0
            return self.send_json(200, {"ok": True, "message": "Muted"})
        if p == "/api/time":
            clock_set = True
            return self.send_json(200, {"ok": True, "message": "Time set"})
        if p == "/api/v2/command":
            j = (j or {})
            if j.get("cmd") == "mute":
                muted_until = time.time() + 60 * j.get("minutes", 5) if j.get("minutes", 5) > 0 else 0
                return self.send_json(200, {"ok": True, "message": "ok"})
        if p == "/api/v2/wifi/search":
            if not self.authed(): return
            return self.send_json(200, {"ok": True, "message": "Searching for networks"})
        if p == "/api/onewire/scan": return self.send_json(202, {"ok": True, "message": "Scanning"})
        if not self.authed(): return
        with lock:
            if p == "/api/config/sensors":
                ids = [s["id"] for s in j]
                if len(set(ids)) != len(ids): return self.send_json(400, {"ok": False, "message": "Duplicate id"})
                cfg["sensors"] = j; rev += 1
                for s in j: history.setdefault(s["id"], [])
                return self.send_json(200, {"ok": True, "message": "Sensors saved"})
            if p == "/api/config/relays":
                cfg["relays"] = j; rev += 1
                for r in j: relay_state.setdefault(r["id"], dict(on=False, out=False, blinkMs=r["blinkMs"]))
                return self.send_json(200, {"ok": True, "message": "Relays saved"})
            if p == "/api/config/system":
                for k, v in j.items():
                    if k.endswith("Pass") and not v: continue
                    cfg["system"][k] = v
                rev += 1
                return self.send_json(200, {"ok": True, "message": "System settings saved"})
            if p in ("/api/config/turbo", "/api/config/layout"):
                k = p.rsplit("/", 1)[1]; cfg[k] = j; rev += 1
                return self.send_json(200, {"ok": True, "message": ("Turbo timer" if k == "turbo" else "Layout") + " saved"})
            if p == "/api/blackbox/clear":
                return self.send_json(200, {"ok": True, "message": "Recordings deleted"})
            if p in ("/api/config/weather", "/api/config/cloud"):
                k = p.rsplit("/", 1)[1]
                for kk, v in j.items():
                    if kk in ("zbxPsk", "influxAuth") and not v: continue
                    cfg[k][kk] = v
                rev += 1
                return self.send_json(200, {"ok": True, "message": k.title() + " settings saved"})
            if p == "/api/v2/board":
                cfg["system"]["board"] = (j or {}).get("board", "auto"); rev += 1
                return self.send_json(200, {"ok": True, "message": "Board saved"})
            if p == "/api/v2/config/restore":
                return self.send_json(200, {"ok": True, "message": "Restored the previous configuration"})
            if p == "/api/v2/safemode/leave":
                return self.send_json(200, {"ok": True, "message": "Leaving safe mode. Rebooting...", "reboot": True})
            if p == "/api/altitude/calibrate":
                return self.send_json(200, {"ok": True, "message": "Sea-level reference set to 1014.9 hPa"})
            if p in ("/api/cloud/flush", "/api/cloud/clear"):
                return self.send_json(200, {"ok": True, "message": "ok"})
            if p == "/api/relays/control":
                rs = relay_state[j["id"]]
                if "on" in j: rs["on"] = rs["out"] = j["on"]
                if "blinkMs" in j: rs["blinkMs"] = j["blinkMs"]
                return self.send_json(200, {"ok": True, "message": "ok"})
            if p == "/api/update":
                return self.send_json(200, {"ok": True, "message": "Update installed. Rebooting...", "reboot": True})
            if p in ("/api/reboot", "/api/config/reset", "/api/config/import"):
                return self.send_json(200, {"ok": True, "message": "ok"})
        self.send_json(404, {"ok": False, "message": "Not found"})

ThreadingHTTPServer(("127.0.0.1", PORT), H).serve_forever()
