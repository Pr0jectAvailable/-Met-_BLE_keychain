#include "ble_module.h"
#include "config.h"
#include "crypto_module.h"
#include "keystore_module.h"
#include "storage_module.h"

uint8_t  BLEModule::currentMac[6] = {0};
uint8_t  BLEModule::anonId[16] = {0};
uint32_t BLEModule::lastEpoch = 0;
bool     BLEModule::advRunning = false;

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
                uint8_t plen = (uint8_t)(valLen - 6);
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
}

static void onDisconnect(uint16_t conn_handle, uint8_t reason) {
    LOG("BLE disconnected: handle=%u reason=0x%02X\n", conn_handle, reason);
}

void BLEModule::begin() {
    // Bluefruit.begin() уже вызван в setup() до регистрации колбэков.
    Bluefruit.setTxPower(KeystoreModule::advTxPowerDbm());
    Bluefruit.Periph.setConnectCallback(onConnect);
    Bluefruit.Periph.setDisconnectCallback(onDisconnect);

    uint16_t period = (uint16_t)(KeystoreModule::scanPeriodMs() / 0.625f);
    uint16_t window = (uint16_t)(KeystoreModule::scanWindowMs() / 0.625f);
    LOG("BLE scan: window=%ums period=%ums (units %u/%u), rssiThr=%d dBm\n",
        KeystoreModule::scanWindowMs(), KeystoreModule::scanPeriodMs(),
        window, period, KeystoreModule::rssiThreshold());
    Bluefruit.Scanner.setRxCallback(scanCallback);
    Bluefruit.Scanner.restartOnDisconnect(false);
    Bluefruit.Scanner.setInterval(period, window);
    Bluefruit.Scanner.useActiveScan(false);
    Bluefruit.Scanner.start(0);
    LOGLN("BLE scan started.");

    Bluefruit.Advertising.restartOnDisconnect(true);

    updateAnonId();
    buildAdvertising();
}

void BLEModule::loop() {
    if (KeystoreModule::paired() && KeystoreModule::timeValid()) {
        uint32_t epoch = KeystoreModule::utc() / ANON_EPOCH_SECONDS;
        if (epoch != lastEpoch) {
            LOG("Anon epoch %u -> %u: rotating ID/MAC\n",
                (unsigned)lastEpoch, (unsigned)epoch);
            updateAnonId();
            restartAdvertising();
        }
    }
}

void BLEModule::rotateMac() {
    uint8_t rnd[6];
    if (!CryptoModule::random(rnd, 6)) return;
    rnd[5] &= 0x3F;   // non-resolvable private address
    ble_gap_addr_t addr;
    addr.addr_type = BLE_GAP_ADDR_TYPE_RANDOM_PRIVATE_NON_RESOLVABLE;
    for (int i = 0; i < 6; i++) addr.addr[i] = rnd[5 - i];
    Bluefruit.setAddr(&addr);
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
    Bluefruit.Advertising.stop();
    advRunning = false;
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

void BLEModule::scanCallback(ble_gap_evt_adv_report_t* report) {
    uint8_t type = 0;
    uint8_t payload[24];
    uint8_t payloadLen = sizeof(payload);

    if (!parseMetAdv(report->data.p_data, report->data.len,
                     &type, payload, &payloadLen)) {
        Bluefruit.Scanner.resume();
        return;
    }

    int8_t rssi = report->rssi;
    if (rssi < KeystoreModule::rssiThreshold()) {
        LOG("Scan: Met rssi=%d < thr=%d, skip\n", rssi, KeystoreModule::rssiThreshold());
        Bluefruit.Scanner.resume();
        return;
    }

    uint8_t mac[6];
    for (int i = 0; i < 6; i++) mac[i] = report->peer_addr.addr[5 - i];
    if (memcmp(mac, currentMac, 6) == 0) {
        LOGLN("Scan: own adv, skip");
        Bluefruit.Scanner.resume();
        return;
    }

    if (type == MET_PAYLOAD_ANON && payloadLen >= 16 && KeystoreModule::timeValid()) {
        LOG("Scan: anon id rssi=%d, recording\n", rssi);
        StorageModule::record(KeystoreModule::utc(), payload, rssi);
    } else if (type == MET_PAYLOAD_PAIR) {
        LOG("Scan: pair beacon rssi=%d, ignored\n", rssi);
    }
    Bluefruit.Scanner.resume();
}
