#ifndef BLE_UART_MODULE_H
#define BLE_UART_MODULE_H

#include <bluefruit.h>
#include <queue>

// BLE UART-протокол с транзакциями (см. protocol_ble.txt).
//
// Запрос телефона : <id> <CMD> [аргументы]\n
// Ответ MCU       : <id> <TOKEN> [данные]\n   (0..N строк данных)
//                   <id> END n=<N>\n          (терминатор успеха)
//                   <id> ERR <код> [текст]\n  (терминатор ошибки)
//
// Одна транзакция за раз: пока ответ предыдущей команды не отправлен
// полностью (END/ERR ушли), новая команда получает BUSY.
class BLEUartModule {
public:
    static void begin();
    static void loop();

private:
    static BLEUart bleuart;

    static void rxCallback(uint16_t conn_handle);

    // Разбирает накопленную строку <id> <CMD> [args] и формирует ответ.
    static void processCommand(const String& line);

    // --- Асинхронная TX-очередь ---
    // Формирование ответа и физическая отправка разнесены: rxCallback
    // только кладёт готовые строки в очередь (без delay/блокировок), а
    // txPump() вызывается из loop() и отправляет их через BLE-стек.
    static std::queue<String> txQueue;
    static String txCurrent;     // строка, отправляемая прямо сейчас
    static size_t txOffset;      // сколько байт txCurrent уже отправлено
    static uint32_t txRetryMs;   // время последней неудачной попытки
    static bool resetPending;    // отложенный сброс (SET_MAC)

    // --- Состояние активной транзакции ---
    static uint16_t activeId;      // id текущей транзакции (0 = нет активной)
    static int      txDataLines;   // строк данных, поставленных в очередь
    static uint32_t txDeadlineMs;  // дедлайн транзакции (защита от зависания)
    static bool     txClosing;     // поставлен терминатор END/ERR

    // Текущий согласованный MTU активного соединения (или PROTO_MTU_REPORT).
    static uint16_t currentMtu();

    // Кладёт готовую строку в очередь (без блокировки).
    static void enqueueResponse(const String& resp);

    // Строка данных транзакции: добавляет префикс "<id> " и считает n.
    static void enqueueData(const String& token);

    // Терминаторы транзакции: "<id> END n=N" и "<id> ERR ...".
    static void endTransaction();
    static void errTransaction(const char* code, const char* text);

    // Вызывается из loop(): продвигает отправку текущего ответа.
    static void txPump();

    // Сброс состояния транзакции по завершении/таймауту.
    static void finishTransaction();

    // Отправка страницы журнала посещённых устройств.
    static void handleGetDevices(const String& args);
};

#endif
