#include "ble_uart_module.h"
#include "config.h"
#include "storage_module.h"
#include "settings_module.h"
#include "ble_module.h"

BLEUart BLEUartModule::bleuart;

// Асинхронная TX-очередь: строки ответа формируются в rxCallback и
// складываются сюда, физическая отправка идёт из loop() через txPump().
std::queue<String> BLEUartModule::txQueue;
String BLEUartModule::txCurrent = "";
size_t BLEUartModule::txOffset = 0;
uint32_t BLEUartModule::txRetryMs = 0;
bool BLEUartModule::resetPending = false;

// --- Состояние активной транзакции ---
uint16_t BLEUartModule::activeId = 0;
int       BLEUartModule::txDataLines = 0;
uint32_t  BLEUartModule::txDeadlineMs = 0;
bool      BLEUartModule::txClosing = false;

// Если за это время ни одного байта не удалось вытолкнуть — считаем
// транзакцию зависшей, очищаем очередь и принимаем следующую команду.
static const uint32_t TX_STALL_TIMEOUT_MS = 5000;

void BLEUartModule::begin() {
    // Отправляем напрямую через notify: write() возвращает число реально
    // принятых байт (0 = стек занят), поэтому txPump() честно отслеживает
    // txOffset и повторяет неудачный notify на следующем тике.
    // bufferTXD(true) НЕ используем: он копит данные во внутреннем FIFO и
    // всегда возвращает len, из-за чего txPump() считал кусок отправленным,
    // хотя notify фактически ещё не ушёл (обрыв/дубли на границе MTU).
    bleuart.begin();
    bleuart.setRxCallback(rxCallback);
    // Advertising уже настроен в BLEModule::begin() (baseUuid + name).
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

    // Сторож транзакции: если TX застрял и за TX_STALL_TIMEOUT_MS не
    // отправилось ни байта — сбрасываем очередь и готовы к новой команде.
    if (activeId != 0 &&
        (uint32_t)(millis() - txDeadlineMs) > TX_STALL_TIMEOUT_MS) {
        LOGLN("Transaction watchdog: resetting TX.");
        std::queue<String> empty;
        std::swap(txQueue, empty);
        txCurrent = "";
        txOffset = 0;
        finishTransaction();
    }

    // Отложенный сброс: даём txPump() вытолкнуть ответ, затем перезагружаемся.
    if (resetPending && txCurrent.length() == 0 && txQueue.empty()) {
        LOGLN("SET_MAC: rebooting...");
        delay(50);
        NVIC_SystemReset();
    }
}

// Кладёт готовую строку (с уже вписанным id) в очередь. Без delay/write,
// чтобы не блокировать BLE-стек внутри rxCallback.
void BLEUartModule::enqueueResponse(const String& resp) {
    if (resp.length() == 0) return;
    if (txQueue.size() >= TX_QUEUE_MAX) {
        LOGLN("TX queue full, dropping response.");
        return;
    }
    txQueue.push(resp);
}

// Строка данных текущей транзакции: "<id> <token>\n", увеличивает счётчик
// строк n (кроме служебных строк — они кладутся через enqueueResponse).
void BLEUartModule::enqueueData(const String& token) {
    if (activeId == 0) return;
    txDataLines++;
    enqueueResponse(String(activeId) + " " + token + "\n");
}

// Успешный терминатор: "<id> END n=<число строк данных>".
void BLEUartModule::endTransaction() {
    if (activeId == 0) return;
    enqueueResponse(String(activeId) + " END n=" + String(txDataLines) + "\n");
    txClosing = true;
}

// Ошибочный терминатор: "<id> ERR <код> [текст]".
void BLEUartModule::errTransaction(const char* code, const char* text) {
    if (activeId == 0) return;
    String line = String(activeId) + " ERR " + code;
    if (text != nullptr && text[0] != '\0') {
        line += " ";
        line += text;
    }
    line += "\n";
    enqueueResponse(line);
    txClosing = true;
}

void BLEUartModule::finishTransaction() {
    activeId = 0;
    txDataLines = 0;
    txClosing = false;
}

