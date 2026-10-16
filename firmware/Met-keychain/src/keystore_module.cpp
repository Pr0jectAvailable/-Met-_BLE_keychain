#include "keystore_module.h"
#include "config.h"
#include "crypto_module.h"
#include <InternalFileSystem.h>

using namespace Adafruit_LittleFS_Namespace;

const char* KeystoreModule::KEY_FILE = "/keys.dat";
const char* KeystoreModule::ALT_FILE = "/keys.alt";
uint8_t  KeystoreModule::s_activeSlot = 0;
uint32_t KeystoreModule::s_gen = 0;

static const uint32_t KEY_MAGIC = 0x4D45544B; // 'METK'
static const uint16_t KEY_FORMAT_VERSION = 1;

#pragma pack(push, 1)
struct KeyRecord {
    uint32_t magic;
    uint16_t version;
    char     devNo[16];
    uint8_t  kDev[16];
    uint8_t  kPair[16];
    uint8_t  kOwner[16];
    uint8_t  flags;
    uint32_t seq;
    uint32_t utc;
    uint16_t advIntervalMs;
    uint16_t scanWindowMs;
    uint16_t scanPeriodMs;
    int8_t   advTxPowerDbm;
    int8_t   rssiThreshold;
};
#pragma pack(pop)

static KeyRecord s_rec;

// Текущий счётчик seq живёт в RAM; s_rec.seq — зарезервированная «верхняя
// граница». save() вызывается только когда RAM-счётчик доходит до границы,
// поэтому на каждую встречу flash больше не пишется (см. firmware-fixes.md
// P0-4). После сброса пропускаем до 32 номеров — серверу нужна только
// монотонность. Формат файла не меняется: граница хранится в s_rec.seq.
static uint32_t s_seq = 0;

// CRC32 (IEEE 802.3) для контроля целостности слота ключей.
static uint32_t ks_crc32(const uint8_t* data, size_t len) {
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1)));
    }
    return ~crc;
}

// Читает один слот. Формат: [KeyRecord][gen u32][crc u32]; crc считается по
// KeyRecord+gen. Старый однофайловый формат (только KeyRecord) принимается
// как gen=0, чтобы не терять ключи уже провизионенных устройств.
static bool readSlot(const char* path, KeyRecord& out, uint32_t& gen) {
    File file = InternalFS.open(path, FILE_O_READ);
    if (!file) return false;
    uint8_t buf[sizeof(KeyRecord) + 8];
    int rd = file.read(buf, sizeof(buf));
    file.close();
    if (rd < (int)sizeof(KeyRecord)) return false;
    KeyRecord* r = (KeyRecord*)buf;
    if (r->magic != KEY_MAGIC || r->version != KEY_FORMAT_VERSION) return false;
    if (rd >= (int)sizeof(buf)) {
        uint32_t g, c;
        memcpy(&g, buf + sizeof(KeyRecord), 4);
        memcpy(&c, buf + sizeof(KeyRecord) + 4, 4);
        if (ks_crc32(buf, sizeof(KeyRecord) + 4) != c) return false;
        gen = g;
    } else {
        gen = 0;
    }
    out = *r;
    return true;
}

// Время только в RAM (не сохраняется): после перезагрузки брелок молчит,
// пока телефон снова не поставит точное время.
volatile bool     KeystoreModule::s_timeValid = false;
volatile uint32_t KeystoreModule::s_utcBase = 0;
volatile uint32_t KeystoreModule::s_utcBaseMs = 0;

static void setDefaults() {
    memset(&s_rec, 0, sizeof(s_rec));
    s_rec.magic = KEY_MAGIC;
    s_rec.version = KEY_FORMAT_VERSION;
    strncpy(s_rec.devNo, "MET-00000", sizeof(s_rec.devNo) - 1);
    s_rec.flags = 0;
    s_rec.seq = 0;
    s_rec.utc = 0;
    s_rec.advIntervalMs = ADV_INTERVAL_MS_DEFAULT;
    s_rec.scanWindowMs = SCAN_WINDOW_MS_DEFAULT;
    s_rec.scanPeriodMs = SCAN_PERIOD_MS_DEFAULT;
    s_rec.advTxPowerDbm = ADV_TX_POWER_DBM_DEFAULT;
    s_rec.rssiThreshold = RSSI_THRESHOLD_DEFAULT;
}

void KeystoreModule::begin() { load(); }

