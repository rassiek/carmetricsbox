#include "Health.h"
#include "State.h"
#include "Board.h"
#include <Preferences.h>
#include <WiFi.h>
#include <LittleFS.h>
#include <esp_system.h>
#include <esp_ota_ops.h>
#include <esp_freertos_hooks.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <esp_arduino_version.h>

// The Arduino core marks a new firmware valid at boot unless this returns true;
// we confirm it ourselves once the firmware has run for HEALTHY_AFTER_S.
extern "C" bool verifyRollbackLater() { return true; }

namespace {
// Survives software/watchdog resets, lost on power-off.
RTC_NOINIT_ATTR uint32_t rtcMagic;
RTC_NOINIT_ATTR uint32_t rtcCrashes;
const uint32_t MAGIC = 0xC0DE5AFE;

bool safe = false;
String safeReason;
uint8_t crashes = 0;
int resetReason = 0;
uint32_t bootCount = 0;
uint8_t resetHist[10];
uint8_t resetHistLen = 0;
bool healthyDone = false;
uint32_t configAppliedMs = 0;
bool configGoodPending = true;
String otaNote;

// --- CPU load per core, by sampling: on every 1 ms tick, was the core running
// its idle task? (No FreeRTOS run-time stats in the Arduino build.) Short bursts
// that start and finish between two ticks are partly missed, so light loads read low.
volatile TaskHandle_t idleTask[2] = {nullptr, nullptr};
volatile uint32_t ticks[2] = {0, 0}, idleTicks[2] = {0, 0};
uint32_t sampleTicks[2] = {0, 0}, sampleIdle[2] = {0, 0}, sampleAtMs = 0;
float load[2] = {NAN, NAN};
// Hooks may run while the flash cache is off (during flash writes), so they live in IRAM.
static void IRAM_ATTR tickCount(int core) {
  ticks[core]++;
  if (xTaskGetCurrentTaskHandle() == idleTask[core]) idleTicks[core]++;
}
bool IRAM_ATTR idleHook0() { idleTask[0] = xTaskGetCurrentTaskHandle(); return true; }
bool IRAM_ATTR idleHook1() { idleTask[1] = xTaskGetCurrentTaskHandle(); return true; }
void IRAM_ATTR tickHook0() { tickCount(0); }
void IRAM_ATTR tickHook1() { tickCount(1); }

// --- loop timing
uint32_t loopStartUs = 0, loopMaxUs = 0, loopMaxShown = 0, loopWindowStart = 0;
uint64_t loopSumUs = 0; uint32_t loopN = 0, loopAvgShown = 0;

// --- tasks
struct TaskInfo { const char* name; TaskHandle_t h; };
TaskInfo tasks[8]; uint8_t taskCount = 0;
portMUX_TYPE taskMux = portMUX_INITIALIZER_UNLOCKED;

uint32_t flashWrites = 0, flashBytes = 0, sseClients = 0, wsClients = 0;

bool isCrash(int r) { return r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT || r == ESP_RST_WDT; }
}  // namespace

const char* Health::resetReasonStr(int r) {
  switch (r) {
    case ESP_RST_POWERON:   return "power on";
    case ESP_RST_EXT:       return "reset pin";
    case ESP_RST_SW:        return "restart";
    case ESP_RST_PANIC:     return "crash";
    case ESP_RST_INT_WDT:   return "watchdog (interrupt)";
    case ESP_RST_TASK_WDT:  return "watchdog (task)";
    case ESP_RST_WDT:       return "watchdog";
    case ESP_RST_DEEPSLEEP: return "wake from sleep";
    case ESP_RST_BROWNOUT:  return "low voltage (brown-out)";
    case ESP_RST_SDIO:      return "SDIO";
    default:                return "unknown";
  }
}

