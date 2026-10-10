#ifndef BLE_UART_MODULE_H
#define BLE_UART_MODULE_H

#include <bluefruit.h>

class BLEUartModule {
public:
    static void begin();
    static void loop();

private:
    static BLEUart bleuart;
    static void rxCallback(uint16_t conn_handle);
    static void connectCallback(uint16_t conn_handle);
    static void disconnectCallback(uint16_t conn_handle, uint8_t reason);
    static void processCommand(const String& command);
};

#endif