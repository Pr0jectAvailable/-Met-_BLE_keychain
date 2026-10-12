#include "ble_module.h"
#include "config.h"
#include "storage_module.h"
#include "settings_module.h"
#include <InternalFileSystem.h>

#define SCAN_POWER_SAVE 0   // 0 = continuous scan (демо), 1 = 80% duty (продакшн)

// 16-бит UUID вместо 128-бит: экономит 16 байт в пакете,
// освобождая место под имя в основном ADV-пакете.
// ВАЖНО: формат UUID изменён — все сканеры/фильтры должны
// использовать этот же 16-бит идентификатор.
uint16_t BLEModule::baseUuid16 = 0x0201;

void BLEModule::begin() {
    const char* name = SettingsModule::getName();
    uint8_t mac[6];
    SettingsModule::getMac(mac);

    // --- setAddr: ble_gap_addr_t.addr — little-endian (addr[0] = младший
    // байт), а macAddr хранится в человеческом big-endian (AA:BB:CC:DD:EE:FF).
    // Разворачиваем перед передачей в SoftDevice.
    if (mac[0] | mac[1] | mac[2] | mac[3] | mac[4] | mac[5]) {
        ble_gap_addr_t addr;
        addr.addr_type = BLE_GAP_ADDR_TYPE_RANDOM_STATIC;
        for (int i = 0; i < 6; i++) addr.addr[i] = mac[5 - i];
        Bluefruit.setAddr(&addr);
    }

    // ATT MTU: без этой настройки Bluefruit держит дефолт 23 байта и
    // отклоняет запрос телефона на больший MTU, из-за чего команды
    // длиннее 20 байт (напр. SET_NAME) не отправляются. Значение должно
    // совпадать с PROTO_MTU_REPORT (config.h: 247). Вызывать ДО begin().
    Bluefruit.configPrphConn(PROTO_MTU_REPORT, BLE_GAP_EVENT_LENGTH_DEFAULT,
                             BLE_GATTS_HVN_TX_QUEUE_SIZE_DEFAULT,
                             BLE_GATTC_WRITE_CMD_TX_QUEUE_SIZE_DEFAULT);
    Bluefruit.begin(1, 1);
    Bluefruit.setName(name);
    Bluefruit.setTxPower(-4);

    // --- Advertising ---
    // Fast-режим: высокая частота ADV первые ADV_FAST_TIMEOUT_S секунд
    // (или всегда, если таймаут 0), затем slow — для быстрого взаимного
    // обнаружения двух устройств с этой прошивкой.
    Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);
    Bluefruit.Advertising.addUuid(BLEUuid(BLEModule::baseUuid16));
    // Имя теперь влезает в основной ADV-пакет (16-бит UUID освободил место).
    Bluefruit.Advertising.addName();
    Bluefruit.Advertising.setInterval(ADV_INTERVAL_FAST, ADV_INTERVAL_SLOW);
    Bluefruit.Advertising.setFastTimeout(ADV_FAST_TIMEOUT_S);
    Bluefruit.Advertising.start(0);

    // --- Scanning ---
    // restartOnDisconnect(false): скан не должен останавливаться на время
    // подключения — иначе во время сессии поиск микроконтроллеров «слепнет».
    Bluefruit.Scanner.setRxCallback(scanCallback);
    Bluefruit.Scanner.restartOnDisconnect(false);
    // После отключения ADV поднимется сам с обновлённым именем.
    Bluefruit.Advertising.restartOnDisconnect(true);

#if SCAN_POWER_SAVE
    Bluefruit.Scanner.setInterval(320, 256);   // 200/160 мс
#else
    Bluefruit.Scanner.setInterval(SCAN_INTERVAL, SCAN_WINDOW);
#endif

    Bluefruit.Scanner.useActiveScan(SCAN_ACTIVE);
#if SCAN_FILTER_BY_UUID
    // Фильтр по UUID оставлен как опция: для поиска произвольных MCU
    // он отключён (SCAN_FILTER_BY_UUID=0), иначе видны только «свои».
    Bluefruit.Scanner.filterUuid(BLEUuid(BLEModule::baseUuid16));
#endif
    Bluefruit.Scanner.start(0);

    LOG("BLE: adv@5s, scan %u/%u ms, TX -4 dBm, active=%u, filterUuid=%u\n",
        SCAN_INTERVAL, SCAN_WINDOW, SCAN_ACTIVE, SCAN_FILTER_BY_UUID);
}

