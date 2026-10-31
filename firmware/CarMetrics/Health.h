// CarMetrics - board health: metrics (memory, CPU per core, loop time, task
// stacks, resets, flash writes), firmware rollback and safe-mode boot.
//
// Safe mode: after 3 crash resets in a row (panic / watchdog) the board starts
// with the last config that ran well ("good"), all relays off and no internet
// features, so sensors and alarms keep working. Leaving safe mode = reboot.
//
// Rollback: a newly installed firmware is only marked valid after it has run
// for HEALTHY_AFTER_S; if it crashes before that, the bootloader goes back to
// the previous firmware on the next reset.
#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

#define HEALTHY_AFTER_S 60       // uptime before firmware is marked valid and the crash counter clears
#define CONFIG_GOOD_AFTER_S 300  // uptime after a config change before it becomes the "good" config
#define SAFE_MODE_CRASHES 3

namespace Health {
void earlyBoot();                // first thing in setup()
bool safeMode();
String safeModeReason();
uint8_t crashCount();
void leaveSafeMode();            // clears the crash counter; caller reboots
void configApplied();            // restart the "config is good" timer

void loopStart();                // top of loop()
void loopEnd();                  // end of loop() (before delay)
void registerTask(const char* name);   // call from inside a task once it has started
void addFlashWrite(uint32_t bytes);    // offline buffer writes
void setClientCounts(uint32_t sse, uint32_t ws);

float cpuLoad(int core);         // 0..100 %, averaged over the last ~2 s
// System metrics recorded for the cloud upload ("sys_*" ids)
extern const char* const kSysIds[];
extern const uint8_t kSysCount;
float sysMetric(uint8_t i);

void toJson(JsonObject o);       // full board metrics (takes the state lock itself)
const char* resetReasonStr(int r);
}
