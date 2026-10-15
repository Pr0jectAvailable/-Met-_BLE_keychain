#include "ble_uart_module.h"
#include "config.h"
#include "crypto_module.h"
#include "keystore_module.h"
#include "storage_module.h"
#include "ble_module.h"

BLEUart BLEUartModule::bleuart;

std::queue<String> BLEUartModule::txQueue;
String BLEUartModule::txCurrent = "";
size_t BLEUartModule::txOffset = 0;
uint32_t BLEUartModule::txRetryMs = 0;

uint16_t BLEUartModule::activeId = 0;
int      BLEUartModule::txDataLines = 0;
uint32_t BLEUartModule::txDeadlineMs = 0;
bool     BLEUartModule::txClosing = false;

static const uint32_t TX_STALL_TIMEOUT_MS = 5000;

// Состояние challenge-response.
static uint8_t  s_challenge[16];
static bool     s_challengeActive = false;
static bool     s_ownerAuthed = false;
static uint8_t  s_authFails = 0;
static uint32_t s_lockoutUntil = 0;

void BLEUartModule::begin() {
    bleuart.begin();
    bleuart.setRxCallback(rxCallback);
    LOGLN("BLE UART ready.");
}

uint16_t BLEUartModule::currentMtu() {
    BLEConnection* conn = Bluefruit.Connection(0);
    if (conn != nullptr && conn->connected()) {
        uint16_t mtu = conn->getMtu();
        if (mtu >= 23) return mtu;
    }
    return 23;
}

void BLEUartModule::loop() {
    txPump();
    if (activeId != 0 &&
        (uint32_t)(millis() - txDeadlineMs) > TX_STALL_TIMEOUT_MS) {
        LOGLN("Transaction watchdog: resetting TX.");
        std::queue<String> empty;
        std::swap(txQueue, empty);
        txCurrent = "";
        txOffset = 0;
        finishTransaction();
    }
}

void BLEUartModule::enqueueResponse(const String& resp) {
    if (resp.length() == 0) return;
    if (txQueue.size() >= TX_QUEUE_MAX) { LOGLN("TX queue full, dropping."); return; }
    txQueue.push(resp);
}

void BLEUartModule::enqueueData(const String& token) {
    if (activeId == 0) return;
    txDataLines++;
    enqueueResponse(String(activeId) + " " + token + "\n");
}

void BLEUartModule::endTransaction() {
    if (activeId == 0) return;
    enqueueResponse(String(activeId) + " END n=" + String(txDataLines) + "\n");
    txClosing = true;
}

void BLEUartModule::errTransaction(const char* code, const char* text) {
    if (activeId == 0) return;
    LOG("TX id=%u ERR %s %s\n", activeId, code, text ? text : "");
    String line = String(activeId) + " ERR " + code;
    if (text != nullptr && text[0] != '\0') { line += " "; line += text; }
    line += "\n";
    enqueueResponse(line);
    txClosing = true;
}

void BLEUartModule::finishTransaction() {
    activeId = 0;
    txDataLines = 0;
    txClosing = false;
}

void BLEUartModule::txPump() {
    if (txCurrent.length() == 0) {
        if (txQueue.empty()) { if (txClosing) finishTransaction(); return; }
        txCurrent = txQueue.front();
        txQueue.pop();
        txOffset = 0;
        txRetryMs = millis();
    }
    uint16_t chunk = currentMtu();
    if (chunk < 23) chunk = 23;
    chunk -= 3;
    size_t remain = txCurrent.length() - txOffset;
    if (remain > chunk) remain = chunk;
    const uint8_t* data = (const uint8_t*)txCurrent.c_str() + txOffset;
    size_t n = bleuart.write(data, remain);
    if (n == 0) {
        if ((uint32_t)(millis() - txRetryMs) > 2000) {
            txCurrent = ""; txOffset = 0; txRetryMs = millis();
        }
        return;
    }
    txOffset += n;
    txRetryMs = millis();
    txDeadlineMs = millis();
    if (txOffset >= txCurrent.length()) {
        txCurrent = ""; txOffset = 0;
        if (txQueue.empty() && txClosing) finishTransaction();
    }
}

