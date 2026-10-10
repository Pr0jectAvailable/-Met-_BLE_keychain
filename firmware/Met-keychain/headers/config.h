#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>

// 1 = Serial включён (отладка), 0 = выключен (батарея).
// USB CDC держит PHY включённым и тянет ~0.5–1 mA, поэтому в релизе выключаем.
#define ENABLE_SERIAL 1

#if ENABLE_SERIAL
  #define LOG(...)   Serial.printf(__VA_ARGS__)
  #define LOGLN(s)   Serial.println(s)
#else
  #define LOG(...)   do {} while (0)
  #define LOGLN(s)   do {} while (0)
#endif

#endif