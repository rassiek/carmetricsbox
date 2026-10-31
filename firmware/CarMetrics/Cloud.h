// CarMetrics - internet features: NTP time, public weather, metrics upload.
//
// Recorder (loop task): downsamples sensor values (avg/min/max per interval)
//   into 4 KB chunks kept in RAM.
// Net task (own FreeRTOS task): when the ESP32 has joined a Wi-Fi network it
//   sets the clock (NTP), fetches weather, and uploads chunks (Zabbix trapper or
//   InfluxDB line protocol). While offline, chunks go to a flash ring buffer
//   (/q on LittleFS, capped at bufferKB, oldest dropped) and are sent later.
#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

namespace Cloud {
void begin();             // (re)configure recorder after config changes (loop)
void update();            // recorder tick (loop)
void startNetTask();      // once from setup
void requestWeather();    // fetch weather now
void requestFlush();      // try uploading now
void requestClearBacklog();
void weatherToJson(JsonObject o);  // lock held
void statusToJson(JsonObject o);   // lock held
#ifdef CM_EMU_SELFTEST
void selftestEncode(String& influx, String& zabbix, uint32_t& records);
#endif
}
