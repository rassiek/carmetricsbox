#!/usr/bin/env python3
"""A pretend main board for testing the web app on a PC: API v1 (docs/API.md) with
in-memory settings, live values that drift, two paired boards, a WebSocket.

usage: python3 mock_server.py <web dir> [port]
"""
import base64, copy, hashlib, json, math, os, random, re, socket, struct, sys, threading, time
from http.server import ThreadingHTTPServer, BaseHTTPRequestHandler
from urllib.parse import urlparse, parse_qs, unquote

WEB = sys.argv[1] if len(sys.argv) > 1 else 'web'
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 8099
ROLE = sys.argv[3] if len(sys.argv) > 3 else 'main'   # main | worker (5.4 worker firmware)
T0 = time.time()
LOCK = threading.RLock()

def S(id, name, type, unit, **k):
    d = dict(id=id, name=name, type=type, unit=unit, enabled=True, decimals=1, gaugeMin=0, gaugeMax=120, smoothing=0.3,
             oledStyle='text', oledLabel='', warnLow=None, warnHigh=None, critLow=None, critHigh=None, hysteresis=1,
             engineRunningOnly=False, faultAlarm=True, pin=-1, sckPin=-1, soPin=-1, adsAddress=72, adsChannel=0, address='')
    d.update(k); return d

CFG = {
    'version': 6,
    'ui': {'appName': 'CarMetrics', 'machineName': 'Defender', 'pages': {'dashboard': 'Dashboard', 'sensors': 'Sensors', 'actuators': 'Actuators', 'boards': 'Boards', 'system': 'System'},
           'runningWord': 'engine', 'units': {'temperature': 'C', 'pressure': '', 'speed': ''}, 'preset': 'car'},
    'sensors': [
        S('coolant_temp', 'Coolant', 'ds18b20', '°C', warnHigh=98, critHigh=105, oledLabel='CLT'),
        S('egt', 'EGT', 'max6675', '°C', gaugeMax=900, warnHigh=650, critHigh=720, pin=5, sckPin=18, soPin=19, decimals=0),
        S('boost_pressure', 'Boost', 'ads1115', 'psi', gaugeMax=30, warnHigh=22, critHigh=26, adsChannel=0),
        S('oil_pressure', 'Oil pressure', 'ads1115', 'psi', gaugeMax=100, warnLow=15, critLow=8, adsChannel=1),
        S('bat_voltage', 'Battery', 'ads1115', 'V', gaugeMin=10, gaugeMax=16, warnLow=12, critLow=11.5, adsChannel=2, divider=5, decimals=2),
        S('cabin_temp', 'Cabin', 'bmp', '°C', gaugeMax=50, bmpField='temperature'),
        S('intake_temp', 'Intake', 'ds18b20', '°C', gaugeMax=80, warnHigh=60, critHigh=70),
        S('altitude', 'Altitude', 'bmp', 'm', gaugeMax=3000, bmpField='altitude', decimals=0),
        dict(S('b1_gearbox_temp', 'Engine bay Gearbox', 'remote', '°C', warnHigh=110), board=1, remoteId='gearbox_temp', remoteIndex=0, mockSt='not_found'),
    ],
    'relays': [
        dict(id='fan', name='Fan', pin=13, type='pwm', mode='on_off', activeLow=False, defaultOn=False, blinkMs=500, enabled=True, safeState='off',
             freqHz=25000, kickMs=500, minDuty=20, maxDuty=100, rampPerS=20, safeDuty=0, rule='threshold', ruleSensor='coolant_temp', onAt=95, offAt=90),
        dict(id='lights', name='Spot lights', pin=26, type='relay', mode='on_off', activeLow=True, defaultOn=False, blinkMs=500, enabled=True, safeState='off', rule='manual'),
        dict(id='horn', name='Horn', pin=21, type='relay', mode='on_off', activeLow=True, defaultOn=False, blinkMs=500, enabled=False, safeState='off', rule='manual'),
    ],
    'system': {'board': 'auto', 'role': 'main', 'boardEncrypt': False, 'canEnabled': False, 'canTx': -1, 'canRx': -1, 'canKbps': 500,
               'apSsid': 'CarMetrics', 'apPass': '', 'networks': [{'ssid': 'Home', 'pass': '', 'passSet': True, 'priority': 5}], 'scanWhileApInUse': True,
               'statusLight': {'builtin': True, 'builtinPin': -1, 'builtinBrightness': 15, 'external': False, 'r': -1, 'g': -1, 'b': -1, 'commonAnode': False, 'externalBrightness': 80},
               'ignitionPin': -1, 'ignitionActiveLow': False, 'sleepAfterMinutes': 0, 'gpsRxPin': -1, 'gpsTxPin': -1, 'gpsBaud': 9600,
               'hostname': 'carmetrics', 'adminPass': '', 'readNeedsAuth': False, 'i2cSda': 8, 'i2cScl': 9, 'i2cKhz': 100, 'oneWirePin': 15, 'oneWireResolution': 11,
               'oledType': 'ssd1327_128x128', 'oledFlip': False, 'oledClock': True, 'oledPageSeconds': 4, 'oledContrast': 200,
               'buzzerPin': 16, 'buzzerActiveLow': True, 'ledPin': 17, 'ledActiveLow': False, 'engineSensorId': 'bat_voltage', 'engineRunningAbove': 13.2,
               'engineDelaySeconds': 5, 'seaLevelHpa': 1013.25, 'historyIntervalSeconds': 5, 'tzOffsetMinutes': 180},
    'weather': {'enabled': True, 'refTown': 'Nairobi', 'autoSeaLevel': True, 'refreshMinutes': 30, 'towns': [{'name': 'Nairobi', 'lat': -1.2864, 'lon': 36.8172}]},
    'cloud': {'enabled': False, 'protocol': 'zabbix', 'intervalMs': 1000, 'uploadEverySec': 60, 'bufferKB': 256, 'onlyEngineRunning': True, 'sendSystem': False,
              'zbxServer': '', 'zbxPort': 10051, 'zbxHost': 'carmetrics', 'zbxPacked': True, 'zbxPskIdentity': '', 'zbxPsk': '', 'zbxPskSet': False,
              'influxUrl': '', 'influxAuth': '', 'influxAuthSet': False, 'influxMeasurement': 'carmetrics'},
    'turbo': {'enabled': True, 'egtId': 'egt', 'safeEgt': 300, 'minIdleSeconds': 30, 'rpmId': '', 'idleRpm': 1000, 'speedId': '', 'beep': True},
    'layout': {'tiles': [], 'oled': [], 'oledColumns': 0},
    'blackbox': {'enabled': True, 'keepEvents': 10},
}
if ROLE == 'worker':   # a pump board on its own: its own sensors and actuators only
    CFG['ui'].update(machineName='Borehole', runningWord='pump', preset='')
    CFG['sensors'] = [S('tank_temp', 'Tank', 'ds18b20', '°C', warnHigh=60, critHigh=70, oledLabel='TNK'),
                      S('motor_temp', 'Motor', 'ds18b20', '°C', warnHigh=80, critHigh=95, mockSt='not_found'),
                      S('line_pressure', 'Line pressure', 'adc', 'bar', gaugeMax=10, warnLow=1, pin=34)]
    CFG['relays'] = [CFG['relays'][1]]
    CFG['system'].update(role='worker', i2cSda=21, i2cScl=22, oledType='none', engineSensorId='')
