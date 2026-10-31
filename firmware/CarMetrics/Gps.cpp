#include "Gps.h"
#include "State.h"
#include <sys/time.h>

namespace {
HardwareSerial& port = Serial2;
bool portOpen = false;
int openRx = -2, openTx = -2; uint32_t openBaud = 0;
char line[100]; uint8_t len = 0;

// Split a sentence (without '$' and checksum) into fields, in place.
int split(char* s, char** f, int max) {
  int n = 0;
  f[n++] = s;
  for (char* p = s; *p && n < max; p++) if (*p == ',') { *p = 0; f[n++] = p + 1; }
  return n;
}

double coord(const char* v, const char* hemi) {   // ddmm.mmmm -> degrees
  if (!*v) return NAN;
  double x = atof(v);
  int deg = (int)(x / 100);
  double d = deg + (x - deg * 100) / 60.0;
  return (*hemi == 'S' || *hemi == 'W') ? -d : d;
}

void setClockFromRmc(const char* t, const char* d) {   // hhmmss.ss, ddmmyy (UTC)
  if (strlen(t) < 6 || strlen(d) != 6) return;
  struct tm tm = {};
  tm.tm_hour = (t[0] - '0') * 10 + (t[1] - '0'); tm.tm_min = (t[2] - '0') * 10 + (t[3] - '0'); tm.tm_sec = (t[4] - '0') * 10 + (t[5] - '0');
  tm.tm_mday = (d[0] - '0') * 10 + (d[1] - '0'); tm.tm_mon = (d[2] - '0') * 10 + (d[3] - '0') - 1; tm.tm_year = 100 + (d[4] - '0') * 10 + (d[5] - '0');
  // mktime() assumes local time; the ESP32 runs with TZ unset (UTC), so this is UTC.
  time_t epoch = mktime(&tm);
  if (epoch < 1700000000) return;
  struct timeval tv = {epoch, 0};
  settimeofday(&tv, nullptr);
  Serial.println("[gps] clock set from GPS");
}

bool parse(char* s) {
  // checksum
  char* star = strchr(s, '*');
  if (!star || s[0] != '$') return false;
  uint8_t sum = 0;
  for (char* p = s + 1; p < star; p++) sum ^= (uint8_t)*p;
  if (strtol(star + 1, nullptr, 16) != sum) return false;
  *star = 0;
  char* f[20];
  int n = split(s + 1, f, 20);
  if (strlen(f[0]) != 5) return false;
  const char* type = f[0] + 2;   // GP / GN / GL / GA / BD talker
  uint32_t now = millis();
  StateLock lk;
  GpsRuntime& g = gSys.gps;
  g.lastSentenceMs = now; g.fitted = true;
  if (!strcmp(type, "RMC") && n >= 10) {
    bool valid = f[2][0] == 'A';
    g.fix = valid;
    if (valid) {
      g.lat = coord(f[3], f[4]); g.lon = coord(f[5], f[6]);
      g.speedKmh = *f[7] ? atof(f[7]) * 1.852f : NAN;
      g.heading = *f[8] ? atof(f[8]) : NAN;
      g.lastFixMs = now;
      if (!gSys.timeSynced) { setClockFromRmc(f[1], f[9]); gSys.timeSynced = true; gSys.timeSource = "GPS"; }
    }
    return true;
  }
  if (!strcmp(type, "GGA") && n >= 10) {
    int q = atoi(f[6]);
    g.sats = atoi(f[7]);
    g.hdop = *f[8] ? atof(f[8]) : NAN;
    if (q > 0) { g.altM = *f[9] ? atof(f[9]) : NAN; g.lastFixMs = now; }
    else g.fix = false;
    return true;
  }
  return true;   // other sentences: still proves a module is there
}
}  // namespace

void Gps::begin() {
  const SystemConfig& s = gConfig.sys;
  if (s.gpsRxPin == openRx && s.gpsTxPin == openTx && s.gpsBaud == openBaud) return;
  if (portOpen) { port.end(); portOpen = false; }
  openRx = s.gpsRxPin; openTx = s.gpsTxPin; openBaud = s.gpsBaud;
  { StateLock lk; gSys.gps = GpsRuntime(); }
  if (s.gpsRxPin < 0) return;
  port.begin(s.gpsBaud, SERIAL_8N1, s.gpsRxPin, s.gpsTxPin);
  portOpen = true;
  Serial.printf("[gps] listening on RX %d at %u baud\n", s.gpsRxPin, (unsigned)s.gpsBaud);
}

void Gps::update() {
  if (portOpen) {
    int budget = 256;   // bytes per loop pass: a 9600 baud module sends ~1 KB/s
    while (port.available() && budget--) {
      char c = port.read();
      if (c == '\n' || c == '\r') {
        if (len) { line[len] = 0; parse(line); len = 0; }
      } else if (len < sizeof(line) - 1) line[len++] = c;
      else len = 0;   // garbage: resync on the next newline
    }
  }
  uint32_t now = millis();
  StateLock lk;
  GpsRuntime& g = gSys.gps;
  if (g.fitted && now - g.lastSentenceMs > 5000) { g.fitted = false; g.fix = false; }
  if (g.fix && now - g.lastFixMs > 5000) g.fix = false;
}

#ifdef CM_EMU_SELFTEST
bool Gps::selftestParse(const char* l) { strncpy(line, l, sizeof(line) - 1); line[sizeof(line) - 1] = 0; return parse(line); }
#endif
