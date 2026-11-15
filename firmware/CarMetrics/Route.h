// CarMetrics - API path matching (kept free of Arduino types so it is tested on the PC:
// tools/prototest/route_test.cpp). Pattern parts are literal, or "*" = exactly one
// non-empty path part. Matching is exact: "boards" never answers "boards/config".
#pragma once
#include <string>
#include <cstring>

inline bool routeMatch(const char* pat, const char* path, std::string* parts, int maxParts) {
  size_t i = 0, n = strlen(path); int np = 0;
  while (true) {
    if (*pat == '*') {
      size_t e = i;
      while (e < n && path[e] != '/') e++;
      if (e == i) return false;
      if (np < maxParts) parts[np] = std::string(path + i, e - i);
      np++; i = e; pat++;
    } else if (*pat == 0) return i == n;
    else if (i < n && path[i] == *pat) { i++; pat++; }
    else return false;
  }
}
