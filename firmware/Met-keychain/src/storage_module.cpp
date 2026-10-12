#include "storage_module.h"
#include "config.h"
#include <InternalFileSystem.h>

using namespace Adafruit_LittleFS_Namespace;

const char* StorageModule::DEVICES_FILE = "/devices.dat";
const uint32_t StorageModule::DEVICES_MAGIC;
const uint16_t StorageModule::DEVICES_FORMAT_VERSION;
SeenDevice StorageModule::devices[StorageModule::MAX_DEVICES];
int StorageModule::deviceCount = 0;
volatile bool StorageModule::dirty = false;
uint32_t StorageModule::lastDirtyMs = 0;

// Задержка перед записью на flash после последнего изменения.
// Защищает flash от износа при частых RSSI-обновлениях в scan-callback.
static const uint32_t SAVE_DEBOUNCE_MS = 5000;

void StorageModule::begin() {
    loadDevices();
}

void StorageModule::loop() {
    if (dirty && (millis() - lastDirtyMs) > SAVE_DEBOUNCE_MS) {
        saveDevices();
        dirty = false;
    }
}

void StorageModule::addDevice(const uint8_t mac[6], int8_t rssi, const char* name) {
    for (int i = 0; i < deviceCount; i++) {
        if (memcmp(devices[i].mac, mac, 6) == 0) {
            devices[i].rssi = rssi;
            devices[i].timestamp = millis();
            // Имя может появиться позже (active scan / смена adv-пакета):
            // обновляем, не затирая уже известное непустое значение пустым.
            if (name && name[0]) {
                strncpy(devices[i].name, name, SEEN_NAME_MAX - 1);
                devices[i].name[SEEN_NAME_MAX - 1] = '\0';
            }
            dirty = true;
            lastDirtyMs = millis();
            return;
        }
    }
    if (deviceCount < MAX_DEVICES) {
        memcpy(devices[deviceCount].mac, mac, 6);
        devices[deviceCount].rssi = rssi;
        devices[deviceCount].timestamp = millis();
        if (name && name[0]) {
            strncpy(devices[deviceCount].name, name, SEEN_NAME_MAX - 1);
            devices[deviceCount].name[SEEN_NAME_MAX - 1] = '\0';
        } else {
            devices[deviceCount].name[0] = '\0';
        }
        deviceCount++;
        dirty = true;
        lastDirtyMs = millis();
        LOG("New device: %02X:%02X:%02X:%02X:%02X:%02X name='%s'\n",
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
            devices[deviceCount - 1].name);
    }
}

// Улучшенный upsert с вытеснением худшей записи при переполнении.
// Возвращает true, если данные изменились.
bool StorageModule::upsertDevice(const uint8_t mac[6], int8_t rssi,
                                 const char* name) {
    uint32_t now = millis();

    // 1. Обновление существующей записи.
    for (int i = 0; i < deviceCount; i++) {
        if (memcmp(devices[i].mac, mac, 6) == 0) {
            devices[i].rssi = rssi;
            devices[i].timestamp = now;
            if (name && name[0]) {
                strncpy(devices[i].name, name, SEEN_NAME_MAX - 1);
                devices[i].name[SEEN_NAME_MAX - 1] = '\0';
            }
            dirty = true;
            lastDirtyMs = now;
            return true;
        }
    }

    // 2. Есть свободный слот — добавляем.
    if (deviceCount < MAX_DEVICES) {
        memcpy(devices[deviceCount].mac, mac, 6);
        devices[deviceCount].rssi = rssi;
        devices[deviceCount].timestamp = now;
        if (name && name[0]) {
            strncpy(devices[deviceCount].name, name, SEEN_NAME_MAX - 1);
            devices[deviceCount].name[SEEN_NAME_MAX - 1] = '\0';
        } else {
            devices[deviceCount].name[0] = '\0';
        }
        deviceCount++;
        dirty = true;
        lastDirtyMs = now;
        LOG("New device: %02X:%02X:%02X:%02X:%02X:%02X name='%s'\n",
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
            devices[deviceCount - 1].name);
        return true;
    }

    // 3. Журнал полон — ищем худшую запись (наименьший RSSI, затем
    // самый старый timestamp). Новую принимаем только если она «лучше».
    int worst = 0;
    for (int i = 1; i < deviceCount; i++) {
        if (devices[i].rssi < devices[worst].rssi ||
            (devices[i].rssi == devices[worst].rssi &&
             (int32_t)(devices[i].timestamp - devices[worst].timestamp) < 0)) {
            worst = i;
        }
    }
    if (rssi <= devices[worst].rssi) {
        // Новая запись не лучше худшей — отбрасываем.
        return false;
    }
    memcpy(devices[worst].mac, mac, 6);
    devices[worst].rssi = rssi;
    devices[worst].timestamp = now;
    if (name && name[0]) {
        strncpy(devices[worst].name, name, SEEN_NAME_MAX - 1);
        devices[worst].name[SEEN_NAME_MAX - 1] = '\0';
    } else {
        devices[worst].name[0] = '\0';
    }
    dirty = true;
    lastDirtyMs = now;
    LOG("Evict worst slot %d -> %02X:%02X:%02X:%02X:%02X:%02X rssi=%d\n",
        worst, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], rssi);
    return true;
}