void Health::earlyBoot() {
#ifdef CM_EMULATOR_TEST
  // QEMU's emulated flash takes seconds per write, which starves IDLE0 and trips
  // the task watchdog. Real flash writes take milliseconds.
  disableCore0WDT();
#endif
  resetReason = (int)esp_reset_reason();
  if (rtcMagic != MAGIC || resetReason == ESP_RST_POWERON) { rtcMagic = MAGIC; rtcCrashes = 0; }
  if (isCrash(resetReason)) rtcCrashes++;
  else if (resetReason != ESP_RST_BROWNOUT) rtcCrashes = 0;   // cranking dips neither count nor clear
  crashes = rtcCrashes > 255 ? 255 : rtcCrashes;
#ifdef CM_FORCE_SAFE_MODE
  crashes = SAFE_MODE_CRASHES;
#endif
  if (crashes >= SAFE_MODE_CRASHES) {
    safe = true;
    safeReason = String(crashes) + " crash resets in a row (last: " + resetReasonStr(resetReason) + ")";
  }

  // Reset-reason history + boot counter (one small NVS write per boot)
  Preferences p;
  if (p.begin("health", false, "cfg")) {
    bootCount = p.getUInt("boots", 0) + 1;
    p.putUInt("boots", bootCount);
    resetHistLen = p.getBytes("rr", resetHist, sizeof(resetHist));
    if (resetHistLen > sizeof(resetHist)) resetHistLen = 0;
    memmove(resetHist + 1, resetHist, sizeof(resetHist) - 1);
    resetHist[0] = (uint8_t)resetReason;
    if (resetHistLen < sizeof(resetHist)) resetHistLen++;
    p.putBytes("rr", resetHist, resetHistLen);
    p.end();
  }

  // Firmware slot / rollback information
  const esp_partition_t* bad = esp_ota_get_last_invalid_partition();
  if (bad) {
    esp_app_desc_t d;
    if (esp_ota_get_partition_description(bad, &d) == ESP_OK)
      otaNote = String("A firmware update (") + d.version + ", slot " + bad->label + ") did not start properly and was rolled back.";
  }

  esp_register_freertos_idle_hook_for_cpu(idleHook0, 0);
  esp_register_freertos_tick_hook_for_cpu(tickHook0, 0);
#if !CONFIG_FREERTOS_UNICORE
  esp_register_freertos_idle_hook_for_cpu(idleHook1, 1);
  esp_register_freertos_tick_hook_for_cpu(tickHook1, 1);
#endif
  configAppliedMs = millis();
  if (safe) Serial.printf("[safe] SAFE MODE: %s\n", safeReason.c_str());
  Serial.printf("[boot] #%u, reset: %s, crash count %u\n", (unsigned)bootCount, resetReasonStr(resetReason), crashes);
}

bool Health::safeMode() { return safe; }
String Health::safeModeReason() { return safeReason; }
uint8_t Health::crashCount() { return crashes; }
void Health::leaveSafeMode() { rtcCrashes = 0; }
void Health::configApplied() { configAppliedMs = millis(); configGoodPending = true; }

void Health::registerTask(const char* name) {
  portENTER_CRITICAL(&taskMux);
  if (taskCount < 8) tasks[taskCount++] = {name, xTaskGetCurrentTaskHandle()};
  portEXIT_CRITICAL(&taskMux);
}
void Health::addFlashWrite(uint32_t bytes) { flashWrites++; flashBytes += bytes; }
void Health::setClientCounts(uint32_t sse, uint32_t ws) { sseClients = sse; wsClients = ws; }

void Health::loopStart() { loopStartUs = micros(); }

void Health::loopEnd() {
  uint32_t us = micros() - loopStartUs;
  if (us > loopMaxUs) loopMaxUs = us;
  loopSumUs += us; loopN++;
  uint32_t now = millis();
  if (now - loopWindowStart >= 10000) {   // 10 s windows
    loopWindowStart = now;
    loopMaxShown = loopMaxUs; loopAvgShown = loopN ? loopSumUs / loopN : 0;
    loopMaxUs = 0; loopSumUs = 0; loopN = 0;
  }
  // CPU load every 2 s
  if (now - sampleAtMs >= 2000) {
    sampleAtMs = now;
    for (int c = 0; c < 2; c++) {
      uint32_t t = ticks[c], i = idleTicks[c];
      uint32_t dt = t - sampleTicks[c], di = i - sampleIdle[c];
      load[c] = dt ? 100.0f - di * 100.0f / dt : NAN;
      sampleTicks[c] = t; sampleIdle[c] = i;
    }
  }
  // Healthy run: confirm the firmware and clear the crash counter
  if (!healthyDone && now > HEALTHY_AFTER_S * 1000UL) {
    healthyDone = true;
    rtcCrashes = 0;
#ifdef CM_EMULATOR_TEST   // QEMU's flash is so slow that erasing otadata trips the task watchdog
    esp_err_t e = ESP_OK;
#else
    esp_err_t e = esp_ota_mark_app_valid_cancel_rollback();
#endif
    Serial.printf("[health] running well for %d s: firmware confirmed (%s)\n", HEALTHY_AFTER_S, esp_err_to_name(e));
  }
  // A config that ran this long without trouble becomes the safe-mode fallback
  if (!safe && configGoodPending && now - configAppliedMs > CONFIG_GOOD_AFTER_S * 1000UL) {
    configGoodPending = false;
    markConfigGood();
  }
}

float Health::cpuLoad(int core) { return core >= 0 && core < 2 ? load[core] : NAN; }

const char* const Health::kSysIds[] = {"sys_heap_free_kb", "sys_heap_min_kb", "sys_cpu0", "sys_cpu1", "sys_loop_max_ms", "sys_rssi"};
const uint8_t Health::kSysCount = 6;

float Health::sysMetric(uint8_t i) {
  switch (i) {
    case 0: return ESP.getFreeHeap() / 1024.0f;
    case 1: return ESP.getMinFreeHeap() / 1024.0f;
    case 2: return load[0];
    case 3: return load[1];
    case 4: return loopMaxShown / 1000.0f;
    case 5: return WiFi.status() == WL_CONNECTED ? (float)WiFi.RSSI() : NAN;
    default: return NAN;
  }
}

