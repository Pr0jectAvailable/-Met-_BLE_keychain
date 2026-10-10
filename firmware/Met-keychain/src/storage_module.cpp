#include "storage_module.h"
#include "config.h"
#include <InternalFileSystem.h>

using namespace Adafruit_LittleFS_Namespace;   // <-- ФИКС

const char* StorageModule::DEVICES_FILE = "/devices.dat";
SeenDevice StorageModule::devices[StorageModule::MAX_DEVICES];
int StorageModule::deviceCount = 0;

void StorageModule::begin() {
    loadDevices();
}

void StorageModule::addDevice(const uint8_t mac[6], int8_t rssi) {
    for (int i = 0; i < deviceCount; i++) {
        if (memcmp(devices[i].mac, mac, 6) == 0) {
            devices[i].rssi = rssi;
            devices[i].timestamp = millis();
            saveDevices();
            return;
        }
    }
    if (deviceCount < MAX_DEVICES) {
        memcpy(devices[deviceCount].mac, mac, 6);
        devices[deviceCount].rssi = rssi;
        devices[deviceCount].timestamp = millis();
        deviceCount++;
        saveDevices();
        LOG("New device: %02X:%02X:%02X:%02X:%02X:%02X\n",
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
}

void StorageModule::clearDevices() {
    deviceCount = 0;
    saveDevices();
    LOGLN("Device log cleared.");
}

int StorageModule::getDeviceCount() {
    return deviceCount;
}

bool StorageModule::getDevice(int index, SeenDevice& device) {
    if (index < 0 || index >= deviceCount) return false;
    device = devices[index];
    return true;
}

String StorageModule::getDevicesJson() {
    String json = "[";
    for (int i = 0; i < deviceCount; i++) {
        if (i > 0) json += ",";
        char macStr[18];
        sprintf(macStr, "%02X:%02X:%02X:%02X:%02X:%02X",
                devices[i].mac[0], devices[i].mac[1], devices[i].mac[2],
                devices[i].mac[3], devices[i].mac[4], devices[i].mac[5]);
        json += "{\"mac\":\"" + String(macStr) +
                "\",\"rssi\":" + String(devices[i].rssi) +
                ",\"time\":" + String(devices[i].timestamp) + "}";
    }
    json += "]";
    return json;
}

void StorageModule::loadDevices() {
    File file = InternalFS.open(DEVICES_FILE, FILE_O_READ);
    if (file) {
        deviceCount = file.read((uint8_t*)devices,
                                sizeof(SeenDevice) * MAX_DEVICES) / sizeof(SeenDevice);
        file.close();
        LOG("Loaded %d devices.\n", deviceCount);
    } else {
        deviceCount = 0;
        LOGLN("No device log yet.");
    }
}

void StorageModule::saveDevices() {
    File file = InternalFS.open(DEVICES_FILE, FILE_O_WRITE);
    if (file) {
        file.write((uint8_t*)devices, sizeof(SeenDevice) * deviceCount);
        file.close();
    }
}