// Продвигает отправку текущей строки. Вызывается из loop().
// При bufferTXD(false) write() = прямой notify: возвращает длину при успехе
// и 0, если стек занят. Поэтому txOffset двигаем только на реально принятое
// число байт, а неудачный notify просто повторяем на следующем тике.
void BLEUartModule::txPump() {
    if (txCurrent.length() == 0) {
        if (txQueue.empty()) {
            if (txClosing) finishTransaction();
            return;
        }
        txCurrent = txQueue.front();
        txQueue.pop();
        txOffset = 0;
        txRetryMs = millis();
    }

    // Размер куска — актуальный MTU минус 3 байта заголовка ATT.
    uint16_t chunk = currentMtu();
    if (chunk < 23) chunk = 23;
    chunk -= 3;

    size_t remain = txCurrent.length() - txOffset;
    if (remain > chunk) remain = chunk;

    const uint8_t* data = (const uint8_t*)txCurrent.c_str() + txOffset;
    size_t n = bleuart.write(data, remain);
    if (n == 0) {
        // notify не принят (стек занят): повторяем на следующем тике.
        // Таймаут не даёт строке зависнуть навсегда.
        if ((uint32_t)(millis() - txRetryMs) > 2000) {
            LOG("TX timeout, dropping line.\n");
            txCurrent = "";
            txOffset = 0;
            txRetryMs = millis();
        }
        return;
    }

    txOffset += n;
    txRetryMs = millis();
    txDeadlineMs = millis();  // прогресс — продлеваем сторож транзакции

    if (txOffset >= txCurrent.length()) {
        LOG("TX done: %u bytes\n", (unsigned)txCurrent.length());
        txCurrent = "";
        txOffset = 0;
        if (txQueue.empty() && txClosing) finishTransaction();
    }
}

void BLEUartModule::rxCallback(uint16_t conn_handle) {
    char buffer[160];
    int len = bleuart.read(buffer, sizeof(buffer) - 1);
    if (len <= 0) return;

    buffer[len] = '\0';
    LOG("RX chunk (%d): '%s'\n", len, buffer);

    // Статический буфер для накопления строки запроса.
    static String cmdBuffer = "";
    static bool overflow = false;

    for (int i = 0; i < len; i++) {
        char c = buffer[i];
        if (c == '\n' || c == '\r') {
            if (overflow) {
                // Уже ответили TOO_LONG — ждём конца строки.
                if (c == '\n') overflow = false;
                cmdBuffer = "";
                continue;
            }
            cmdBuffer.trim();
            if (cmdBuffer.length() > 0) {
                LOG("Complete command: '%s'\n", cmdBuffer.c_str());
                processCommand(cmdBuffer);
            }
            cmdBuffer = "";
        } else if (!overflow) {
            cmdBuffer += c;
            if (cmdBuffer.length() > MAX_LINE) {
                LOGLN("Command too long, resetting buffer.");
                cmdBuffer = "";
                overflow = true;
                enqueueResponse("0 ERR TOO_LONG line too long\n");
            }
        }
    }
}

void BLEUartModule::processCommand(const String& line) {
    // Разбор запроса: <id> <CMD> [аргументы]
    int sp1 = line.indexOf(' ');
    if (sp1 <= 0) {
        LOGLN("Malformed line (no id/cmd), ignoring.");
        return;
    }
    long idLong = line.substring(0, sp1).toInt();
    if (idLong <= 0 || idLong > 65535) {
        LOGLN("Invalid transaction id, ignoring.");
        return;
    }
    uint16_t id = (uint16_t)idLong;

    String rest = line.substring(sp1 + 1);
    rest.trim();
    if (rest.length() == 0) {
        enqueueResponse(String(id) + " ERR BAD_CMD empty command\n");
        return;
    }

    int sp2 = rest.indexOf(' ');
    String cmd = (sp2 < 0) ? rest : rest.substring(0, sp2);
    String args = (sp2 < 0) ? String("") : rest.substring(sp2 + 1);
    args.trim();

    // Одна транзакция за раз: пока предыдущая не завершена — BUSY.
    if (activeId != 0) {
        enqueueResponse(String(id) + " ERR BUSY transaction in progress\n");
        return;
    }

    // Начинаем новую транзакцию.
    activeId = id;
    txDataLines = 0;
    txClosing = false;
    txDeadlineMs = millis();

    if (cmd == "HELLO") {
        // Служебная строка, в n не входит (END n=0).
        String payload = String("HELLO proto=") + PROTO_VERSION
                       + " fw=" + FW_VERSION
                       + " mtu=" + String(currentMtu());
        enqueueResponse(String(activeId) + " " + payload + "\n");
        endTransaction();
        return;
    }

    if (cmd == "GET_NAME") {
        enqueueData(String("NAME ") + SettingsModule::getName());
        endTransaction();
        return;
    }

    if (cmd == "GET_MAC") {
        uint8_t mac[6];
        SettingsModule::getMac(mac);
        char macStr[18];
        snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        enqueueData(String("MAC ") + macStr);
        endTransaction();
        return;
    }

    if (cmd == "SET_NAME") {
        if (args.length() == 0) {
            errTransaction("BAD_ARG", "name required");
            return;
        }
        SettingsModule::setName(args.c_str());
        BLEModule::setDeviceName(args.c_str());
        enqueueResponse(String(activeId) + " OK\n");
        endTransaction();
        return;
    }

    if (cmd == "SET_MAC") {
        // ВАЖНО: не использовать %hhx — Adafruit nRF52 core линкует
        // newlib-nano, чей sscanf/vfscanf не реализует модификатор 'hh',
        // из-за чего любой MAC даёт BAD_ARG. Парсим через %x в unsigned
        // int и сами приводим к uint8_t.
        uint8_t mac[6];
        unsigned int m0, m1, m2, m3, m4, m5;
        if (sscanf(args.c_str(), "%x:%x:%x:%x:%x:%x",
                   &m0, &m1, &m2, &m3, &m4, &m5) == 6 &&
            m0 <= 0xFF && m1 <= 0xFF && m2 <= 0xFF &&
            m3 <= 0xFF && m4 <= 0xFF && m5 <= 0xFF) {
            mac[0] = (uint8_t)m0;
            mac[1] = (uint8_t)m1;
            mac[2] = (uint8_t)m2;
            mac[3] = (uint8_t)m3;
            mac[4] = (uint8_t)m4;
            mac[5] = (uint8_t)m5;
            SettingsModule::setMac(mac);
            enqueueResponse(String(activeId) + " OK\n");
            endTransaction();
            // Сброс выполнит loop() после того, как txPump() отправит ответ.
            resetPending = true;
        } else {
            errTransaction("BAD_ARG", "MAC format");
        }
        return;
    }

    // --- Журнал посещённых устройств ---
    // GET_DEVICES            -> DEVS total=<N>            (END n=1)
    // GET_DEVICES <off> <n>  -> DEVS total.. offset.. count.. (служебная)
    //                           DEV {..} x count          (END n=count)
    if (cmd == "GET_DEVICES") {
        handleGetDevices(args);
        return;
    }

    if (cmd == "CLEAR_DEVICES") {
        StorageModule::clearDevices();
        enqueueResponse(String(activeId) + " OK\n");
        endTransaction();
        return;
    }

    errTransaction("BAD_CMD", "unknown command");
}

