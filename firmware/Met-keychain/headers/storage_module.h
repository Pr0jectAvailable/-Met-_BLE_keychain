#ifndef STORAGE_MODULE_H
#define STORAGE_MODULE_H

#include <Arduino.h>
#include <InternalFileSystem.h>

struct SeenDevice {
    uint8_t mac[6];
    int8_t rssi;
    uint32_t timestamp;
};

class StorageModule {
public:
    static void begin();
    static void addDevice(const uint8_t mac[6], int8_t rssi);
    static void clearDevices();
    static int getDeviceCount();
    static bool getDevice(int index, SeenDevice& device);
    static String getDevicesJson();

private:
    static const char* DEVICES_FILE;
    static const int MAX_DEVICES = 100;
    static SeenDevice devices[MAX_DEVICES];
    static int deviceCount;
    static void loadDevices();
    static void saveDevices();
};

#endif