REV = [1]
CUSTOM = [None]   # worker: custom board definition
CORE_CAPS = ['sensors', 'actuators', 'pwm', 'discover', 'pins', 'keys']
ACT = {'fan': {'value': 0, 'on': False, 'by': 'rule', 'manual': False}, 'lights': {'value': 0, 'on': False, 'by': 'manual', 'manual': False}}
STATE = {'muted': False, 'level': 'ok'}
KEYS = []
BOARDS = [
    {'id': 1, 'name': 'Engine bay', 'mac': 'AA:BB:CC:00:00:01', 'role': 'sensor_relay', 'chip': 'ESP32', 'firmware': '5.3.0-beta.1', 'online': True, 'ageS': 0,
     'rssi': -61, 'rssiAtBoard': -63, 'uptimeS': 5400, 'level': 'ok', 'relaysSafe': False, 'safeMode': False, 'received': 900, 'lost': 3, 'lossPct': 0,
     'commandsIn': 2, 'commandsFailed': 0, 'muted': False, 'encrypted': False, 'sameChip': False,
     'config': {'pending': False, 'status': 'accepted', 'message': '', 'hasShare': True, 'upToDate': True},
     'points': [{'id': 'gearbox_temp', 'name': 'Gearbox', 'unit': '°C', 'relay': False, 'decimals': 1, 'value': 74.2, 'status': 'ok', 'level': 'ok'},
                {'id': 'pump', 'name': 'Pump', 'unit': '', 'relay': True, 'decimals': 0, 'value': 0, 'status': 'ok', 'level': 'ok'}]},
    {'id': 2, 'name': 'Dash display', 'mac': 'AA:BB:CC:00:00:02', 'role': 'display', 'chip': 'ESP32-S3', 'firmware': '5.3.0-beta.1', 'online': True, 'ageS': 0,
     'rssi': -48, 'rssiAtBoard': -50, 'uptimeS': 5000, 'level': 'ok', 'relaysSafe': False, 'safeMode': False, 'received': 800, 'lost': 0, 'lossPct': 0,
     'commandsIn': 0, 'commandsFailed': 0, 'muted': False, 'encrypted': False, 'sameChip': True, 'display': {'catalogSent': True, 'sensors': 7, 'relays': 3},
     'config': {'pending': False, 'status': 'accepted', 'message': '', 'hasShare': False, 'upToDate': True}, 'points': []},
]
SHARES = {1: {'sensors': [S('gearbox_temp', 'Gearbox', 'ds18b20', '°C', warnHigh=110)],
              'relays': [dict(id='pump', name='Pump', pin=26, type='relay', mode='on_off', activeLow=True, enabled=True, rule='manual', safeState='off', blinkMs=500, defaultOn=False)]}}
