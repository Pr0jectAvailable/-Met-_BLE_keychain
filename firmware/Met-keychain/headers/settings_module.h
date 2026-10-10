#ifndef SETTINGS_MODULE_H
#define SETTINGS_MODULE_H

#include <Arduino.h>

class SettingsModule {
public:
    static void begin();
    static void load();
    static void save();
    static void setName(const char* name);
    static const char* getName();
    static void setMac(const uint8_t mac[6]);
    static void getMac(uint8_t mac[6]);

private:
    static const char* SETTINGS_FILE;
    static char deviceName[32];
    static uint8_t macAddr[6];
};

#endif