#include <Arduino.h>
#include <bluefruit.h>
#include "config.h"
#include "ble_module.h"
#include "storage_module.h"
#include "settings_module.h"
#include "ble_uart_module.h"

void setup() {
#if ENABLE_SERIAL
    Serial.begin(115200);
#endif

    InternalFS.begin();
    SettingsModule::begin();
    BLEModule::begin();
    BLEUartModule::begin();
}

void loop() {
    BLEUartModule::loop();

    // Печатаем boot-лог, только когда USB CDC реально готов.
    // Serial (как bool) == true, когда хост открыл порт (DTR).
    static bool bootLogged = false;
    if (!bootLogged && Serial) {
        bootLogged = true;
        LOGLN("===============");
        LOGLN("=== BOOT ===");
        LOG("Name: %s\n", SettingsModule::getName());
        uint8_t mac[6];
        SettingsModule::getMac(mac);
        LOG("MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        LOG("Devices: %d\n", StorageModule::getDeviceCount());
        LOGLN("===============");
    }

    delay(200);
}