#include "storage_module.h"
#include "config.h"
#include "crypto_module.h"
#include "keystore_module.h"
#include <InternalFileSystem.h>

using namespace Adafruit_LittleFS_Namespace;

const char* StorageModule::ENC_FILE = "/encounters.dat";
const char* StorageModule::TMP_FILE = "/encounters.tmp";
const uint32_t StorageModule::ENC_MAGIC;
const uint16_t StorageModule::ENC_FORMAT_VERSION;
Encounter StorageModule::records[StorageModule::MAX_ENCOUNTERS];
int StorageModule::recCount = 0;
volatile bool StorageModule::dirty = false;
uint32_t StorageModule::lastDirtyMs = 0;

static const uint32_t SAVE_DEBOUNCE_MS = 5000;

// CRC32 (IEEE 802.3) для контроля целостности файла журнала.
static uint32_t crc32(const uint8_t* data, size_t len) {
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1)));
    }
    return ~crc;
}

void StorageModule::begin() { load(); }

void StorageModule::loop() {
    if (dirty && (millis() - lastDirtyMs) > SAVE_DEBOUNCE_MS) {
        save();
        dirty = false;
    }
}

// tag = HMAC-SHA256(K_log, devNo ‖ seq ‖ id ‖ tFirst ‖ tLast ‖ rssi ‖ count)[0..15]
static bool computeTag(const char* devNo, uint32_t seq, const uint8_t id[16],
                       uint32_t tFirst, uint32_t tLast, int8_t rssi,
                       uint16_t count, uint8_t out[16]) {
    uint8_t kLog[32];
    if (!KeystoreModule::deriveLog(kLog)) return false;
    uint8_t buf[16 + 4 + 16 + 4 + 4 + 1 + 2];
    size_t n = 0;
    size_t dn = strlen(devNo); if (dn > 16) dn = 16;
    memcpy(buf + n, devNo, dn); n += dn;
    buf[n++] = (uint8_t)(seq >> 24); buf[n++] = (uint8_t)(seq >> 16);
    buf[n++] = (uint8_t)(seq >> 8);  buf[n++] = (uint8_t)seq;
    memcpy(buf + n, id, 16); n += 16;
    buf[n++] = (uint8_t)(tFirst >> 24); buf[n++] = (uint8_t)(tFirst >> 16);
    buf[n++] = (uint8_t)(tFirst >> 8);  buf[n++] = (uint8_t)tFirst;
    buf[n++] = (uint8_t)(tLast >> 24);  buf[n++] = (uint8_t)(tLast >> 16);
    buf[n++] = (uint8_t)(tLast >> 8);   buf[n++] = (uint8_t)tLast;
    buf[n++] = (uint8_t)rssi;
    buf[n++] = (uint8_t)(count >> 8);   buf[n++] = (uint8_t)count;
    uint8_t mac[32];
    if (!CryptoModule::hmacSha256(kLog, 32, buf, n, mac)) return false;
    memcpy(out, mac, 16);
    return true;
}

bool StorageModule::record(uint32_t nowUtc, const uint8_t id[16], int8_t rssi) {
    for (int i = 0; i < recCount; i++) {
        if (memcmp(records[i].id, id, 16) == 0) {
            // Окно склейки считаем от tLast (последней встречи), а не от
            // tFirst: иначе долго слышимый сосед дробится каждые 2 минуты.
            // Знаковая разность защищает от шага RTC назад.
            int32_t dtLast = (int32_t)(nowUtc - records[i].tLast);
            if (dtLast >= 0 && dtLast <= (ENCOUNTER_MERGE_MS / 1000)) {
                records[i].tLast = nowUtc;
                if (rssi > records[i].rssi) records[i].rssi = rssi;
                if (records[i].count < 0xFFFF) records[i].count++;
                // Пересчёт тега: сервер проверяет HMAC по текущим полям,
                // без этого обновлённые tLast/rssi/count отклоняются.
                if (!computeTag(KeystoreModule::devNo(), records[i].seq,
                                records[i].id, records[i].tFirst,
                                records[i].tLast, records[i].rssi,
                                records[i].count, records[i].tag)) {
                    memset(records[i].tag, 0, 16);
                }
                dirty = true;
                lastDirtyMs = millis();
                LOG("Encounter merge slot=%d seq=%u tLast=%u rssi=%d count=%u\n",
                    i, (unsigned)records[i].seq, (unsigned)nowUtc,
                    records[i].rssi, records[i].count);
                return true;
            }
            LOG("Encounter id seen but outside merge window -> new record\n");
        }
    }
    int slot;
    if (recCount < MAX_ENCOUNTERS) {
        slot = recCount++;
    } else {
        slot = 0;
        for (int i = 1; i < recCount; i++)
            if ((int32_t)(records[i].tFirst - records[slot].tFirst) < 0) slot = i;
    }
    Encounter& e = records[slot];
    memcpy(e.id, id, 16);
    e.tFirst = nowUtc;
    e.tLast = nowUtc;
    e.rssi = rssi;
    e.count = 1;
    e.seq = KeystoreModule::nextSeq();
    if (!computeTag(KeystoreModule::devNo(), e.seq, e.id, e.tFirst, e.tLast,
                    e.rssi, e.count, e.tag)) {
        memset(e.tag, 0, 16);
    }
    dirty = true;
    lastDirtyMs = millis();
    LOG("Encounter new slot=%d seq=%u t=%u rssi=%d count=%u\n",
        slot, (unsigned)e.seq, (unsigned)nowUtc, rssi, e.count);
    return true;
}

