#ifndef STORAGE_MODULE_H
#define STORAGE_MODULE_H

#include <Arduino.h>
#include <InternalFileSystem.h>

// Запись журнала встреч (см. task.txt «Журнал встреч»).
struct Encounter {
    uint8_t  id[16];
    uint32_t tFirst;
    uint32_t tLast;
    int8_t   rssi;
    uint16_t count;
    uint32_t seq;
    uint8_t  tag[16];
};

class StorageModule {
public:
    static void begin();
    static void loop();

    static bool record(uint32_t nowUtc, const uint8_t id[16], int8_t rssi);
    static void clear();

    static int  count();
    static bool get(int index, Encounter& out);
    static int  snapshot(Encounter* out, int maxCount);

    static int  ackUpTo(uint32_t upto);
    static void flush();

    static const int MAX_ENCOUNTERS = 200;
    static const uint16_t ENC_FORMAT_VERSION = 1;

private:
    static const uint32_t ENC_MAGIC = 0x4D45544A; // 'METJ'
    static const char* ENC_FILE;
    static Encounter records[MAX_ENCOUNTERS];
    static int recCount;
    static volatile bool dirty;
    static uint32_t lastDirtyMs;

    static void load();
    static void save();
};

#endif