// Извлекает Complete/Shortened Local Name (AD type 0x08/0x09) из adv-пакета.
// Возвращает длину имени (0, если имя отсутствует).
static int extractAdvName(ble_gap_evt_adv_report_t* report, char* out, int outSize) {
    if (outSize < 1) return 0;
    out[0] = '\0';

    const uint8_t* data = report->data.p_data;
    uint8_t len = report->data.len;
    int i = 0;

    while (i < len) {
        uint8_t fieldLen = data[i];
        if (fieldLen == 0) break;               // конец списка AD-структур
        if (i + 1 + fieldLen > len) break;      // обрезанная структура

        uint8_t adType = data[i + 1];
        const uint8_t* val = &data[i + 2];
        int valLen = fieldLen - 1;

        if (adType == BLE_GAP_AD_TYPE_COMPLETE_LOCAL_NAME ||
            adType == BLE_GAP_AD_TYPE_SHORT_LOCAL_NAME) {
            int copy = valLen;
            if (copy > outSize - 1) copy = outSize - 1;
            memcpy(out, val, copy);
            out[copy] = '\0';
            // Отсекаем возможные непечатные хвосты.
            for (int j = 0; j < copy; j++) {
                if (out[j] < 0x20 || out[j] > 0x7E) { out[j] = '\0'; break; }
            }
            return strlen(out);
        }
        i += 1 + fieldLen;
    }
    return 0;
}

void BLEModule::scanCallback(ble_gap_evt_adv_report_t* report) {
    // peer_addr.addr — little-endian (addr[0] = младший байт).
    // Разворачиваем в человеческий big-endian для журнала/вывода.
    uint8_t mac[6];
    for (int i = 0; i < 6; i++) mac[i] = report->peer_addr.addr[5 - i];
    int8_t rssi = report->rssi;

    // Отсечение по RSSI: слишком слабые пакеты (шум, дальние устройства)
    // не попадают в журнал, освобождая слоты под реальные микроконтроллеры.
#if SCAN_MIN_RSSI
    if (rssi < SCAN_MIN_RSSI) {
        Bluefruit.Scanner.resume();
        return;
    }
#endif

    // Пропускаем собственный ADV (этот же MAC) — иначе шлюз логирует сам себя.
    static uint8_t selfMac[6];
    static bool selfMacValid = false;
    if (!selfMacValid) {
        SettingsModule::getMac(selfMac);
        selfMacValid = true;
    }
    if (memcmp(mac, selfMac, 6) == 0) {
        Bluefruit.Scanner.resume();
        return;
    }

    // Имя достаём из adv-пакета или scan response (active scan их
    // запрашивает). Если устройство имени не транслирует — пустая строка.
    char name[SEEN_NAME_MAX];
    extractAdvName(report, name, sizeof(name));

    // Дедупликация по времени: частые ADV не должны дёргать flash.
    // addDevice сам обновляет RSSI/имя существующей записи, поэтому
    // достаточно ограничить частоту вызовов для уже известного MAC.
    static uint32_t lastSeenMs = 0;
    static uint8_t lastSeenMac[6] = {0};
    uint32_t now = millis();
    if (memcmp(mac, lastSeenMac, 6) == 0 &&
        (now - lastSeenMs) < SCAN_DEVICE_REFRESH_MS) {
        Bluefruit.Scanner.resume();
        return;
    }
    memcpy(lastSeenMac, mac, 6);
    lastSeenMs = now;

    // upsertDevice вместо addDevice: при заполненном журнале вытесняет
    // худшую (слабую/старую) запись, сохраняя сильные сигналы MCU.
    StorageModule::upsertDevice(mac, rssi, name);
    Bluefruit.Scanner.resume();
}

void BLEModule::setDeviceName(const char* name) {
    Bluefruit.setName(name);

    // Всегда пересобираем ADV, чтобы сканеры видели новое имя.
    Bluefruit.Advertising.stop();
    Bluefruit.Advertising.clearData();
    Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);
    Bluefruit.Advertising.addUuid(BLEUuid(BLEModule::baseUuid16));
    Bluefruit.Advertising.addName();
    Bluefruit.Advertising.setInterval(ADV_INTERVAL_FAST, ADV_INTERVAL_SLOW);
    Bluefruit.Advertising.setFastTimeout(ADV_FAST_TIMEOUT_S);

    // Возобновляем только если не подключены; иначе ADV стартует
    // по событию disconnect (restartOnDisconnect включён).
    if (Bluefruit.connected() == 0) {
        Bluefruit.Advertising.start(0);
    }
}

void BLEModule::setMacAddress(const uint8_t mac[6]) {
    // применяется после NVIC_SystemReset() из BLEUartModule
}