void StorageModule::clear() { recCount = 0; save(); dirty = false; }
int StorageModule::count() { return recCount; }

bool StorageModule::get(int index, Encounter& out) {
    if (index < 0 || index >= recCount) return false;
    out = records[index];
    return true;
}

int StorageModule::snapshot(Encounter* out, int maxCount) {
    if (!out || maxCount <= 0) return 0;
    int n = recCount; if (n > maxCount) n = maxCount;
    memcpy(out, records, sizeof(Encounter) * n);
    return n;
}

int StorageModule::ackUpTo(uint32_t upto) {
    int removed = 0, w = 0;
    for (int i = 0; i < recCount; i++) {
        // Знаковая разность: корректно работает при переполнении nextSeq().
        if ((int32_t)(upto - records[i].seq) >= 0) { removed++; }
        else { if (w != i) records[w] = records[i]; w++; }
    }
    recCount = w;
    if (removed) { save(); dirty = false; }
    LOG("Journal ack<=%u: removed=%d remaining=%d\n",
        (unsigned)upto, removed, recCount);
    return removed;
}

void StorageModule::flush() { if (dirty) { save(); dirty = false; } }

void StorageModule::load() {
    recCount = 0; dirty = false;
    File file = InternalFS.open(ENC_FILE, FILE_O_READ);
    if (!file) { LOGLN("Journal: no file yet."); return; }
    uint32_t magic = 0; uint16_t version = 0; uint32_t crc = 0;
    file.read((uint8_t*)&magic, sizeof(magic));
    file.read((uint8_t*)&version, sizeof(version));
    file.read((uint8_t*)&crc, sizeof(crc));
    if (magic != ENC_MAGIC || version != ENC_FORMAT_VERSION) {
        LOGLN("Journal: bad header -> reset.");
        file.close();
        InternalFS.remove(ENC_FILE);
        return;
    }
    int raw = file.read((uint8_t*)records, sizeof(Encounter) * MAX_ENCOUNTERS);
    file.close();
    int n = raw / (int)sizeof(Encounter);
    if (n < 0) n = 0;
    if (n > MAX_ENCOUNTERS) n = MAX_ENCOUNTERS;
    size_t dataLen = sizeof(Encounter) * n;
    if (crc32((const uint8_t*)records, dataLen) != crc) {
        LOGLN("Journal: CRC mismatch -> reset.");
        recCount = 0;
        InternalFS.remove(ENC_FILE);
        return;
    }
    recCount = n;
    LOG("Journal: loaded %d encounters (ver=%u).\n",
        recCount, (unsigned)version);
}

void StorageModule::save() {
    // Атомарная запись: сначала во временный файл + CRC, затем переименование.
    // Пропадание питания больше не оставляет пустой/битый журнал.
    size_t dataLen = sizeof(Encounter) * recCount;
    uint32_t crc = crc32((const uint8_t*)records, dataLen);

    InternalFS.remove(TMP_FILE);
    File file = InternalFS.open(TMP_FILE, FILE_O_WRITE);
    if (!file) {
        LOGLN("Journal: save open failed!");
        return;
    }
    file.write((uint8_t*)&ENC_MAGIC, sizeof(ENC_MAGIC));
    file.write((uint8_t*)&ENC_FORMAT_VERSION, sizeof(ENC_FORMAT_VERSION));
    file.write((uint8_t*)&crc, sizeof(crc));
    file.write((uint8_t*)records, dataLen);
    file.close();

    InternalFS.remove(ENC_FILE);
    if (!InternalFS.rename(TMP_FILE, ENC_FILE)) {
        LOGLN("Journal: rename failed!");
        return;
    }
    LOG("Journal: saved %d encounters.\n", recCount);
}