void KeystoreModule::load() {
    setDefaults();
    // Читаем оба слота и берём валидный с наибольшим поколением. Благодаря
    // этому незавершённая запись в один слот не уничтожает ключи: второй
    // слот остаётся валидным фолбэком (см. save()).
    KeyRecord r0, r1; uint32_t g0 = 0, g1 = 0;
    bool ok0 = readSlot(KEY_FILE, r0, g0);
    bool ok1 = readSlot(ALT_FILE, r1, g1);
    if (!ok0 && !ok1) {
        LOGLN("Keystore: no keys file.");
        s_activeSlot = 0; s_gen = 0;
        return;
    }
    if (ok0 && (!ok1 || g0 >= g1)) { s_rec = r0; s_activeSlot = 0; s_gen = g0; }
    else                           { s_rec = r1; s_activeSlot = 1; s_gen = g1; }
    s_rec.devNo[sizeof(s_rec.devNo) - 1] = '\0';
    // Возобновляем счёт с сохранённой границы резерва: номера ниже неё могли
    // быть выданы до сброса, повторно их не используем.
    s_seq = s_rec.seq;
    LOG("Keystore: devNo=%s paired=%u seq=%u gen=%u slot=%u\n", s_rec.devNo,
        (unsigned)(s_rec.flags & 1), (unsigned)s_rec.seq,
        (unsigned)s_gen, (unsigned)s_activeSlot);

    // Разовая миграция радио-параметров на "поисковые" значения, если они
    // всё ещё равны старым дефолтам (т.е. не настраивались вручную).
    if (s_rec.advIntervalMs == 500 && s_rec.advTxPowerDbm == 0 &&
        s_rec.scanWindowMs == 600 && s_rec.scanPeriodMs == 10000 &&
        s_rec.rssiThreshold == -75) {
        s_rec.advIntervalMs = ADV_INTERVAL_MS_DEFAULT;
        s_rec.advTxPowerDbm = ADV_TX_POWER_DBM_DEFAULT;
        s_rec.scanWindowMs = SCAN_WINDOW_MS_DEFAULT;
        s_rec.scanPeriodMs = SCAN_PERIOD_MS_DEFAULT;
        s_rec.rssiThreshold = RSSI_THRESHOLD_DEFAULT;
        save();
        LOGLN("Keystore: radio params migrated for better discovery.");
    }

    // Миграция "высоконагруженных" радио-параметров: 90% duty скана (9000/10000)
    // и +4dBm одновременно на USB-питании давали brown-out/сброс (reset_reas=0x0,
    // bootcnt растёт). Понижаем duty и TX, чтобы МК не перезагружался и не
    // терял RAM-часы (иначе timeok=0 -> брелок уходит в pairing beacon).
    if (s_rec.scanWindowMs == 9000 && s_rec.scanPeriodMs == 10000 &&
        s_rec.advTxPowerDbm == 4) {
        s_rec.scanWindowMs = SCAN_WINDOW_MS_DEFAULT;
        s_rec.scanPeriodMs = SCAN_PERIOD_MS_DEFAULT;
        s_rec.advTxPowerDbm = ADV_TX_POWER_DBM_DEFAULT;
        save();
        LOGLN("Keystore: radio params de-rated (scan duty/TX) to stop resets.");
    }
}

void KeystoreModule::save() {
    // Атомарная двухслотовая запись: пишем всегда в НЕактивный слот, а активный
    // остаётся валидным фолбэком. Сброс/пропадание питания в любой момент
    // оставляет хотя бы один целый файл ключей -> nextSeq()/prov() больше не
    // теряет провижн (см. инцидент MET-00001, ребут во время remove+write).
    uint32_t nextGen = s_gen + 1;
    const char* path = (s_activeSlot == 0) ? ALT_FILE : KEY_FILE;
    s_rec.magic = KEY_MAGIC;
    s_rec.version = KEY_FORMAT_VERSION;

    uint8_t buf[sizeof(KeyRecord) + 8];
    memcpy(buf, &s_rec, sizeof(KeyRecord));
    memcpy(buf + sizeof(KeyRecord), &nextGen, 4);
    uint32_t crc = ks_crc32(buf, sizeof(KeyRecord) + 4);
    memcpy(buf + sizeof(KeyRecord) + 4, &crc, 4);

    InternalFS.remove(path);
    File file = InternalFS.open(path, FILE_O_WRITE);
    if (!file) { LOGLN("Keystore: save open failed!"); return; }
    file.write(buf, sizeof(buf));
    file.close();

    s_activeSlot ^= 1;
    s_gen = nextGen;
}

const char* KeystoreModule::devNo() { return s_rec.devNo; }
void KeystoreModule::getKDev(uint8_t out[16])   { memcpy(out, s_rec.kDev, 16); }
void KeystoreModule::getKPair(uint8_t out[16])  { memcpy(out, s_rec.kPair, 16); }
void KeystoreModule::getKOwner(uint8_t out[16]) { memcpy(out, s_rec.kOwner, 16); }

bool KeystoreModule::paired() { return (s_rec.flags & 0x01) != 0; }
void KeystoreModule::setPaired(bool v) {
    LOG("Keystore: setPaired=%u\n", (unsigned)(v ? 1 : 0));
    if (v) s_rec.flags |= 0x01; else s_rec.flags &= ~0x01;
    save();
}
void KeystoreModule::setKOwner(const uint8_t kOwner[16]) {
    LOGLN("Keystore: set K_owner.");
    memcpy(s_rec.kOwner, kOwner, 16); save();
}

bool KeystoreModule::provisioned() {
    for (size_t i = 0; i < 16; i++) if (s_rec.kDev[i] != 0) return true;
    return false;
}

