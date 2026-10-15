#ifndef BLE_UART_MODULE_H
#define BLE_UART_MODULE_H

#include <bluefruit.h>
#include <queue>

// GATT-протокол брелока (см. PROTOCOL.md, task.txt п.6).
//
// Запрос телефона : <id> <CMD> [аргументы]\n
// Ответ MCU       : <id> <TOKEN> [данные]\n
//                   <id> END n=<N>\n
//                   <id> ERR <код> [текст]\n
class BLEUartModule {
public:
    static void begin();
    static void loop();

private:
    static BLEUart bleuart;
    static void rxCallback(uint16_t conn_handle);
    static void processCommand(const String& line);

    static std::queue<String> txQueue;
    static String txCurrent;
    static size_t txOffset;
    static uint32_t txRetryMs;

    static uint16_t activeId;
    static int      txDataLines;
    static uint32_t txDeadlineMs;
    static bool     txClosing;

    static uint16_t currentMtu();
    static void enqueueResponse(const String& resp);
    static void enqueueData(const String& token);
    static void endTransaction();
    static void errTransaction(const char* code, const char* text);
    static void txPump();
    static void finishTransaction();

    static void handleLogGet(const String& args);
};

#endif
