#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>

// 1 = Serial включён (отладка), 0 = выключен (батарея).
// USB CDC держит PHY включённым и тянет ~0.5–1 mA, поэтому в релизе выключаем.
#define ENABLE_SERIAL 1

// Mirror logs onto hardware UART (Serial1) for an external USB-UART (CH341).
// This survives USB-CDC re-enumeration during resets, unlike Serial.
// nice!nano default UART pins: TX = P0.06 (D0), RX = P0.08 (D1).
// Wiring to CH341: board TX -> CH341 RX, GND <-> GND, 115200 baud.
#define ENABLE_UART_MIRROR 1
#define UART_MIRROR_TX_PIN 6
#define UART_MIRROR_RX_PIN 8

// 1 = BLE-скан включён (основной режим), 0 = выключен (тест питания:
// проверяем, не вызывает ли ~90%% duty-скан ребутов). Переопределяется
// build-флагом -DENABLE_BLE_SCAN=0 в env:power_test.
#ifndef ENABLE_BLE_SCAN
  #define ENABLE_BLE_SCAN 1
#endif

#if ENABLE_SERIAL
  #define LOG(...)   Serial.printf(__VA_ARGS__)
  #define LOGLN(s)   Serial.println(s)
#else
  #define LOG(...)   do {} while (0)
  #define LOGLN(s)   do {} while (0)
#endif

#if ENABLE_UART_MIRROR
  #define ULOG(...)   Serial1.printf(__VA_ARGS__)
  #define ULOGLN(s)   Serial1.println(s)
#else
  #define ULOG(...)   do {} while (0)
  #define ULOGLN(s)   do {} while (0)
#endif

// --- BLE UART TX-очередь ---
// Ответы формируются в RX-callback и складываются в очередь, физическая
// отправка (notify) выполняется из loop(). См. PROTOCOL.md.
#define TX_QUEUE_MAX 16

// --- Протокол BLE (см. PROTOCOL.md) ---
#define PROTO_VERSION   3
#define FW_VERSION      "2.0.0"

// Фиксированный максимум длины строки протокола (запрос и ответ).
#define MAX_LINE        220

// Максимум записей журнала встреч в одной странице ответа LOG_GET.
#define LOG_PAGE_MAX    6

// --- Метка Met в рекламе ---
// Manufacturer data: company ID (2, LE) | 'M' 'e' 't' | version | payload...
#define MET_COMPANY_ID   0xFFFF
#define MET_ADV_VERSION  1
#define MET_PAYLOAD_ANON 0x01   // анонимный ID (привязан и знает время)
#define MET_PAYLOAD_PAIR 0x02   // номер устройства (режим привязки)

// --- Радиопараметры (стартовые, настраиваются по BLE) ---
#define ADV_INTERVAL_MS_DEFAULT   200    // интервал рекламы, мс (чаще = быстрее находят)
#define ADV_TX_POWER_DBM_DEFAULT  0      // мощность передатчика, dBm (снижено: +4dBm при 90%
                                         // duty скана давал brown-out/сброс на USB-питании)
#define SCAN_WINDOW_MS_DEFAULT    3000   // окно скана, мс
#define SCAN_PERIOD_MS_DEFAULT    10000  // период скана, мс (30% duty: стабильно, без перегрева/сброса)

// Порог RSSI (dBm): слабее — не пишем. Старт: -90 (ловить и дальние/слабые пакеты).
#define RSSI_THRESHOLD_DEFAULT (-90)

// Склейка одного и того же ID, увиденного снова в пределах 2 мин.
#define ENCOUNTER_MERGE_MS 120000

// Эпоха анонимного ID: 900 с (15 мин).
#define ANON_EPOCH_SECONDS 900

// Защита от перебора: после N неверных ответов — пауза (мс).
#define AUTH_MAX_FAILS   3
#define AUTH_LOCKOUT_MS  30000

#endif
