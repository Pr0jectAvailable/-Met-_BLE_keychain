#include "ble_module.h"
#include "config.h"
#include "crypto_module.h"
#include "keystore_module.h"
#include "storage_module.h"
#include "ble_uart_module.h"

uint8_t  BLEModule::currentMac[6] = {0};
uint8_t  BLEModule::anonId[16] = {0};
uint32_t BLEModule::lastEpoch = 0;
bool     BLEModule::advRunning = false;

// Диагностика: сколько ADV прошло парс и было записано в журнал.
static uint32_t s_advSeen = 0;
static uint32_t s_advRecorded = 0;
static uint32_t s_rawReports = 0;   // DIAG: все отчёты сканера (до парсинга)

// Запрос на перезапуск радио (скан+реклама) из основного контекста.
// Ставится в onDisconnect (контекст события SoftDevice) и обрабатывается
// в BLEModule::loop(): запускать скан/рекламу прямо в обработчике события
// нельзя — стек уходит в рассинхрон (raw=0) и МК сбрасывается.
static volatile bool s_radioRestartPending = false;

// --- Буфер отчётов сканера (producer/consumer) ---
// scanCallback() выполняется в контексте события SoftDevice (ISR). Любая
// блокирующая работа там (Serial LOG, HMAC в computeTag(), запись во
// flash в save()) срывает тайминги SoftDevice и приводит к сбросу МК.
// Поэтому колбэк только копирует отчёт в ring-buffer и сразу возвращает
// управление; разбор/фильтрация/record() делаются в loop().
#define SCAN_RAW_MAX 31
struct ScanReport {
    uint8_t peerMac[6];      // в порядке отображения (addr[5-i])
    uint8_t payload[16];
    uint8_t payloadLen;
    uint8_t type;
    int8_t  rssi;
    bool    parsed;          // прошёл parseMetAdv (Met-реклама)
    uint8_t raw[SCAN_RAW_MAX]; // DIAG: сырые данные рекламы
    uint8_t rawLen;
};
#define SCAN_QUEUE_SIZE 16
static ScanReport         s_scanQueue[SCAN_QUEUE_SIZE];
static volatile uint8_t   s_scanHead = 0;
static volatile uint8_t   s_scanTail = 0;
static volatile uint32_t  s_scanDropped = 0;
static uint32_t           s_rawDumped = 0;  // DIAG: сколько отчётов уже выгрузили
void BLEModule::markAdvStopped() { advRunning = false; }

// companyID(2,LE) | 'M' 'e' 't' | ver | type | payload
static bool parseMetAdv(const uint8_t* data, uint8_t len,
                        uint8_t* type, uint8_t* payload, uint8_t* payloadLen) {
    int i = 0;
    while (i < len) {
        uint8_t fieldLen = data[i];
        if (fieldLen == 0) break;
        if (i + 1 + fieldLen > len) break;
        uint8_t adType = data[i + 1];
        const uint8_t* val = &data[i + 2];
        int valLen = fieldLen - 1;
        if (adType == BLE_GAP_AD_TYPE_MANUFACTURER_SPECIFIC_DATA && valLen >= 7) {
            if (val[2] == 'M' && val[3] == 'e' && val[4] == 't') {
                if (type) *type = val[6];
                // Заголовок производителя = 7 байт: company(2) + "Met"(3)
                // + ver(1) + type(1). Раньше вычиталось 6, из-за чего
                // payload читался на 1 байт длиннее (хвостовой мусор в
                // devNo: MET-00001H/R/z).
                uint8_t plen = (uint8_t)(valLen - 7);
                if (payload && payloadLen) {
                    if (plen > *payloadLen) plen = *payloadLen;
                    memcpy(payload, &val[7], plen);
                    *payloadLen = plen;
                }
                return true;
            }
        }
        i += 1 + fieldLen;
    }
    return false;
}