void BLEUartModule::rxCallback(uint16_t conn_handle) {
    char buffer[160];
    int len = bleuart.read(buffer, sizeof(buffer) - 1);
    if (len <= 0) return;
    buffer[len] = '\0';
    LOG("RX %d bytes: '%s'\n", len, buffer);
    static String cmdBuffer = "";
    static bool overflow = false;
    for (int i = 0; i < len; i++) {
        char c = buffer[i];
        if (c == '\n' || c == '\r') {
            if (overflow) { if (c == '\n') overflow = false; cmdBuffer = ""; continue; }
            cmdBuffer.trim();
            if (cmdBuffer.length() > 0) {
                LOG("CMD line: '%s'\n", cmdBuffer.c_str());
                processCommand(cmdBuffer);
            }
            cmdBuffer = "";
        } else if (!overflow) {
            cmdBuffer += c;
            if (cmdBuffer.length() > MAX_LINE) {
                cmdBuffer = ""; overflow = true;
                enqueueResponse("0 ERR TOO_LONG line too long\n");
            }
        }
    }
}

// Разбор hex-строки в буфер. Возвращает true при точном совпадении длины.
static bool parseHex(const String& s, uint8_t* out, size_t outLen) {
    if (s.length() != outLen * 2) return false;
    for (size_t i = 0; i < outLen; i++) {
        char hi = s.charAt(i * 2);
        char lo = s.charAt(i * 2 + 1);
        int v = 0;
        for (int k = 0; k < 2; k++) {
            char ch = (k == 0) ? hi : lo;
            int d;
            if (ch >= '0' && ch <= '9') d = ch - '0';
            else if (ch >= 'a' && ch <= 'f') d = ch - 'a' + 10;
            else if (ch >= 'A' && ch <= 'F') d = ch - 'A' + 10;
            else return false;
            v = (v << 4) | d;
        }
        out[i] = (uint8_t)v;
    }
    return true;
}

static String toHex(const uint8_t* data, size_t len) {
    static const char* hx = "0123456789abcdef";
    String s; s.reserve(len * 2);
    for (size_t i = 0; i < len; i++) { s += hx[data[i] >> 4]; s += hx[data[i] & 0xF]; }
    return s;
}

void BLEUartModule::handleLogGet(const String& args) {
    static Encounter snap[StorageModule::MAX_ENCOUNTERS];
    int total = StorageModule::snapshot(snap, StorageModule::MAX_ENCOUNTERS);
    if (args.length() == 0) {
        enqueueData(String("LOG total=") + String(total));
        endTransaction();
        return;
    }
    int offset = 0, cnt = LOG_PAGE_MAX;
    int parsed = sscanf(args.c_str(), "%d %d", &offset, &cnt);
    if (parsed == 1) cnt = LOG_PAGE_MAX;
    else if (parsed != 2) { errTransaction("BAD_ARG", "offset count"); return; }
    if (offset < 0 || offset > total) { errTransaction("RANGE", "offset"); return; }
    if (cnt <= 0) cnt = LOG_PAGE_MAX;
    if (cnt > LOG_PAGE_MAX) cnt = LOG_PAGE_MAX;
    int end = offset + cnt; if (end > total) end = total;
    int actual = end - offset;
    LOG("LOG_GET page offset=%d count=%d total=%d\n", offset, actual, total);
    enqueueResponse(String(activeId) + " LOG total=" + String(total)
                    + " offset=" + String(offset) + " count=" + String(actual) + "\n");
    for (int i = offset; i < end; i++) {
        Encounter& e = snap[i];
        String rec = "ENC seq="; rec += e.seq;
        rec += " id="; rec += toHex(e.id, 16);
        rec += " tf="; rec += e.tFirst;
        rec += " tl="; rec += e.tLast;
        rec += " rssi="; rec += e.rssi;
        rec += " cnt="; rec += e.count;
        rec += " tag="; rec += toHex(e.tag, 16);
        enqueueData(rec);
    }
    endTransaction();
}

