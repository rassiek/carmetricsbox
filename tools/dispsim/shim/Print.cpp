#include "Arduino.h"
size_t Print::print(const String& s) { return write(s.c_str()); }