bool KeystoreModule::provision(const char* devNo, const uint8_t kDev[16],
                               const uint8_t kPair[16]) {
    if (provisioned()) { LOGLN("Keystore: already provisioned, refuse."); return false; }
    if (devNo == nullptr || devNo[0] == '\0') return false;
    memset(s_rec.devNo, 0, sizeof(s_rec.devNo));
    strncpy(s_rec.devNo, devNo, sizeof(s_rec.devNo) - 1);
    memcpy(s_rec.kDev, kDev, 16);
    memcpy(s_rec.kPair, kPair, 16);
    memset(s_rec.kOwner, 0, 16);
    s_rec.flags = 0;
    s_rec.seq = 0;
    s_seq = 0;
    save();
    LOG("Keystore: provisioned dev=%s\n", s_rec.devNo);
    return true;
}

bool KeystoreModule::deriveLog(uint8_t out[32]) {
    return CryptoModule::hkdfSha256(s_rec.kDev, 16, "met-log-v1", out, 32);
}
bool KeystoreModule::deriveTime(uint8_t out[32]) {
    return CryptoModule::hkdfSha256(s_rec.kDev, 16, "met-time-v1", out, 32);
}
bool KeystoreModule::deriveRcpt(uint8_t out[32]) {
    return CryptoModule::hkdfSha256(s_rec.kDev, 16, "met-pair-v1", out, 32);
}

uint32_t KeystoreModule::nextSeq() {
    uint32_t s = s_seq++;                 // счётчик — только в RAM
    if (s_seq >= s_rec.seq) {             // резерв исчерпан — пишем следующий блок
        s_rec.seq = s_seq + 32;
        save();
    }
    LOG("Keystore: nextSeq=%u\n", (unsigned)s);
    return s;
}
uint32_t KeystoreModule::peekSeq() { return s_seq; }

bool KeystoreModule::timeValid() { return s_timeValid; }

uint32_t KeystoreModule::utc() {
    if (!s_timeValid) return 0;
    uint32_t elapsed = (millis() - s_utcBaseMs) / 1000;
    return s_utcBase + elapsed;
}

void KeystoreModule::setUtc(uint32_t t) {
    LOG("Keystore: setUtc=%u (valid, RAM only)\n", (unsigned)t);
    s_utcBase = t;
    s_utcBaseMs = millis();
    s_timeValid = true;
}

uint16_t KeystoreModule::advIntervalMs() { return s_rec.advIntervalMs; }
int8_t   KeystoreModule::advTxPowerDbm() { return s_rec.advTxPowerDbm; }
uint16_t KeystoreModule::scanWindowMs()  { return s_rec.scanWindowMs; }
uint16_t KeystoreModule::scanPeriodMs()  { return s_rec.scanPeriodMs; }
int8_t   KeystoreModule::rssiThreshold() { return s_rec.rssiThreshold; }

void KeystoreModule::setAdvIntervalMs(uint16_t v) { s_rec.advIntervalMs = v; save(); }
void KeystoreModule::setAdvTxPowerDbm(int8_t v)   { s_rec.advTxPowerDbm = v; save(); }
void KeystoreModule::setScanWindowMs(uint16_t v)  { s_rec.scanWindowMs = v; save(); }
void KeystoreModule::setScanPeriodMs(uint16_t v)  { s_rec.scanPeriodMs = v; save(); }
void KeystoreModule::setRssiThreshold(int8_t v)   { s_rec.rssiThreshold = v; save(); }

void KeystoreModule::unpair() {
    LOGLN("Keystore: unpair, clearing K_owner.");
    memset(s_rec.kOwner, 0, 16);
    s_rec.flags &= ~0x01;
    save();
}

void KeystoreModule::unbind() {
    LOGLN("Keystore: unbind, clearing owner binding (provision kept).");
    memset(s_rec.kOwner, 0, 16);
    s_rec.flags &= ~0x01;
    save();
    // Время живёт только в RAM: после отвязки брелок должен снова молчать,
    // пока новый владелец не установит время подписанным TIME_SET.
    s_timeValid = false;
    s_utcBase = 0;
    s_utcBaseMs = 0;
}

void KeystoreModule::factoryReset() {
    LOGLN("Keystore: FACTORY RESET, clearing devNo/K_dev/K_pair/K_owner.");
    // Сохраняем только счётчик seq (он монотонно растёт и не сбрасывается)
    // и настроенные параметры радио — их перезапись не нарушает протокол.
    // keepSeq уже является зарезервированной границей — с неё и возобновляем.
    uint32_t keepSeq = s_rec.seq;
    uint16_t advI = s_rec.advIntervalMs;
    uint16_t scanW = s_rec.scanWindowMs;
    uint16_t scanP = s_rec.scanPeriodMs;
    int8_t   advTx = s_rec.advTxPowerDbm;
    int8_t   rssi = s_rec.rssiThreshold;
    setDefaults();
    s_rec.seq = keepSeq;
    s_seq = keepSeq;
    s_rec.advIntervalMs = advI;
    s_rec.scanWindowMs = scanW;
    s_rec.scanPeriodMs = scanP;
    s_rec.advTxPowerDbm = advTx;
    s_rec.rssiThreshold = rssi;
    save();
}