static void onConnect(uint16_t conn_handle) {
    LOG("BLE connected: handle=%u\n", conn_handle);
    // Останавливаем скан на время соединения: реклама+скан+линк
    // одновременно жгут батарею и мешают друг другу.
    Bluefruit.Scanner.stop();
    // SoftDevice сам останавливает рекламу при подключении, но наш флаг
    // остаётся true — тогда buildAdvertising() вызовет stop() повторно и
    // получит assert. Сбрасываем флаг заранее.
    BLEModule::markAdvStopped();
}

static void onDisconnect(uint16_t conn_handle, uint8_t reason) {
    LOG("BLE disconnected: handle=%u reason=0x%02X\n", conn_handle, reason);
    // Сбрасываем RAM-журнал на диск, пока есть питание.
    StorageModule::flush();
    // Сброс сессии: иначе следующий телефон остаётся "владельцем"
    // (LOG_GET/UNPAIR без повторного AUTH). Также чистит TX-очередь.
    BLEUartModule::resetSession();
    // ВАЖНО: ничего радио-связанного здесь не делаем. Этот колбэк
    // выполняется в контексте события SoftDevice; запуск скана и рекламы
    // прямо в нём (вместе с restartOnDisconnect) приводил к тому, что скан
    // «running», но отчётов нет (raw=0), и к сбросам МК. Откладываем в loop().
    BLEModule::requestRadioRestart();
}

void BLEModule::requestRadioRestart() { s_radioRestartPending = true; }

void BLEModule::begin() {
    // Bluefruit.begin() уже вызван в setup() до регистрации колбэков.
    Bluefruit.setTxPower(KeystoreModule::advTxPowerDbm());
    Bluefruit.Periph.setConnectCallback(onConnect);
    Bluefruit.Periph.setDisconnectCallback(onDisconnect);

#if ENABLE_BLE_SCAN
    // Скан нужен только привязанному брелоку с валидным временем: он ищет
    // встреченные маячки и пишет журнал. Пока провижининга нет (pairing
    // beacon), скан только отбирает эфир у рекламы (~90% duty) и мешает ПК
    // найти брелок — поэтому до провижининга его НЕ запускаем. НО колбэк,
    // интервал и active-scan настраиваем ВСЕГДА: иначе после TIME_SET скан
    // стартует (onDisconnect) без rx-callback и не видит ни одного пакета.
    {
        uint16_t period = (uint16_t)(KeystoreModule::scanPeriodMs() / 0.625f);
        uint16_t window = (uint16_t)(KeystoreModule::scanWindowMs() / 0.625f);
        Bluefruit.Scanner.setRxCallback(scanCallback);
        Bluefruit.Scanner.restartOnDisconnect(false);
        Bluefruit.Scanner.setInterval(period, window);
        Bluefruit.Scanner.useActiveScan(false);
    }
    if (KeystoreModule::provisioned() && KeystoreModule::timeValid()) {
        Bluefruit.Scanner.start(0);
        LOGLN("BLE scan started.");
    } else {
        LOGLN("BLE scan configured, not started (no time yet).");
    }
#else
    LOGLN("BLE scan DISABLED (power test build).");
#endif

    // Рекламу после отключения поднимаем сами в loop() (requestRadioRestart),
    // чтобы не стартовать её в контексте события SoftDevice одновременно со
    // сканом — это и вызывало рассинхрон и сбросы.
    Bluefruit.Advertising.restartOnDisconnect(false);

    updateAnonId();
    buildAdvertising();
}