FOUND = [{'mac': 'AA:BB:CC:00:00:09', 'name': 'Relay X8', 'firmware': '5.3.0-beta.1', 'role': 'relay', 'chip': 'ESP32', 'rssi': -70, 'ageS': 2}]
EVENTS = [{'seq': 3, 'bytes': 4200, 'epoch': int(T0) - 3600, 'uptimeS': 1200, 'reason': 'coolant_temp warn'}]

def bump(): REV[0] += 1

def value(s, t):
    base = {'coolant_temp': 99 + 3 * math.sin(t / 20), 'egt': 420 + 80 * math.sin(t / 15), 'boost_pressure': 12 + 6 * math.sin(t / 7),
            'oil_pressure': 45 + 5 * math.sin(t / 9), 'bat_voltage': 14.1 + 0.1 * math.sin(t / 5), 'cabin_temp': 24.6, 'intake_temp': 38 + 2 * math.sin(t / 25), 'altitude': 1661 + 20 * math.sin(t / 60), 'b1_gearbox_temp': 74 + math.sin(t / 30)}
    return round(base.get(s['id'], 50), 2)

def level(s, v):
    if v is None: return 'ok'
    if (s.get('critHigh') is not None and v >= s['critHigh']) or (s.get('critLow') is not None and v <= s['critLow']): return 'crit'
    if (s.get('warnHigh') is not None and v >= s['warnHigh']) or (s.get('warnLow') is not None and v <= s['warnLow']): return 'warn'
    return 'ok'

def addr(s): return f"b{s['board']}.{s['remoteId']}" if s['type'] == 'remote' else f"local.{s['id']}"

def points(meta=False, only=None):
    t = time.time() - T0
    items, worst = [], 'ok'
    order = {'ok': 0, 'warn': 1, 'fault': 2, 'crit': 3}
    for s in CFG['sensors']:
        v = value(s, t) if s['enabled'] else None
        lv = level(s, v)
        if order[lv] > order[worst]: worst = lv
        x = {'a': addr(s), 'v': v, 'st': 'ok' if s['enabled'] else 'disabled', 'lvl': lv}
        if s.get('mockSt') and s['enabled']: x.update(v=None, st=s['mockSt'], lvl='fault')
        if meta: x.update(kind='sensor', name=s['name'], label=s.get('oledLabel', ''), unit=s['unit'], decimals=s['decimals'], min=s['gaugeMin'], max=s['gaugeMax'],
                          board=f"b{s['board']}" if s['type'] == 'remote' else 'local',
                          limits={'wl': s.get('warnLow'), 'wh': s.get('warnHigh'), 'cl': s.get('critLow'), 'ch': s.get('critHigh')})
        items.append(x)
    for r in CFG['relays']:
        st = ACT.setdefault(r['id'], {'value': 0, 'on': False, 'by': 'manual', 'manual': False})
        x = {'a': 'local.' + r['id'], 'v': st['value'], 'st': 'ok', 'lvl': 'ok'}
        if meta: x.update(kind='actuator', type=r['type'], name=r['name'], board='local', unit='%' if r['type'] != 'relay' else '')
        items.append(x)
    items.append({'a': 'local.sys_heap', 'v': 151.2, 'st': 'ok', 'lvl': 'ok', **({'kind': 'system', 'name': 'heap', 'board': 'local'} if meta else {})})
    if only: items = [i for i in items if i['a'] in only]
    STATE['level'] = worst
    d = {'t': int(t * 1000), 'epoch': int(time.time()), 'level': worst, 'muted': STATE['muted'], 'running': True, 'rev': REV[0], 'items': items,
         'turbo': {'phase': 'cooling', 'leftS': 42} if ROLE == 'main' else None}
    return d

