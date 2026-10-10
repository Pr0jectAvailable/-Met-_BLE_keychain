#include "ble_uart_module.h"
#include "config.h"
#include "storage_module.h"
#include "settings_module.h"
#include "ble_module.h"

BLEUart BLEUartModule::bleuart;

void BLEUartModule::begin() {
    bleuart.begin();
    bleuart.setRxCallback(rxCallback);
    // Advertising уже настроен в BLEModule::begin() (наш baseUuid + name).
    // UART-сервис телефон обнаружит после подключения.
    LOGLN("BLE UART ready.");
}

void BLEUartModule::loop() {
    // Пусто — работа в колбэке.
}

void BLEUartModule::rxCallback(uint16_t conn_handle) {
    char buffer[160];
    int len = bleuart.read(buffer, sizeof(buffer) - 1);
    if (len <= 0) return;

    buffer[len] = '\0';
    LOG("RX chunk (%d): '%s'\n", len, buffer);

    // Статический буфер для накопления команды
    static String cmdBuffer = "";

    for (int i = 0; i < len; i++) {
        char c = buffer[i];
        if (c == '\n' || c == '\r') {
            // Конец команды — обрабатываем
            cmdBuffer.trim();
            if (cmdBuffer.length() > 0) {
                LOG("Complete command: '%s'\n", cmdBuffer.c_str());
                processCommand(cmdBuffer);
            }
            cmdBuffer = ""; // Сбрасываем буфер
        } else {
            cmdBuffer += c;
            // Защита от переполнения
            if (cmdBuffer.length() > 200) {
                LOGLN("Command too long, resetting buffer.");
                cmdBuffer = "";
            }
        }
    }
}

void BLEUartModule::processCommand(const String& command) {
    if (command.length() == 0) return;

    char buf[220];   // хватит и на JSON, и на строки

    if (command == "PING") {
        bleuart.write("PONG\n", 5);
        return;
    }

    if (command == "GET_NAME") {
        snprintf(buf, sizeof(buf), "NAME:%s\n", SettingsModule::getName());
        bleuart.write(buf, strlen(buf));
        return;
    }

    if (command == "GET_MAC") {
        uint8_t mac[6];
        SettingsModule::getMac(mac);
        snprintf(buf, sizeof(buf),
                 "MAC:%02X:%02X:%02X:%02X:%02X:%02X\n",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        bleuart.write(buf, strlen(buf));
        return;
    }

    if (command.startsWith("SET_NAME:")) {
        String name = command.substring(9);
        name.trim();
        if (name.length() == 0) {
            bleuart.write("ERROR:empty name\n", 17);
            return;
        }
        SettingsModule::setName(name.c_str());
        BLEModule::setDeviceName(name.c_str());
        snprintf(buf, sizeof(buf), "OK:%s\n", SettingsModule::getName());
        bleuart.write(buf, strlen(buf));
        return;
    }

    if (command == "GET_DEVICES") {
        String json = StorageModule::getDevicesJson();
        // Отдельный вызов для длинных данных — String::c_str + \n одной строкой
        String line = json + "\n";
        bleuart.write((const uint8_t*)line.c_str(), line.length());
        return;
    }

    if (command == "CLEAR_DEVICES") {
        StorageModule::clearDevices();
        bleuart.write("OK:cleared\n", 11);
        return;
    }

    if (command.startsWith("SET_MAC:")) {
        String macStr = command.substring(8);
        uint8_t mac[6];
        if (sscanf(macStr.c_str(), "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
                   &mac[0], &mac[1], &mac[2], &mac[3], &mac[4], &mac[5]) == 6) {
            SettingsModule::setMac(mac);
            bleuart.write("OK:rebooting\n", 13);
            delay(200);
            NVIC_SystemReset();
        } else {
            bleuart.write("ERROR:MAC format\n", 17);
        }
        return;
    }

    bleuart.write("ERROR:unknown\n", 14);
}