void BLEUartModule::processCommand(const String& line) {
    int sp1 = line.indexOf(' ');
    if (sp1 <= 0) return;
    long idLong = line.substring(0, sp1).toInt();
    if (idLong <= 0 || idLong > 65535) return;
    uint16_t id = (uint16_t)idLong;
    String rest = line.substring(sp1 + 1); rest.trim();
    if (rest.length() == 0) { enqueueResponse(String(id) + " ERR BAD_CMD\n"); return; }
    int sp2 = rest.indexOf(' ');
    String cmd = (sp2 < 0) ? rest : rest.substring(0, sp2);
    String args = (sp2 < 0) ? String("") : rest.substring(sp2 + 1);
    args.trim();

    if (activeId != 0) {
        LOG("BUSY id=%u (active=%u), rejecting cmd=%s\n", id, activeId, cmd.c_str());
        enqueueResponse(String(id) + " ERR BUSY\n");
        return;
    }
    activeId = id; txDataLines = 0; txClosing = false; txDeadlineMs = millis();
    LOG("CMD id=%u %s args='%s'\n", id, cmd.c_str(), args.c_str());

    // --- Информация об устройстве (всем) ---
    if (cmd == "HELLO") {
        String payload = String("HELLO proto=") + PROTO_VERSION
                       + " fw=" + FW_VERSION + " dev=" + KeystoreModule::devNo()
                       + " paired=" + String(KeystoreModule::paired() ? 1 : 0)
                       + " timeok=" + String(KeystoreModule::timeValid() ? 1 : 0)
                       + " mtu=" + String(currentMtu());
        enqueueResponse(String(activeId) + " " + payload + "\n");
        endTransaction();
        return;
    }

    // --- Провизьон с завода (только для непровизьоненного устройства) ---
    if (cmd == "PROVISION") {
        if (KeystoreModule::paired()) { errTransaction("ALREADY_PAIRED", ""); return; }
        if (KeystoreModule::provisioned()) { errTransaction("ALREADY_PROVISIONED", ""); return; }
        // args: <devNo> <kDev_hex32> <kPair_hex32>
        int s1 = args.indexOf(' ');
        if (s1 <= 0) { errTransaction("BAD_ARG", "devNo kdev kpair"); return; }
        String devNoStr = args.substring(0, s1);
        String restArgs = args.substring(s1 + 1); restArgs.trim();
        int s2 = restArgs.indexOf(' ');
        if (s2 <= 0) { errTransaction("BAD_ARG", "kdev kpair"); return; }
        String kDevStr = restArgs.substring(0, s2);
        String kPairStr = restArgs.substring(s2 + 1); kPairStr.trim();
        if (devNoStr.length() == 0 || devNoStr.length() > 15) {
            errTransaction("BAD_ARG", "devNo"); return;
        }
        uint8_t kDev[16], kPair[16];
        if (!parseHex(kDevStr, kDev, 16) || !parseHex(kPairStr, kPair, 16)) {
            errTransaction("BAD_ARG", "hex"); return;
        }
        if (!KeystoreModule::provision(devNoStr.c_str(), kDev, kPair)) {
            errTransaction("PROV_FAIL", ""); return;
        }
        BLEModule::restartAdvertising();
        enqueueResponse(String(activeId) + " OK dev=" + KeystoreModule::devNo() + "\n");
        endTransaction();
        return;
    }

    // --- Проверка провизьона: HMAC(K_dev, nonce), ключ не покидает брелок ---
    if (cmd == "PROV_VERIFY") {
        uint8_t nonce[16];
        if (!parseHex(args, nonce, 16)) { errTransaction("BAD_ARG", "nonce16"); return; }
        uint8_t kDev[16]; KeystoreModule::getKDev(kDev);
        uint8_t mac[32];
        if (!CryptoModule::hmacSha256(kDev, 16, nonce, 16, mac)) {
            errTransaction("CRYPTO", ""); return;
        }
        enqueueData(String("PROV dev=") + KeystoreModule::devNo()
                    + " mac=" + toHex(mac, 16));
        endTransaction();
        return;
    }

    // --- Заводской сброс провизьона ---
    // Непривязанный брелок сбрасывается без аутентификации. Привязанный —
    // только по доказательству знания K_dev (FACTORY_RESET_BEGIN/FINISH):
    // K_dev никогда не покидает устройство и есть только у завода/сервера,
    // поэтому это безопасный manufacturing-путь, не требующий владельца.
    if (cmd == "FACTORY_RESET") {
        if (KeystoreModule::paired()) { errTransaction("ALREADY_PAIRED", "unpair first"); return; }
        LOGLN("FACTORY_RESET: clearing provision");
        KeystoreModule::factoryReset();
        StorageModule::clear();
        s_ownerAuthed = false;
        BLEModule::restartAdvertising();
        enqueueResponse(String(activeId) + " OK\n");
        endTransaction();
        return;
    }

    // Шаг 1 manufacturing-сброса привязанного брелока: выдать одноразовое число.
    if (cmd == "FACTORY_RESET_BEGIN") {
        if (!CryptoModule::random(s_challenge, 16)) { errTransaction("RNG", ""); return; }
        s_challengeActive = true;
        enqueueData(String("NONCE ") + toHex(s_challenge, 16));
        endTransaction();
        return;
    }

    // Шаг 2: HMAC-SHA256(K_dev, nonce); при совпадении стираем провизьон.
    if (cmd == "FACTORY_RESET_FINISH") {
        if (!s_challengeActive) { errTransaction("NO_CHALLENGE", ""); return; }
        uint8_t sig[32];
        if (!parseHex(args, sig, 32)) { errTransaction("BAD_ARG", "hmac32"); return; }
        uint8_t kDev[16]; KeystoreModule::getKDev(kDev);
        uint8_t mac[32];
        if (!CryptoModule::hmacSha256(kDev, 16, s_challenge, 16, mac)) {
            errTransaction("CRYPTO", ""); return;
        }
        s_challengeActive = false;
        if (memcmp(mac, sig, 32) != 0) {
            LOGLN("FACTORY_RESET: proof mismatch");
            errTransaction("RESET_FAIL", ""); return;
        }
        LOGLN("FACTORY_RESET: authenticated wipe");
        KeystoreModule::factoryReset();
        StorageModule::clear();
        s_ownerAuthed = false;
        BLEModule::restartAdvertising();
        enqueueResponse(String(activeId) + " OK\n");
        endTransaction();
        return;
    }

    // --- Аутентификация владельца (challenge-response K_owner) ---
    if (cmd == "AUTH_BEGIN") {
        if (!KeystoreModule::paired()) { errTransaction("NOT_PAIRED", ""); return; }
        if (millis() < s_lockoutUntil) { errTransaction("LOCKOUT", "wait"); return; }
        if (!CryptoModule::random(s_challenge, 16)) { errTransaction("RNG", ""); return; }
        s_challengeActive = true;
        s_ownerAuthed = false;
        enqueueData(String("CHALLENGE ") + toHex(s_challenge, 16));
        endTransaction();
        return;
    }

    if (cmd == "AUTH") {
        if (!s_challengeActive) { errTransaction("NO_CHALLENGE", ""); return; }
        uint8_t resp[16];
        if (!parseHex(args, resp, 16)) { errTransaction("BAD_ARG", "resp16"); return; }
        uint8_t kOwner[16]; KeystoreModule::getKOwner(kOwner);
        uint8_t mac[32];
        if (!CryptoModule::hmacSha256(kOwner, 16, s_challenge, 16, mac)) {
            errTransaction("CRYPTO", ""); return;
        }
        if (memcmp(mac, resp, 16) == 0) {
            LOGLN("AUTH: owner ok");
            s_ownerAuthed = true; s_authFails = 0;
            enqueueResponse(String(activeId) + " OK\n");
        } else {
            s_authFails++;
            LOG("AUTH: fail #%u\n", s_authFails);
            if (s_authFails >= AUTH_MAX_FAILS) {
                s_lockoutUntil = millis() + AUTH_LOCKOUT_MS;
                s_authFails = 0;
                LOGLN("AUTH: lockout engaged");
                errTransaction("LOCKOUT", "too many");
            } else {
                errTransaction("AUTH_FAIL", "");
            }
        }
        s_challengeActive = false;
        endTransaction();
        return;
    }

    // --- Привязка (по K_pair) ---
    if (cmd == "PAIR_BEGIN") {
        if (KeystoreModule::paired()) { errTransaction("ALREADY_PAIRED", ""); return; }
        if (!CryptoModule::random(s_challenge, 16)) { errTransaction("RNG", ""); return; }
        s_challengeActive = true;
        enqueueData(String("CHALLENGE ") + toHex(s_challenge, 16));
        endTransaction();
        return;
    }

    if (cmd == "PAIR_FINISH") {
        // args: <resp16> <kowner16> <nonce16> <utc>
        if (!s_challengeActive) { errTransaction("NO_CHALLENGE", ""); return; }
        int s1 = args.indexOf(' ');
        if (s1 < 0) { errTransaction("BAD_ARG", "resp"); return; }
        String respStr = args.substring(0, s1);
        String restArgs = args.substring(s1 + 1); restArgs.trim();
        int s2 = restArgs.indexOf(' ');
        if (s2 < 0) { errTransaction("BAD_ARG", "kowner"); return; }
        String kOwnerStr = restArgs.substring(0, s2);
        String rest2 = restArgs.substring(s2 + 1); rest2.trim();
        int s3 = rest2.indexOf(' ');
        if (s3 < 0) { errTransaction("BAD_ARG", "nonce"); return; }
        String nonceStr = rest2.substring(0, s3);
        uint32_t utc = (uint32_t)rest2.substring(s3 + 1).toInt();

        uint8_t resp[16], kOwner[16], nonce[16];
        if (!parseHex(respStr, resp, 16) || !parseHex(kOwnerStr, kOwner, 16) ||
            !parseHex(nonceStr, nonce, 16)) { errTransaction("BAD_ARG", "hex"); return; }

        uint8_t kPair[16]; KeystoreModule::getKPair(kPair);
        uint8_t mac[32];
        if (!CryptoModule::hmacSha256(kPair, 16, s_challenge, 16, mac)) {
            errTransaction("CRYPTO", ""); return;
        }
        if (memcmp(mac, resp, 16) != 0) {
            LOGLN("PAIR: challenge response mismatch");
            s_challengeActive = false;
            errTransaction("PAIR_FAIL", ""); return;
        }
        s_challengeActive = false;
        LOGLN("PAIR: challenge ok, storing K_owner");

        KeystoreModule::setKOwner(kOwner);
        KeystoreModule::setPaired(true);
        KeystoreModule::setUtc(utc);

        // receipt = HMAC-SHA256(K_rcpt, "paired" ‖ devNo ‖ nonce ‖ SHA256(K_owner))
        uint8_t kRcpt[32];
        if (!KeystoreModule::deriveRcpt(kRcpt)) { errTransaction("CRYPTO", ""); return; }
        uint8_t sha[32];
        if (!CryptoModule::sha256(kOwner, 16, sha)) {
            errTransaction("CRYPTO", "sha256"); return;
        }
        uint8_t msg[5 + 16 + 16 + 32];
        size_t m = 0;
        memcpy(msg + m, "paired", 6); m += 6;
        const char* dn = KeystoreModule::devNo();
        size_t dl = strlen(dn); if (dl > 16) dl = 16;
        memcpy(msg + m, dn, dl); m += dl;
        memcpy(msg + m, nonce, 16); m += 16;
        memcpy(msg + m, sha, 32); m += 32;
        uint8_t receipt[32];
        if (!CryptoModule::hmacSha256(kRcpt, 32, msg, m, receipt)) {
            errTransaction("CRYPTO", ""); return;
        }
        enqueueData(String("RECEIPT ") + toHex(receipt, 32));
        endTransaction();
        return;
    }

    // --- Время (владелец): только подписанное сервером ---
    if (cmd == "TIME_BEGIN") {
        if (!KeystoreModule::paired()) { errTransaction("NOT_PAIRED", ""); return; }
        if (!CryptoModule::random(s_challenge, 16)) { errTransaction("RNG", ""); return; }
        s_challengeActive = true;
        enqueueData(String("NONCE ") + toHex(s_challenge, 16));
        endTransaction();
        return;
    }

    if (cmd == "TIME_SET") {
        // args: <utc> <hmac32>  (HMAC(K_time, utc_be4 ‖ nonce))
        if (!s_challengeActive) { errTransaction("NO_NONCE", ""); return; }
        int sp = args.indexOf(' ');
        if (sp < 0) { errTransaction("BAD_ARG", "utc hmac"); return; }
        uint32_t utc = (uint32_t)args.substring(0, sp).toInt();
        uint8_t sig[32];
        if (!parseHex(args.substring(sp + 1), sig, 32)) { errTransaction("BAD_ARG", "hmac32"); return; }
        uint8_t kTime[32];
        if (!KeystoreModule::deriveTime(kTime)) { errTransaction("CRYPTO", ""); return; }
        uint8_t msg[8];
        msg[0] = (uint8_t)(utc >> 24); msg[1] = (uint8_t)(utc >> 16);
        msg[2] = (uint8_t)(utc >> 8);  msg[3] = (uint8_t)utc;
        memcpy(msg + 4, s_challenge, 4);
        uint8_t mac[32];
        if (!CryptoModule::hmacSha256(kTime, 32, msg, 8, mac)) {
            errTransaction("CRYPTO", ""); return;
        }
        if (memcmp(mac, sig, 32) != 0) {
            LOGLN("TIME: signature mismatch");
            errTransaction("TIME_FAIL", ""); return;
        }
        s_challengeActive = false;
        LOG("TIME: server time accepted utc=%u\n", (unsigned)utc);
        KeystoreModule::setUtc(utc);
        BLEModule::loop();
        enqueueResponse(String(activeId) + " OK\n");
        endTransaction();
        return;
    }

    // --- Чтение журнала (владелец) ---
    if (cmd == "LOG_GET") {
        if (!KeystoreModule::paired()) { errTransaction("NOT_PAIRED", ""); return; }
        if (!s_ownerAuthed) { errTransaction("AUTH_REQUIRED", ""); return; }
        handleLogGet(args);
        return;
    }

    if (cmd == "LOG_ACK") {
        if (!s_ownerAuthed) { errTransaction("AUTH_REQUIRED", ""); return; }
        uint32_t upto = (uint32_t)args.toInt();
        int removed = StorageModule::ackUpTo(upto);
        enqueueData(String("ACKED ") + String(removed));
        endTransaction();
        return;
    }

    // --- Заряд ---
    if (cmd == "BATTERY") {
        if (!s_ownerAuthed) { errTransaction("AUTH_REQUIRED", ""); return; }
        enqueueData(String("BAT mv=0 pct=0"));
        endTransaction();
        return;
    }

    // --- Настройки (владелец) ---
    if (cmd == "SET_RSSI") {
        if (!s_ownerAuthed) { errTransaction("AUTH_REQUIRED", ""); return; }
        KeystoreModule::setRssiThreshold((int8_t)args.toInt());
        enqueueResponse(String(activeId) + " OK\n");
        endTransaction();
        return;
    }

    if (cmd == "SET_ADV_INTERVAL") {
        if (!s_ownerAuthed) { errTransaction("AUTH_REQUIRED", ""); return; }
        KeystoreModule::setAdvIntervalMs((uint16_t)args.toInt());
        BLEModule::restartAdvertising();
        enqueueResponse(String(activeId) + " OK\n");
        endTransaction();
        return;
    }

    if (cmd == "SET_ADV_TX") {
        if (!s_ownerAuthed) { errTransaction("AUTH_REQUIRED", ""); return; }
        KeystoreModule::setAdvTxPowerDbm((int8_t)args.toInt());
        Bluefruit.setTxPower((int8_t)args.toInt());
        enqueueResponse(String(activeId) + " OK\n");
        endTransaction();
        return;
    }

    if (cmd == "SET_SCAN") {
        if (!s_ownerAuthed) { errTransaction("AUTH_REQUIRED", ""); return; }
        int win = 0, per = 0;
        if (sscanf(args.c_str(), "%d %d", &win, &per) != 2) {
            errTransaction("BAD_ARG", "window period"); return;
        }
        KeystoreModule::setScanWindowMs((uint16_t)win);
        KeystoreModule::setScanPeriodMs((uint16_t)per);
        Bluefruit.Scanner.stop();
        Bluefruit.Scanner.setInterval((uint16_t)(per / 0.625f), (uint16_t)(win / 0.625f));
        Bluefruit.Scanner.start(0);
        enqueueResponse(String(activeId) + " OK\n");
        endTransaction();
        return;
    }

    // --- Отвязка (владелец, nonce сервера) ---
    if (cmd == "UNPAIR") {
        if (!s_ownerAuthed) { errTransaction("AUTH_REQUIRED", ""); return; }
        uint8_t nonce[16];
        if (!parseHex(args, nonce, 16)) { errTransaction("BAD_ARG", "nonce16"); return; }
        uint8_t kRcpt[32];
        if (!KeystoreModule::deriveRcpt(kRcpt)) { errTransaction("CRYPTO", ""); return; }
        uint8_t msg[8 + 16 + 16];
        size_t m = 0;
        memcpy(msg + m, "unpaired", 8); m += 8;
        const char* dn = KeystoreModule::devNo();
        size_t dl = strlen(dn); if (dl > 16) dl = 16;
        memcpy(msg + m, dn, dl); m += dl;
        memcpy(msg + m, nonce, 16); m += 16;
        uint8_t receipt[32];
        if (!CryptoModule::hmacSha256(kRcpt, 32, msg, m, receipt)) {
            errTransaction("CRYPTO", ""); return;
        }
        LOGLN("UNPAIR: clearing owner and journal");
        KeystoreModule::unpair();
        StorageModule::clear();
        s_ownerAuthed = false;
        BLEModule::restartAdvertising();
        enqueueData(String("RECEIPT ") + toHex(receipt, 32));
        endTransaction();
        return;
    }

    errTransaction("BAD_CMD", "unknown");
}