def find(lst, id):
    for i, x in enumerate(lst):
        if x.get('id') == id: return i
    return -1

def board_list(q, kind):
    b = q.get('board', ['local'])[0]
    if b == 'local': return [s for s in CFG[kind] if s.get('type') != 'remote'] if kind == 'sensors' else CFG[kind]
    n = int(b[1:]); return SHARES.setdefault(n, {'sensors': [], 'relays': []})[kind]

def map_limits(item, body):
    for k, v in body.items():
        if k == 'limits':
            for a, b in (('wl', 'warnLow'), ('wh', 'warnHigh'), ('cl', 'critLow'), ('ch', 'critHigh')):
                if a in v: item[b] = v[a]
        else: item[k] = v

def merge(d, s):
    for k, v in s.items():
        if isinstance(v, dict) and isinstance(d.get(k), dict): merge(d[k], v)
        else: d[k] = v

class Err(Exception):
    def __init__(self, code, msg, field=None): self.code, self.msg, self.field = code, msg, field

def ok(msg='Done', **k): return {'ok': True, 'message': msg, **k}

def route(m, path, q, body, scope):
    p = path.split('/') if path else []
    need = lambda s: None if scope >= s else (_ for _ in ()).throw(Err(401, 'password needed'))
    if m == 'GET' and path == 'info':
        return {'appName': CFG['ui']['appName'], 'machineName': CFG['ui']['machineName'], 'firmware': '5.3.0-beta.1', 'build': 'mock', 'variant': 's3-8mb',
                'api': [1], 'protocol': 1, 'role': ROLE, 'board': {'id': 'esp32s3_n16r8', 'name': 'ESP32-S3 N16R8'} if ROLE == 'main' else {'id': 'esp32_devkit', 'name': 'ESP32 DevKit'},
                'chip': 'esp32s3' if ROLE == 'main' else 'esp32',
                'capabilities': CORE_CAPS + (['boards', 'displays', 'history', 'events', 'cloud', 'weather', 'presets'] if ROLE == 'main' else ['standalone', 'customBoard']),
                'uptimeS': int(time.time() - T0), 'epoch': int(time.time()), 'hostname': 'carmetrics', 'ip': '192.168.1.50', 'apIp': '192.168.4.1'}
    if m == 'GET' and path == 'points':
        only = q.get('a', [''])[0]; return points(q.get('meta', ['0'])[0] == '1', only.split(',') if only else None)
    if m == 'GET' and path == 'settings':
        d = copy.deepcopy(CFG); d['rev'] = REV[0]; return d
    if m == 'GET' and p[:1] == ['settings'] and len(p) == 2: return CFG[p[1]]
    if m == 'PATCH' and p[:1] == ['settings'] and len(p) == 2:
        need(3)
        if p[1] == 'sensors': CFG['sensors'] = body
        else:
            body = {k: v for k, v in body.items() if not (k.lower().endswith('pass') or k.endswith('Psk') or k == 'influxAuth') or v}
            merge(CFG[p[1]], body)
        bump(); return ok('Saved')
    if m == 'POST' and path == 'system/time': return ok('Time set')
    if m == 'POST' and path == 'system/identify': need(2); return ok('Flashing for 10 s')
    if m == 'PUT' and path == 'firmware': need(3); return ok('Firmware installed: restarting')
    if m == 'GET' and path == 'alerts': return {'level': STATE['level'], 'muted': STATE['muted'], 'items': []}
    if m == 'POST' and path == 'alerts/mute':
        STATE['muted'] = (body or {}).get('minutes', 15) > 0; return ok('Muted' if STATE['muted'] else 'Unmuted')
    if p[:1] in (['sensors'], ['actuators']):
        kind = 'sensors' if p[0] == 'sensors' else 'relays'
        if m == 'GET' and path == 'sensors/discover':
            return {'running': False, 'ageMs': 500, 'i2c': [{'addr': '0x48', 'driver': 'ads1115'}, {'addr': '0x76', 'driver': 'bme280'}],
                    'onewire': {'devices': [{'address': '28FF641E8016034A', 'tempC': 21.5, 'assignedTo': ''}]},
                    'analog': [{'source': 'ads1115@0x48/0', 'volts': 1.82, 'assignedTo': 'boost_pressure'}, {'source': 'ads1115@0x48/3', 'volts': 0.01, 'assignedTo': ''}, {'source': 'gpio4', 'volts': 0.4, 'assignedTo': ''}]}
        if m == 'POST' and path == 'sensors/discover': need(3); return ok('Searching')
        lst = board_list(q, kind)
        if m == 'GET' and len(p) == 1:
            items = copy.deepcopy(lst)
            if kind == 'relays' and q.get('board', ['local'])[0] == 'local':
                for a in items: a['state'] = ACT.get(a['id'], {})
            return {'board': q.get('board', ['local'])[0], 'items': items}
        if m == 'POST' and len(p) == 1:
            need(3)
            id = body.get('id') or re.sub(r'[^a-z0-9]+', '_', body.get('name', 'x').lower()).strip('_') or 'x'
            if find(lst, id) >= 0: raise Err(409, f"'{id}' already exists", 'id')
            item = {}; map_limits(item, body); item['id'] = id
            if kind == 'relays' and item.get('pin', -1) < 0 and item.get('enabled', True): raise Err(400, f"Actuator '{id}': choose its pin", 'pin')
            (CFG[kind] if q.get('board', ['local'])[0] == 'local' else lst).append(item); bump(); return ok(f"'{id}' added")
        i = find(lst, p[1]) if len(p) > 1 else -1
        if i < 0: raise Err(404, f"No '{p[1] if len(p) > 1 else ''}'")
        if m == 'GET' and len(p) == 2: return lst[i]
        if m == 'PATCH' and len(p) == 2: need(2); map_limits(lst[i], body); bump(); return ok('Saved')
        if m == 'DELETE' and len(p) == 2: need(3); (CFG[kind] if q.get('board', ['local'])[0] == 'local' else lst).remove(lst[i]); bump(); return ok('Removed')
        if m == 'POST' and len(p) == 3 and p[2] == 'command':
            need(2); st = ACT.setdefault(p[1], {})
            r = lst[i]
            if body.get('auto'): st.update(manual=False, by='rule')
            elif 'on' in body: st.update(on=body['on'], value=(100 if r['type'] != 'relay' else 1) if body['on'] else 0, manual=True, by='manual')
            elif 'duty' in body: st.update(value=body['duty'], on=body['duty'] > 0, manual=True, by='manual')
            elif 'angle' in body: st.update(value=body['angle'], manual=True, by='manual')
            elif 'direction' in body: st.update(value={'forward': 1, 'reverse': -1}.get(body['direction'], 0) * body.get('speed', 100), manual=True, by='manual')
            else: raise Err(400, 'Nothing to do')
            return ok('Done')
    if ROLE == 'worker' and p[:1] in (['boards'], ['events'], ['history'], ['cloud'], ['weather'], ['presets']): raise Err(404, 'Not on a worker')
    if ROLE == 'worker' and m == 'GET' and path == 'node':
        return {'role': 'worker', 'link': {'type': 'espnow', 'ok': True, 'channel': 6, 'rx': 50, 'tx': 52, 'txFail': 0, 'queueDrops': 0}, 'firmware': '5.4.0-beta.1', 'chip': 'ESP32',
                'node': {'paired': True, 'id': 3, 'linkUp': True, 'mainInRange': True, 'configRev': 7, 'encrypted': True}}
    if ROLE == 'worker' and m == 'POST' and path == 'node/unpair': need(3); return ok('Forgot the main board: restarting')
    if m == 'GET' and path == 'boards':
        return {'role': 'main', 'link': {'type': 'espnow', 'channel': 6, 'ok': True, 'rx': 1200, 'tx': 1300, 'txFail': 2, 'queueDrops': 0}, 'found': FOUND, 'items': BOARDS}
    if p[:1] == ['boards'] and m != 'GET':
        need(3)
        if m == 'POST' and len(p) == 1:
            n = max(b['id'] for b in BOARDS) + 1
            BOARDS.append(dict(copy.deepcopy(BOARDS[0]), id=n, name=body['name'], mac=body['mac'], points=[])); return ok('Paired', id=n)
        if path == 'boards/firmware-file': return ok('Firmware file stored on this board')
        b = next((x for x in BOARDS if str(x['id']) == p[1]), None)
        if not b: raise Err(404, 'Unknown board')
        if m == 'PATCH': b['name'] = body['name']; return ok('Renamed')
        if m == 'DELETE': BOARDS.remove(b); return ok('Board forgotten')
        if p[2:] == ['command']: return ok('Sent')
        if p[2:] == ['firmware']: b['fw'] = {'state': 'sending', 'version': '5.3.0-beta.1', 'error': '', 'percent': 35, 'seconds': 12}; return ok('Sending the firmware')
    if m == 'GET' and path == 'system':
        return {'firmware': {'version': '5.3.0-beta.1', 'build': 'mock', 'core': '3.3.12'}, 'chip': {'model': 'ESP32-S3', 'flashMB': 16, 'psramKB': 8192},
                'board': {'id': 'esp32s3_n16r8', 'name': 'ESP32-S3 N16R8', 'setting': 'auto'}, 'uptime': {'s': int(time.time() - T0) + 5400, 'resetReason': 'power on'},
                'memory': {'free': 160000, 'min': 120000, 'largest': 90000, 'psramFree': 7900000}, 'cpu': [12, 30], 'loop': {'avgUs': 900, 'maxUs': 12000},
                'storage': {'fsUsed': 300000, 'fsTotal': 1500000}, 'clients': {'websocket': 1, 'events': 0}, 'tasks': [{'name': 'loop', 'stackFree': 3500}], 'safeMode': False}
    if m == 'GET' and path == 'network':
        return {'state': 'joined', 'ssid': 'Home', 'ip': '192.168.1.50', 'rssi': -58, 'apClients': 1, 'scanAgeS': 20,
                'scan': [{'ssid': 'Home', 'rssi': -58, 'ch': 6, 'security': True, 'saved': True}, {'ssid': 'Phone hotspot', 'rssi': -66, 'ch': 1, 'security': True, 'saved': False}], 'log': []}
    if m == 'POST' and path in ('network/scan', 'network/join', 'system/reboot', 'settings/reset', 'settings/restore', 'backup', 'presets/apply', 'hardware/profile',
                                'hardware/status-light/test', 'hardware/altitude', 'cloud/flush', 'weather/refresh'):
        need(3)
        if path == 'network/join': CFG['system']['networks'].insert(0, {'ssid': body['ssid'], 'pass': '', 'passSet': True, 'priority': 9})
        if path == 'presets/apply':
            merge(CFG, json.load(open(os.path.join(WEB, 'presets', body['preset'] + '.json')))); CFG['ui']['preset'] = body['preset']; bump()
        return ok('Done')
    if m == 'DELETE' and p[:2] == ['network', 'saved']:
        CFG['system']['networks'] = [n for n in CFG['system']['networks'] if n['ssid'] != unquote(p[2])]; return ok('Forgotten')
    if m == 'GET' and path == 'presets': return {'items': ['blank', 'boat', 'car', 'farm'], 'current': CFG['ui']['preset']}
    if m == 'GET' and path == 'pins':
        used = {8: 'I2C SDA', 9: 'I2C SCL', 15: '1-Wire', 16: 'buzzer', 17: 'warning LED', 13: "actuator 'fan'", 26: "actuator 'lights'", 48: 'built-in RGB light'}
        pins = [{'gpio': g, 'abilities': ['output'] + (['analog'] if 1 <= g <= 10 else []) + (['boot_pin'] if g in (0, 3, 45, 46, 48) else []), **({'usedBy': used[g]} if g in used else {})}
                for g in list(range(0, 22)) + list(range(38, 49))]
        return {'board': 'esp32s3_n16r8', 'boardName': 'ESP32-S3 N16R8', 'chip': 'esp32s3', 'notes': '', 'pins': pins, 'warnings': []}
    if m == 'POST' and re.fullmatch(r'pins/\d+/test', path): return {'gpio': int(p[1]), 'level': 1, 'volts': 3.3} if body.get('mode') == 'read' else {'gpio': int(p[1]), 'seconds': 5}
    if m == 'GET' and path == 'hardware/profiles':
        if ROLE == 'worker':
            dev = {'id': 'esp32_devkit', 'name': 'ESP32 DevKit (WROOM-32, 4 MB)', 'chip': 'esp32', 'flashMB': 4, 'psramMB': 0, 'notes': 'GPIO34-39 are input only.',
                   'valid': [0, 1, 2, 3, 4, 5] + list(range(12, 20)) + [21, 22, 23, 25, 26, 27] + list(range(32, 40)), 'output': [1, 2, 3, 4, 5, 13, 14, 15, 16, 17, 18, 19, 21, 22, 23, 25, 26, 27, 32, 33],
                   'adc1': list(range(32, 40)), 'caution': [1, 2, 3, 5, 15], 'rtc': [0, 2, 4, 12, 13, 14, 15, 25, 26, 27] + list(range(32, 40)),
                   'defaults': {'i2cSda': 21, 'i2cScl': 22, 'oneWirePin': 4, 'buzzerPin': 18, 'ledPin': 19}, 'fixed': {'relays': [], 'relaysActiveLow': False, 'inputs': [], 'inputsActiveLow': True}}
            items = [dev] + ([CUSTOM[0]] if CUSTOM[0] else [])
            cur = CUSTOM[0] if CFG['system']['board'] == 'custom' and CUSTOM[0] else dev
            return {'chip': 'esp32', 'setting': CFG['system']['board'], 'detected': 'esp32_devkit', 'current': cur, 'items': items}
        prof = {'id': 'esp32s3_n16r8', 'name': 'ESP32-S3 N16R8 (16 MB flash, 8 MB PSRAM)', 'notes': 'Same pins as N8R8.'}
        return {'chip': 'esp32s3', 'setting': 'auto', 'detected': 'esp32s3_n16r8', 'current': prof, 'items': [prof, {'id': 'esp32s3_n8r8', 'name': 'ESP32-S3 DevKitC-1 N8R8', 'notes': ''}]}
    if path == 'hardware/custom-board':
        if ROLE != 'worker': raise Err(400 if m != 'GET' else 404, 'Custom boards are for worker firmware')
        if m == 'GET':
            if not CUSTOM[0]: raise Err(404, 'No custom board defined')
            return CUSTOM[0]
        need(3)
        if m == 'DELETE':
            if CFG['system']['board'] == 'custom': raise Err(400, 'The board in use: choose another board type first')
            CUSTOM[0] = None; return ok('Board definition removed')
        if not (body.get('name') or '').strip(): raise Err(400, 'name: 1-60 characters')
        rng = lambda t: [] if not t else [i for x in str(t).split(',') if x.strip() for i in (range(int(x.split('-')[0]), int(x.split('-')[1]) + 1) if '-' in x else [int(x)])]
        bad = [g for g in rng(body.get('valid')) if 6 <= g <= 11]
        if bad: raise Err(400, f"valid: GPIO {bad[0]} can't be used on esp32 (flash, PSRAM or missing)")
        CUSTOM[0] = dict(body, id='custom', chip='esp32', valid=rng(body.get('valid')), output=rng(body.get('output')), adc1=rng(body.get('adc1')), rtc=rng(body.get('rtc')), caution=rng(body.get('caution')))
        return ok('Board definition saved: choose "custom" as the board type to use it')
    if m == 'GET' and path == 'keys': need(3); return {'items': KEYS}
    if m == 'POST' and path == 'keys':
        need(3); KEYS.append({'id': '%04x' % random.randint(0, 65535), 'name': body['name'], 'scope': body['scope']}); return ok('Copy the key now', key='cm_' + '%032x' % random.getrandbits(128))
    if m == 'DELETE' and p[:1] == ['keys']: KEYS[:] = [k for k in KEYS if k['id'] != p[1]]; return ok('Key deleted')
    if m == 'GET' and path == 'weather':
        return {'enabled': True, 'towns': [{'name': 'Nairobi', 'temp': 23.4, 'code': 2, 'wind': 11, 'rh': 55}]}
    if m == 'GET' and path == 'cloud': return {'recording': False, 'backlogFiles': 0}
    if m == 'GET' and path == 'events': return {'enabled': True, 'keep': 10, 'items': EVENTS}
    if m == 'GET' and p[:1] == ['events'] and len(p) == 2:
        rows = [[98 + i / 10, 400 + i] for i in range(60)]; return {'seq': 3, 'reason': 'coolant_temp warn', 'interval': 1, 'before': 30, 'ids': ['coolant_temp', 'egt'], 'rows': rows}
    if m == 'GET' and path == 'history':
        n = 120
        ser = {s['id']: [round(value(s, i * 5), 1) for i in range(n)] for s in CFG['sensors']}
        for r in CFG['relays']: ser[r['id']] = [(40 + 20 * math.sin(i / 10)) if r['type'] != 'relay' else (1 if i % 40 < 20 else 0) for i in range(n)]
        for k, base in (('sys_heap_free_kb', 150), ('sys_heap_min_kb', 120), ('sys_cpu0', 12), ('sys_cpu1', 30), ('sys_loop_max_ms', 9), ('sys_rssi', -60)):
            ser[k] = [round(base + 3 * math.sin(i / 7), 1) for i in range(n)]
        return {'interval': 5, 'len': n, 'count': n, 'ageMs': 1000, 'series': ser}
    if m == 'GET' and path == 'backup': need(3); return CFG
    raise Err(404, 'No such address: ' + path)

