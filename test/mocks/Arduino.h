#include <cstdio>
#pragma once

#include <cstdint>
#include <cmath>
#include "Stream.h"

inline uint32_t g_mock_millis = 0;

using std::isnan;

inline uint32_t millis() {
  return g_mock_millis;
}

inline void delay(uint32_t ms) {
  g_mock_millis += ms;
}

// Arduino number->string conversions (minimal, for TxtDataHelpers)
inline char* ltoa(long v, char* s, int base) {
  if (base < 2 || base > 36) { s[0] = 0; return s; }
  char tmp[70]; int i = 0; bool neg = v < 0;
  unsigned long u = neg ? -(unsigned long)v : (unsigned long)v;
  do { unsigned d = u % base; tmp[i++] = d < 10 ? '0'+d : 'a'+d-10; u /= base; } while (u);
  int j = 0; if (neg) s[j++] = '-';
  while (i) s[j++] = tmp[--i];
  s[j] = 0; return s;
}
inline char* ultoa(unsigned long v, char* s, int base) { return ltoa((long)v, s, base); }
inline char* itoa(int v, char* s, int base) { return ltoa((long)v, s, base); }
