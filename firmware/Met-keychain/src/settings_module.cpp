#include "settings_module.h"
#include "config.h"
#include <InternalFileSystem.h>

using namespace Adafruit_LittleFS_Namespace;   // <-- ФИКС

const char* SettingsModule::SETTINGS_FILE = "/settings.dat";
char SettingsModule::deviceName[32] = "Tracker";
uint8_t SettingsModule::macAddr[6] = {0};

void SettingsModule::begin() {
    load();
}

void SettingsModule::load() {
    File file = InternalFS.open(SETTINGS_FILE, FILE_O_READ);

    if (!file) {
        LOGLN("No settings file -> defaults.");
        strncpy(deviceName, "Tracker", sizeof(deviceName) - 1);
        deviceName[sizeof(deviceName) - 1] = '\0';
        memset(macAddr, 0, 6);
        save();
        return;
    }

    size_t rd1 = file.read((uint8_t*)deviceName, sizeof(deviceName));
    size_t rd2 = file.read(macAddr, 6);
    file.close();

    LOG("Loaded: rd1=%u rd2=%u\n", (unsigned)rd1, (unsigned)rd2);

    // Sanity-проверка: длина + нуль-терминация + печатные ASCII
    bool bad = false;
    if (rd1 != sizeof(deviceName)) bad = true;
    if (rd2 != 6) bad = true;
    if (deviceName[0] == '\0') bad = true;
    if (!bad) {
        for (size_t i = 0; i < sizeof(deviceName) && deviceName[i] != '\0'; i++) {
            if (deviceName[i] < 0x20 || deviceName[i] > 0x7E) { bad = true; break; }
        }
    }

    if (bad) {
        LOGLN("Settings corrupted -> reset to 'Tracker'.");
        strncpy(deviceName, "Tracker", sizeof(deviceName) - 1);
        deviceName[sizeof(deviceName) - 1] = '\0';
        memset(macAddr, 0, 6);
        save();
    } else {
        LOG("Name='%s'\n", deviceName);
    }
}

void SettingsModule::save() {
    InternalFS.remove(SETTINGS_FILE);
    File file = InternalFS.open(SETTINGS_FILE, FILE_O_WRITE);
    if (!file) {
        LOGLN("save(): open failed!");
        return;
    }
    size_t w1 = file.write((uint8_t*)deviceName, sizeof(deviceName));
    size_t w2 = file.write(macAddr, 6);
    file.close();
    LOG("save(): wrote %u + %u bytes\n", (unsigned)w1, (unsigned)w2);
}

void SettingsModule::setName(const char* name) {
    strncpy(deviceName, name, sizeof(deviceName) - 1);
    deviceName[sizeof(deviceName) - 1] = '\0';
    save();
}

const char* SettingsModule::getName() {
    // Всегда проверяем и лечим
    bool empty = true;
    for (size_t i = 0; i < sizeof(deviceName); i++) {
        if (deviceName[i] != 0 && deviceName[i] != 0xFF) {
            empty = false;
            break;
        }
    }
    if (empty) {
        strncpy(deviceName, "Tracker", sizeof(deviceName) - 1);
        deviceName[sizeof(deviceName) - 1] = '\0';
        Serial.println("getName(): healed to 'Tracker'");
    }
    return deviceName;
}

void SettingsModule::setMac(const uint8_t mac[6]) {
    memcpy(macAddr, mac, 6);
    save();
}

void SettingsModule::getMac(uint8_t mac[6]) {
    memcpy(mac, macAddr, 6);
}