class H(BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'
    def log_message(self, *a): pass
    def scope(self):
        a = self.headers.get('Authorization', '')
        if a.startswith('Basic ') and base64.b64decode(a[6:]).decode() == 'admin:carmetrics': return 3
        return 1
    def send(self, code, obj, ctype='application/json', extra=None):
        b = obj if isinstance(obj, bytes) else json.dumps(obj).encode()
        self.send_response(code); self.send_header('Content-Type', ctype); self.send_header('Content-Length', str(len(b)))
        for k, v in (extra or {}).items(): self.send_header(k, v)
        self.end_headers(); self.wfile.write(b)
    def api(self, m):
        u = urlparse(self.path)
        path = u.path[len('/api/v1'):].strip('/')
        n = int(self.headers.get('Content-Length') or 0)
        raw = self.rfile.read(n) if n else b''
        body = None
        if raw and self.headers.get('Content-Type', '').startswith('application/json'):
            try: body = json.loads(raw)
            except Exception: return self.send(400, {'error': {'code': 'invalid', 'message': 'Body is not valid JSON'}})
        try:
            with LOCK: r = route(m, path, parse_qs(u.query), body if body is not None else {}, self.scope())
            self.send(200, r)
        except Err as e:
            if e.code == 401 and not self.headers.get('Authorization'):
                return self.send(401, {'error': {'code': 'auth', 'message': e.msg}}, extra={'WWW-Authenticate': 'Basic realm="CarMetrics"'})
            err = {'code': {400: 'invalid', 401: 'auth', 404: 'not_found', 409: 'conflict'}.get(e.code, 'invalid'), 'message': e.msg}
            if e.field: err['field'] = e.field
            self.send(e.code, {'error': err})
    def do_GET(self):
        if self.path.startswith('/api/v1/ws'): return self.websocket()
        if self.path.startswith('/api/'): return self.api('GET')
        f = urlparse(self.path).path.lstrip('/') or 'index.html'
        fp = os.path.join(WEB, f)
        if not os.path.isfile(fp): return self.send(404, b'not found', 'text/plain')
        ct = {'.html': 'text/html', '.js': 'application/javascript', '.css': 'text/css', '.png': 'image/png', '.json': 'application/json'}.get(os.path.splitext(f)[1], 'application/octet-stream')
        self.send(200, open(fp, 'rb').read(), ct)
    def do_POST(self): self.api('POST')
    def do_PATCH(self): self.api('PATCH')
    def do_DELETE(self): self.api('DELETE')
    def do_PUT(self): self.api('PUT')

    # minimal WebSocket: hello, then points every second
    def websocket(self):
        key = self.headers.get('Sec-WebSocket-Key', '')
        acc = base64.b64encode(hashlib.sha1((key + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').encode()).digest()).decode()
        self.send_response(101); self.send_header('Upgrade', 'websocket'); self.send_header('Connection', 'Upgrade'); self.send_header('Sec-WebSocket-Accept', acc); self.end_headers()
        def frame(s):
            b = s.encode(); hdr = bytes([0x81]) + (bytes([len(b)]) if len(b) < 126 else bytes([126]) + struct.pack('>H', len(b)) if len(b) < 65536 else bytes([127]) + struct.pack('>Q', len(b)))
            self.wfile.write(hdr + b); self.wfile.flush()
        try:
            frame(json.dumps({'type': 'hello', 'api': 1, 'firmware': '5.3.0-beta.1', 'rev': REV[0]}))
            while True:
                with LOCK: p = points()
                frame(json.dumps({'type': 'points', 't': p['t'], 'level': p['level'], 'muted': p['muted'], 'running': True, 'rev': p['rev'], 'turbo': p['turbo'],
                                  'v': {i['a']: [i['v'], i['st'], i['lvl']] for i in p['items']}}))
                time.sleep(1)
        except Exception: pass

if __name__ == '__main__':
    ThreadingHTTPServer.daemon_threads = True
    print(f'mock board on http://127.0.0.1:{PORT}/', flush=True)
    ThreadingHTTPServer(('127.0.0.1', PORT), H).serve_forever()
