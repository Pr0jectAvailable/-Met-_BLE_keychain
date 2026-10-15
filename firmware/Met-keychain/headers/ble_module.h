#ifndef BLE_MODULE_H
#define BLE_MODULE_H

#include <bluefruit.h>

// Реклама и сканирование брелока (см. task.txt «Реклама», «Сканирование»).
class BLEModule {
public:
    static void begin();
    static void loop();
    static void restartAdvertising();

private:
    static void scanCallback(ble_gap_evt_adv_report_t* report);
    static void buildAdvertising();
    static void rotateMac();
    static void updateAnonId();

    static uint8_t  currentMac[6];
    static uint8_t  anonId[16];
    static uint32_t lastEpoch;
    static bool     advRunning;
};

#endif
