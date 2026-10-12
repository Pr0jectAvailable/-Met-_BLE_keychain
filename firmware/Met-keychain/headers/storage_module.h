#ifndef STORAGE_MODULE_H
#define STORAGE_MODULE_H

#include <Arduino.h>
#include <InternalFileSystem.h>

// Максимальная длина имени устройства в журнале (с учётом '\0').
static const int SEEN_NAME_MAX = 24;

struct SeenDevice {
    uint8_t mac[6];
    int8_t rssi;
    uint32_t timestamp;
    char name[SEEN_NAME_MAX];
};

class StorageModule {
public:
    static void begin();
    static void loop();                       // отложенная запись dirty-флага

    // name может быть nullptr/пустым — тогда сохраняется прежнее имя (или "").
    static void addDevice(const uint8_t mac[6], int8_t rssi, const char* name);
    static void clearDevices();

    // Вытеснение: если журнал заполнен (MAX_DEVICES), новая запись замещает
    // самую слабую/старую (наименьший RSSI, при равенстве — самый старый
    // timestamp). Возвращает true, если запись добавлена/обновлена.
    // Помогает искать микроконтроллеры: сильные сигналы не вытесняются
    // мусорными дальними пакетами.
    static bool upsertDevice(const uint8_t mac[6], int8_t rssi, const char* name);

    static int getDeviceCount();
    static bool getDevice(int index, SeenDevice& device);

    // Потокобезопасный снимок журнала для передачи по BLE.
    // Копирует текущее состояние в переданный буфер, возвращает число устройств.
    // bufsize задаётся в элементах SeenDevice. Защищает от гонок с scan-callback.
    static int snapshot(SeenDevice* out, int maxCount);

    // Полный JSON (для отладки/локального использования).
    static String getDevicesJson();

    // JSON только для диапазона [offset, offset+count), total = общий размер журнала.
    static String getDevicesJsonRange(int offset, int count, int& total);

    // Принудительно сбросить отложенные изменения на диск.
    static void flush();

    // Верхняя граница журнала. public, чтобы клиент мог запрашивать страницы
    // и выделять буфер снимка, не имея доступа к приватным данным.
    static const int MAX_DEVICES = 100;

    // Версия формата файла журнала. Меняется при изменении layout
    // SeenDevice (например, добавлении name). На диске перед массивом
    // хранится заголовок {magic, version}, поэтому старые файлы
    // детектируются и сбрасываются без чтения несовместимого бинарника.
    static const uint16_t DEVICES_FORMAT_VERSION = 2;

private:
    // Заголовок файла журнала: magic + version.
    static const uint32_t DEVICES_MAGIC = 0x4D455431; // 'MET1'
    static const int HEADER_SIZE = sizeof(uint32_t) + sizeof(uint16_t);

    static const char* DEVICES_FILE;
    static SeenDevice devices[MAX_DEVICES];
    static int deviceCount;
    static volatile bool dirty;
    static uint32_t lastDirtyMs;

    static void loadDevices();
    static void saveDevices();
};

#endif
