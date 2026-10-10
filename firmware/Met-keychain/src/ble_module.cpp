#include "ble_module.h"
#include "config.h"
#include "storage_module.h"
#include "settings_module.h"
#include <InternalFileSystem.h>

#define SCAN_POWER_SAVE 0   // 0 = continuous scan (демо), 1 = 80% duty (продакшн)

uint8_t BLEModule::baseUuid[16] = {
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
    0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10
};

void BLEModule::begin() {
    const char* name = SettingsModule::getName();
    uint8_t mac[6];
    SettingsModule::getMac(mac);

    // --- ФИКС setAddr: используем ble_gap_addr_t* ---
    if (mac[0] | mac[1] | mac[2] | mac[3] | mac[4] | mac[5]) {
        ble_gap_addr_t addr;
        addr.addr_type = BLE_GAP_ADDR_TYPE_RANDOM_STATIC;
        memcpy(addr.addr, mac, 6);
        Bluefruit.setAddr(&addr);
    }

    Bluefruit.begin(1, 1);
    Bluefruit.setName(name);
    Bluefruit.setTxPower(-4);

    // --- Advertising ---
    Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);
    Bluefruit.Advertising.addUuid(BLEUuid(baseUuid));
    Bluefruit.Advertising.addName();
    Bluefruit.Advertising.setInterval(8000, 8000);   // 5 с
    Bluefruit.Advertising.setFastTimeout(30);
    Bluefruit.Advertising.start(0);

    // --- Scanning ---
    Bluefruit.Scanner.setRxCallback(scanCallback);
    Bluefruit.Scanner.restartOnDisconnect(true);

#if SCAN_POWER_SAVE
    Bluefruit.Scanner.setInterval(320, 256);   // 200/160 мс
#else
    Bluefruit.Scanner.setInterval(320, 160);   // 200/100 мс = 50% duty
#endif

    Bluefruit.Scanner.useActiveScan(false);
    Bluefruit.Scanner.filterUuid(BLEUuid(baseUuid));
    Bluefruit.Scanner.start(0);

#if SCAN_POWER_SAVE
    LOGLN("BLE: adv@5s, scan 160/200 ms, TX -4 dBm, passive");
#else
    LOGLN("BLE: adv@5s, scan CONTINUOUS, TX -4 dBm, passive");
#endif
}

void BLEModule::scanCallback(ble_gap_evt_adv_report_t* report) {
    uint8_t mac[6];
    memcpy(mac, report->peer_addr.addr, 6);
    int8_t rssi = report->rssi;
    StorageModule::addDevice(mac, rssi);
    Bluefruit.Scanner.resume();
}

void BLEModule::setDeviceName(const char* name) {
    Bluefruit.setName(name);

    // Обновляем advertising только если НЕ подключены.
    // Если подключены — GAP-имя обновится, и следующий телефон
    // увидит новое имя при сканировании.
    if (Bluefruit.connected() == 0) {
        Bluefruit.Advertising.stop();
        Bluefruit.Advertising.clearData();
        Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);
        Bluefruit.Advertising.addUuid(BLEUuid(baseUuid));
        Bluefruit.Advertising.addName();
        Bluefruit.Advertising.setInterval(8000, 8000);
        Bluefruit.Advertising.start(0);
    }
}

void BLEModule::setMacAddress(const uint8_t mac[6]) {
    // применяется после NVIC_SystemReset() из BLEUartModule
}