void StorageModule::clearDevices() {
    deviceCount = 0;
    saveDevices();
    dirty = false;
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

int StorageModule::snapshot(SeenDevice* out, int maxCount) {
    if (!out || maxCount <= 0) return 0;
    int n = deviceCount;
    if (n > maxCount) n = maxCount;
    memcpy(out, devices, sizeof(SeenDevice) * n);
    return n;
}

String StorageModule::getDevicesJson() {
    int total = 0;
    return getDevicesJsonRange(0, deviceCount, total);
}

String StorageModule::getDevicesJsonRange(int offset, int count, int& total) {
    total = deviceCount;
    if (offset < 0) offset = 0;
    if (offset > deviceCount) offset = deviceCount;
    int end = offset + count;
    if (end > deviceCount) end = deviceCount;

    // Резервируем примерно 130 байт на запись (имя до 23 символов), чтобы
    // избежать реаллокаций.
    String json;
    json.reserve(2 + (end - offset) * 130);
    json = "[";
    for (int i = offset; i < end; i++) {
        if (i > offset) json += ",";
        char macStr[18];
        sprintf(macStr, "%02X:%02X:%02X:%02X:%02X:%02X",
                devices[i].mac[0], devices[i].mac[1], devices[i].mac[2],
                devices[i].mac[3], devices[i].mac[4], devices[i].mac[5]);
        json += "{\"mac\":\"";
        json += macStr;
        json += "\",\"rssi\":";
        json += devices[i].rssi;
        json += ",\"time\":";
        json += devices[i].timestamp;
        json += ",\"name\":\"";
        json += devices[i].name;
        json += "\"}";
    }
    json += "]";
    return json;
}

void StorageModule::flush() {
    if (dirty) {
        saveDevices();
        dirty = false;
    }
}

void StorageModule::loadDevices() {
    deviceCount = 0;
    dirty = false;

    File file = InternalFS.open(DEVICES_FILE, FILE_O_READ);
    if (!file) {
        LOGLN("No device log yet.");
        return;
    }

    // Читаем заголовок и проверяем совместимость формата.
    uint32_t magic = 0;
    uint16_t version = 0;
    file.read((uint8_t*)&magic, sizeof(magic));
    file.read((uint8_t*)&version, sizeof(version));

    if (magic != DEVICES_MAGIC || version != DEVICES_FORMAT_VERSION) {
        LOG("Devices file: bad header (magic=0x%08X ver=%u) -> reset.\n",
            (unsigned)magic, (unsigned)version);
        file.close();
        InternalFS.remove(DEVICES_FILE);
        return;
    }

    int raw = file.read((uint8_t*)devices, sizeof(SeenDevice) * MAX_DEVICES);
    int n = raw / sizeof(SeenDevice);
    if (n < 0) n = 0;
    if (n > MAX_DEVICES) n = MAX_DEVICES;
    deviceCount = n;
    file.close();

    // Гарантируем нуль-терминацию имён (на случай частично записанных
    // записей при сбое питания во время saveDevices()).
    for (int i = 0; i < deviceCount; i++) {
        devices[i].name[SEEN_NAME_MAX - 1] = '\0';
    }

    LOG("Loaded %d devices.\n", deviceCount);
}

void StorageModule::saveDevices() {
    InternalFS.remove(DEVICES_FILE);
    File file = InternalFS.open(DEVICES_FILE, FILE_O_WRITE);
    if (file) {
        // Сначала заголовок формата, затем массив записей.
        file.write((uint8_t*)&DEVICES_MAGIC, sizeof(DEVICES_MAGIC));
        file.write((uint8_t*)&DEVICES_FORMAT_VERSION,
                   sizeof(DEVICES_FORMAT_VERSION));
        file.write((uint8_t*)devices, sizeof(SeenDevice) * deviceCount);
        file.close();
    }
}
