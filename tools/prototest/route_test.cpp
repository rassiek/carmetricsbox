// API v1 path matching (firmware/CarMetrics/Route.h)
#include "Route.h"
#include <cstdio>

void check(bool ok, const char* what, const std::string& detail);
static void ok(bool c, const char* what) { check(c, (std::string("route: ") + what).c_str(), ""); }

void routeTest() {
  std::string p[2];
  ok(routeMatch("boards", "boards", p, 2), "literal");
  ok(!routeMatch("boards", "boards/config", p, 2), "exact: no prefix match");
  ok(!routeMatch("boards/*", "boards", p, 2), "star needs a part");
  ok(routeMatch("boards/*", "boards/3", p, 2) && p[0] == "3", "star captures");
  ok(!routeMatch("boards/*", "boards/3/command", p, 2), "star is one part");
  ok(routeMatch("actuators/*/command", "actuators/fan/command", p, 2) && p[0] == "fan", "star in the middle");
  ok(!routeMatch("actuators/*/command", "actuators//command", p, 2), "empty part refused");
  ok(routeMatch("", "", p, 2), "index");
  ok(!routeMatch("", "x", p, 2), "index exact");
  ok(routeMatch("network/saved/*", "network/saved/My%20Wifi", p, 2) && p[0] == "My%20Wifi", "encoded part");
  ok(routeMatch("points/*", "points/local.coolant_temp", p, 2) && p[0] == "local.coolant_temp", "dotted address");
}