void BLEUartModule::handleGetDevices(const String& args) {
    // Снимок делаем один раз на запрос: scan-callback может параллельно
    // дописывать журнал, поэтому снимок гарантирует консистентность страницы.
    static SeenDevice snap[StorageModule::MAX_DEVICES];
    int total = StorageModule::snapshot(snap, StorageModule::MAX_DEVICES);

    // Без аргументов — только метаданные (это одна строка данных, n=1).
    if (args.length() == 0) {
        enqueueData(String("DEVS total=") + String(total));
        endTransaction();
        return;
    }

    int offset = 0;
    int count = DEVICES_PAGE_MAX;
    int parsed = sscanf(args.c_str(), "%d %d", &offset, &count);
    if (parsed == 1) {
        count = DEVICES_PAGE_MAX;  // разрешён "GET_DEVICES <offset>"
    } else if (parsed != 2) {
        errTransaction("BAD_ARG", "offset count");
        return;
    }

    if (offset < 0 || offset > total) {
        errTransaction("RANGE", "offset out of range");
        return;
    }
    if (count <= 0) count = DEVICES_PAGE_MAX;
    if (count > DEVICES_PAGE_MAX) count = DEVICES_PAGE_MAX;

    int end = offset + count;
    if (end > total) end = total;
    int actual = end - offset;

    // Служебная строка страницы (в n не входит).
    enqueueResponse(String(activeId) + " DEVS total=" + String(total)
                    + " offset=" + String(offset)
                    + " count=" + String(actual) + "\n");

    for (int i = offset; i < end; i++) {
        char macStr[18];
        sprintf(macStr, "%02X:%02X:%02X:%02X:%02X:%02X",
                snap[i].mac[0], snap[i].mac[1], snap[i].mac[2],
                snap[i].mac[3], snap[i].mac[4], snap[i].mac[5]);

        String rec = "DEV {\"m\":\"";
        rec += macStr;
        rec += "\",\"r\":";
        rec += snap[i].rssi;
        rec += ",\"t\":";
        rec += snap[i].timestamp;
        if (snap[i].name[0] != '\0') {
            rec += ",\"n\":\"";
            rec += snap[i].name;
            rec += "\"";
        }
        rec += "}";

        if (rec.length() + 8 > MAX_LINE) {
            // Не должно случаться при SEEN_NAME_MAX=24; страхуемся от обрезки.
            rec = "DEV {\"m\":\"";
            rec += macStr;
            rec += "\"}";
        }
        enqueueData(rec);
    }

    LOG("TX devices page off=%d cnt=%d total=%d\n", offset, actual, total);
    endTransaction();
}