void BLEModule::loop() {
    // Разбираем накопленные сканером отчёты (HMAC/журнал/лог) ЗДЕСЬ, в
    // основном контексте, а не в ISR-колбэке SoftDevice.
    processScanQueue();

    // Отложенный перезапуск радио после отключения телефона.
    if (s_radioRestartPending && !Bluefruit.connected()) {
        s_radioRestartPending = false;
        if (KeystoreModule::provisioned() && KeystoreModule::timeValid() &&
            !Bluefruit.Scanner.isRunning()) {
            Bluefruit.Scanner.start(0);
            LOGLN("BLE scan (re)started after disconnect.");
        }
        restartAdvertising();
    }
    // Смену MAC/рекламы на ходу не делаем при активном соединении:
    // Bluefruit.setAddr() в этом случае рвёт линк. Отложим до отключения.
    if (Bluefruit.connected()) return;
    if (KeystoreModule::paired() && KeystoreModule::timeValid()) {
        uint32_t epoch = KeystoreModule::utc() / ANON_EPOCH_SECONDS;
        if (epoch != lastEpoch) {
            LOG("Anon epoch %u -> %u: rotating ID/MAC\n",
                (unsigned)lastEpoch, (unsigned)epoch);
            updateAnonId();
            restartAdvertising();
        }
    }
    // Heartbeat раз в ~5 с: если он пропадает — МК завис/сбросился, а не
    // «просто ничего не услышал». uptime показывает, сбрасывалось ли время
    // (uptime мал после сброса) без ожидания BOOT-блока.
    static uint32_t s_lastBeat = 0;
    if (millis() - s_lastBeat > 5000) {
        s_lastBeat = millis();
        // DIAG: heap free/total + a stack sentinel via local address. A steadily
        // falling free heap before the reset points to a leak (Strings/queue).
        LOG("HB up=%lus conn=%u scanRun=%u advRun=%u heap=%u/%u\n",
            (unsigned long)(millis() / 1000),
            (unsigned)Bluefruit.connected(),
            (unsigned)Bluefruit.Scanner.isRunning(), (unsigned)advRunning,
            (unsigned)dbgHeapFree(), (unsigned)dbgHeapTotal());
    }
    // Диагностика раз в ~30 с: сколько Met-ADV услышано и записано.
    static uint32_t s_lastStat = 0;
    if (millis() - s_lastStat > 30000) {
        s_lastStat = millis();
        LOG("SCAN stat: seen=%u recorded=%u enc=%d raw=%u drop=%u scanRun=%u advRun=%u\n",
            (unsigned)s_advSeen, (unsigned)s_advRecorded, StorageModule::count(),
            (unsigned)s_rawReports, (unsigned)s_scanDropped,
            (unsigned)Bluefruit.Scanner.isRunning(),
            (unsigned)advRunning);
    }
}

void BLEModule::rotateMac() {
    // DIAG: маркер входа. Если последний лог перед сбросом — это "rotateMac",
    // значит виновата последовательность stop-scan/set-addr/start-scan/adv.
    LOG("RotateMAC: enter (scanRun=%u advRun=%u)\n",
        (unsigned)Bluefruit.Scanner.isRunning(), (unsigned)advRunning);
    uint8_t rnd[6];
    if (!CryptoModule::random(rnd, 6)) return;

    // sd_ble_gap_addr_set() принимает ТОЛЬКО PUBLIC или RANDOM_STATIC
    // (см. ble_gap.h: sd_ble_gap_addr_set). Тип RANDOM_PRIVATE_NON_RESOLVABLE
    // (0x03) отвергается с BLE_ERROR_GAP_INVALID_BLE_ADDR (0x3202) — приватный
    // адрес задаётся через sd_ble_gap_privacy_set, а не set_addr. Поэтому
    // используем rotating RANDOM_STATIC: старшие два бита старшего байта
    // (addr[5] = rnd[0], т.к. addr[] в LSB-формате) = 0b11.
    rnd[0] = (rnd[0] & 0x3F) | 0xC0;

    // SoftDevice запрещает менять identity-адрес, пока активны реклама,
    // СКАН ИЛИ соединение (sd_ble_gap_addr_set -> NRF_ERROR_INVALID_STATE,
    // 0x08; см. ble_gap.h). У привязанного брелока с валидным временем
    // скан постоянно идёт -> одного Advertising.stop() мало. Глушим и скан,
    // меняем адрес, потом поднимаем всё обратно.
    bool wasScanning = Bluefruit.Scanner.isRunning();
    if (advRunning) { Bluefruit.Advertising.stop(); advRunning = false; }
    if (wasScanning) Bluefruit.Scanner.stop();

    ble_gap_addr_t addr;
    addr.addr_type = BLE_GAP_ADDR_TYPE_RANDOM_STATIC;
    for (int i = 0; i < 6; i++) addr.addr[i] = rnd[5 - i];

    uint32_t st = sd_ble_gap_addr_set(&addr);
    if (wasScanning) Bluefruit.Scanner.start(0);
    if (st != NRF_SUCCESS) {
        LOG("RotateMAC: sd_ble_gap_addr_set failed st=0x%08X\n", (unsigned)st);
        return;
    }
    memcpy(currentMac, rnd, 6);
    LOG("Rotated MAC %02X:%02X:%02X:%02X:%02X:%02X\n",
        currentMac[0], currentMac[1], currentMac[2],
        currentMac[3], currentMac[4], currentMac[5]);
}

