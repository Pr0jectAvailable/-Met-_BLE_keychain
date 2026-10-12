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
    // 16-бит UUID (0x0201) вместо 128-бит: экономит 16 байт в ADV-пакете.
    // ВАЖНО: фильтр сканера и внешние сканеры должны использовать этот же UUID.
    static uint16_t baseUuid16;
};

#endif
