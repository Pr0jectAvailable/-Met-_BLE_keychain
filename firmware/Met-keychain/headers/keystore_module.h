#ifndef KEYSTORE_MODULE_H
#define KEYSTORE_MODULE_H

#include <Arduino.h>

// Хранилище ключей и идентификаторов брелока (см. task.txt).
class KeystoreModule {
public:
    static void begin();

    static const char* devNo();
    static void getKDev(uint8_t out[16]);
    static void getKPair(uint8_t out[16]);
    static void getKOwner(uint8_t out[16]);

    static bool paired();
    static void setPaired(bool v);
    static void setKOwner(const uint8_t kOwner[16]);

    // Провизьон с завода (см. task.txt): одноразовая запись devNo/K_dev/K_pair.
    // Разрешён только пока устройство не провизьонено (K_dev == 0) и не привязано.
    static bool provisioned();
    static bool provision(const char* devNo, const uint8_t kDev[16],
                          const uint8_t kPair[16]);

    static bool deriveLog(uint8_t out[32]);
    static bool deriveTime(uint8_t out[32]);
    static bool deriveRcpt(uint8_t out[32]);

    static uint32_t nextSeq();
    static uint32_t peekSeq();

    // Время живёт только в RAM: его задаёт телефон при подключении.
    // После перезагрузки снова невалидно -> брелок молчит, пока телефон
    // не поставит точное время заново (см. task.txt «Часы»).
    static bool     timeValid();
    static uint32_t utc();            // текущее UTC с учётом millis()
    static void     setUtc(uint32_t t);

    static uint16_t advIntervalMs();
    static int8_t   advTxPowerDbm();
    static uint16_t scanWindowMs();
    static uint16_t scanPeriodMs();
    static int8_t   rssiThreshold();

    static void setAdvIntervalMs(uint16_t v);
    static void setAdvTxPowerDbm(int8_t v);
    static void setScanWindowMs(uint16_t v);
    static void setScanPeriodMs(uint16_t v);
    static void setRssiThreshold(int8_t v);

    static void unpair();

    // Полный сброс провизьона (factory): обнуляет devNo/K_dev/K_pair/K_owner
    // и флаги. Используется заводским/сервисным инструментом, чтобы повторно
    // провизионить брелок без перепрошивки. Разрешать только в непривязанном
    // состоянии (проверка на стороне BLE-команды).
    static void factoryReset();

private:
    static const char* KEY_FILE;
    static void load();
    static void save();

    static volatile bool s_timeValid;
    static volatile uint32_t s_utcBase;
    static volatile uint32_t s_utcBaseMs;
};

#endif