static void putF(JsonObject o, const char* k, float v, int dec = 1) { if (isnan(v)) o[k] = nullptr; else o[k] = serialized(String(v, dec)); }

void Health::toJson(JsonObject o) {
  const BoardProfile& b = Board::current();
  JsonObject chip = o["chip"].to<JsonObject>();
  chip["model"] = ESP.getChipModel(); chip["revision"] = ESP.getChipRevision();
  chip["cores"] = ESP.getChipCores(); chip["mhz"] = ESP.getCpuFreqMHz();
  chip["flashMB"] = ESP.getFlashChipSize() / (1024 * 1024);
  chip["psramKB"] = ESP.getPsramSize() / 1024;
  chip["target"] = Board::chip();
  putF(chip, "tempC", temperatureRead());
  JsonObject bo = o["board"].to<JsonObject>();
  bo["id"] = b.id; bo["name"] = b.name; bo["setting"] = gConfig.sys.board;

  JsonObject fw = o["firmware"].to<JsonObject>();
  fw["version"] = FW_VERSION; fw["build"] = __DATE__ " " __TIME__;
  fw["core"] = String(ESP_ARDUINO_VERSION_MAJOR) + "." + ESP_ARDUINO_VERSION_MINOR + "." + ESP_ARDUINO_VERSION_PATCH;
  fw["idf"] = ESP.getSdkVersion();
  const esp_partition_t* run = esp_ota_get_running_partition();
  fw["slot"] = run ? run->label : "?";
  esp_ota_img_states_t st;
  if (run && esp_ota_get_state_partition(run, &st) == ESP_OK)
    fw["state"] = st == ESP_OTA_IMG_PENDING_VERIFY ? "on trial (confirmed after " + String(HEALTHY_AFTER_S) + " s)" : st == ESP_OTA_IMG_VALID ? "confirmed" : "other";
  else fw["state"] = "factory";
  fw["rollbackAvailable"] = esp_ota_check_rollback_is_possible();
  if (otaNote.length()) fw["note"] = otaNote;
  fw["sketchKB"] = ESP.getSketchSize() / 1024; fw["slotKB"] = ESP.getFreeSketchSpace() / 1024;   // both OTA slots are the same size

  JsonObject up = o["uptime"].to<JsonObject>();
  up["s"] = millis() / 1000; up["boots"] = bootCount;
  up["resetReason"] = resetReasonStr(resetReason);
  JsonArray rh = up["history"].to<JsonArray>();
  for (uint8_t i = 0; i < resetHistLen; i++) rh.add(resetReasonStr(resetHist[i]));
  o["safeMode"] = safe; if (safe) o["safeReason"] = safeReason;
  o["crashCount"] = crashes;

  JsonObject m = o["memory"].to<JsonObject>();
  m["free"] = ESP.getFreeHeap(); m["min"] = ESP.getMinFreeHeap();
  m["largest"] = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  m["psramFree"] = ESP.getFreePsram();
#ifndef CM_EMULATOR_TEST   // walking the filesystem with QEMU's slow flash trips the interrupt watchdog
  m["fsUsed"] = LittleFS.usedBytes(); m["fsTotal"] = LittleFS.totalBytes();
#endif

  JsonArray cpu = o["cpu"].to<JsonArray>();
  for (int c = 0; c < ESP.getChipCores() && c < 2; c++) { if (isnan(load[c])) cpu.add(nullptr); else cpu.add(serialized(String(load[c], 0))); }
  JsonObject lp = o["loop"].to<JsonObject>();
  lp["avgUs"] = loopAvgShown; lp["maxUs"] = loopMaxShown;

  JsonArray ta = o["tasks"].to<JsonArray>();
  for (uint8_t i = 0; i < taskCount; i++) {
    JsonObject t = ta.add<JsonObject>();
    t["name"] = tasks[i].name;
    t["stackFree"] = (uint32_t)uxTaskGetStackHighWaterMark(tasks[i].h);   // bytes on ESP32 FreeRTOS
  }
  JsonObject w = o["wifi"].to<JsonObject>();
  bool sta = WiFi.status() == WL_CONNECTED;
  w["connected"] = sta; w["ssid"] = sta ? WiFi.SSID() : String("");
  if (sta) { w["rssi"] = WiFi.RSSI(); w["channel"] = WiFi.channel(); }
  w["apClients"] = WiFi.softAPgetStationNum();
  JsonObject fl = o["flash"].to<JsonObject>();
  fl["configWrites"] = configWriteCount(); fl["bufferWrites"] = flashWrites; fl["bufferKB"] = flashBytes / 1024;
  JsonObject cl = o["clients"].to<JsonObject>();
  cl["events"] = sseClients; cl["websocket"] = wsClients;
}
