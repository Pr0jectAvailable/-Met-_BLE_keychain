#ifndef BLE_MODULE_H
#define BLE_MODULE_H

#include <bluefruit.h>

class BLEModule {
public:
    static void begin();
    static void setDeviceName(const char* name);
    static void setMacAddress(const uint8_t mac[6]);

private:
    static void scanCallback(ble_gap_evt_adv_report_t* report);
    static uint8_t baseUuid[16];
};

#endif