void BLEModule::updateAnonId() {
    if (!KeystoreModule::paired() || !KeystoreModule::timeValid()) return;
    uint8_t kDev[16];
    KeystoreModule::getKDev(kDev);
    uint32_t epoch = KeystoreModule::utc() / ANON_EPOCH_SECONDS;
    if (!CryptoModule::anonId(kDev, epoch, anonId)) {
        LOGLN("Anon ID: HMAC failed");
        return;
    }
    lastEpoch = epoch;
    LOG("Anon ID epoch=%u %02X%02X%02X%02X...\n", (unsigned)epoch,
        anonId[0], anonId[1], anonId[2], anonId[3]);
    rotateMac();
}

void BLEModule::buildAdvertising() {
    // stop() только если реклама реально запущена: повторный stop при
    // незапущенной рекламе даёт assert и окна невидимости.
    if (advRunning) { Bluefruit.Advertising.stop(); advRunning = false; }
    Bluefruit.Advertising.clearData();
    Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);

    uint8_t buf[31];
    size_t n = 0;
    buf[n++] = MET_COMPANY_ID & 0xFF;
    buf[n++] = (MET_COMPANY_ID >> 8) & 0xFF;
    buf[n++] = 'M'; buf[n++] = 'e'; buf[n++] = 't';
    buf[n++] = MET_ADV_VERSION;

    if (KeystoreModule::paired() && KeystoreModule::timeValid()) {
        LOGLN("ADV build: anonymous ID");
        buf[n++] = MET_PAYLOAD_ANON;
        memcpy(&buf[n], anonId, 16); n += 16;
    } else {
        // Нет анонимного ID (ещё не привязан ИЛИ привязан, но время от
        // телефона не пришло после ребута): рекламируем pairing beacon с
        // номером устройства, чтобы брелок оставался обнаружимым для --list
        // и повторного провижининга. Раньше в этом случае ADV молчал, из-за
        // чего после перезагрузки привязанный брелок пропадал из эфира.
        // Приватность: devNo виден в эфире, но PROVISION всё равно
        // заблокирован (provisioned()/paired()) — перепривязка невозможна.
        LOG("ADV build: pairing beacon dev=%s (paired=%u timeok=%u)\n",
            KeystoreModule::devNo(), KeystoreModule::paired(),
            KeystoreModule::timeValid());
        buf[n++] = MET_PAYLOAD_PAIR;
        const char* dn = KeystoreModule::devNo();
        size_t dl = strlen(dn);
        if (dl > 16) dl = 16;
        memcpy(&buf[n], dn, dl); n += dl;
    }

    Bluefruit.Advertising.addManufacturerData(buf, n);
    uint16_t units = (uint16_t)(KeystoreModule::advIntervalMs() / 0.625f);
    Bluefruit.Advertising.setInterval(units, units);
    Bluefruit.Advertising.start(0);
    advRunning = true;
    LOG("ADV start: %u bytes, interval=%ums, tx=%ddBm\n",
        (unsigned)n, KeystoreModule::advIntervalMs(),
        KeystoreModule::advTxPowerDbm());
}

void BLEModule::restartAdvertising() {
    Bluefruit.setTxPower(KeystoreModule::advTxPowerDbm());
    buildAdvertising();
}

uint32_t BLEModule::advSeen()     { return s_advSeen; }
uint32_t BLEModule::advRecorded() { return s_advRecorded; }

void BLEModule::scanCallback(ble_gap_evt_adv_report_t* report) {
    s_rawReports++;
    // ВАЖНО: этот колбэк работает в контексте прерывания SoftDevice.
    // Никакого Serial/LOG, никакой криптографии и записи во flash здесь
    // быть не должно — только разбор рекламы (чистые вычисления) и
    // копирование в ring-buffer. Вся тяжёлая работа — в processScanQueue().
    uint8_t type = 0;
    uint8_t payload[24];
    uint8_t payloadLen = sizeof(payload);

    bool ok = parseMetAdv(report->data.p_data, report->data.len,
                          &type, payload, &payloadLen);

    uint8_t head = s_scanHead;
    uint8_t next = (uint8_t)((head + 1) % SCAN_QUEUE_SIZE);
    if (next != s_scanTail) {
        ScanReport& r = s_scanQueue[head];
        for (int i = 0; i < 6; i++) r.peerMac[i] = report->peer_addr.addr[5 - i];
        r.rssi = report->rssi;
        r.parsed = ok;
        r.type = type;
        uint8_t pl = ok ? payloadLen : 0;
        if (pl > sizeof(r.payload)) pl = sizeof(r.payload);
        if (ok) memcpy(r.payload, payload, pl);
        r.payloadLen = pl;
        // DIAG: копируем сырые данные рекламы для выгрузки в loop().
        uint8_t rl = report->data.len;
        if (rl > SCAN_RAW_MAX) rl = SCAN_RAW_MAX;
        memcpy(r.raw, report->data.p_data, rl);
        r.rawLen = rl;
        __DMB();              // данные записаны до публикации head
        s_scanHead = next;
        if (ok) s_advSeen++;
    } else {
        s_scanDropped++;      // очередь переполнена — отчёт теряется
    }
    // SoftDevice останавливает скан после каждого отчёта — продолжаем.
    Bluefruit.Scanner.resume();
}

void BLEModule::processScanQueue() {
    while (s_scanTail != s_scanHead) {
        ScanReport r = s_scanQueue[s_scanTail];
        s_scanTail = (uint8_t)((s_scanTail + 1) % SCAN_QUEUE_SIZE);

        // DIAG: первые 40 отчётов выгружаем целиком (hex) — понять, что
        // реально в эфире и почему seen=0 при raw>0.
        if (s_rawDumped < 40) {
            s_rawDumped++;
            char hex[3 * SCAN_RAW_MAX + 1];
            size_t n = 0;
            for (uint8_t i = 0; i < r.rawLen; i++)
                n += snprintf(hex + n, sizeof(hex) - n, "%02X ", r.raw[i]);
            // NEARMISS: в сырых данных есть сигнатура 'M''e''t', но парсер
            // не признал её (проблема формата), в отличие от «нет в эфире».
            bool metBytes = false;
            for (uint8_t i = 0; i + 4 < r.rawLen; i++)
                if (r.raw[i] == 'M' && r.raw[i+1] == 'e' && r.raw[i+2] == 't') {
                    metBytes = true; break;
                }
            LOG("ScanRaw #%u rssi=%d len=%u parsed=%u metBytes=%u: %s\n",
                (unsigned)s_rawDumped, (int)r.rssi, (unsigned)r.rawLen,
                (unsigned)r.parsed, (unsigned)metBytes, hex);
        }

        if (!r.parsed) continue;
        if (r.rssi < KeystoreModule::rssiThreshold()) continue;
        if (memcmp(r.peerMac, currentMac, 6) == 0) continue;

        if (r.type == MET_PAYLOAD_ANON && r.payloadLen >= 16 &&
            KeystoreModule::timeValid()) {
            LOG("Scan: anon id rssi=%d, recording\n", r.rssi);
            StorageModule::record(KeystoreModule::utc(), r.payload, r.rssi);
            s_advRecorded++;
        } else if (r.type == MET_PAYLOAD_PAIR) {
            char dn[17];
            uint8_t dl = r.payloadLen; if (dl > 16) dl = 16;
            memcpy(dn, r.payload, dl); dn[dl] = '\0';
            LOG("Scan: pair beacon dev=%s rssi=%d, ignored\n", dn, r.rssi);
        }
    }
}
