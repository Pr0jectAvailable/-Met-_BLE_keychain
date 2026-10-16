# Прошивка брелока: что починить

Илья, привет. Здесь всё, что вылезло 15–16.10 на двух брелоках с прошивкой `alfa` (`554bb18 fast_fix`), и где,
по-моему, лежит исправление. Номера строк — по этому коммиту. Протокол v3 (команды, форматы, реклама, крипта)
приложение уже использует — его, пожалуйста, не меняй, а если меняешь, скажи мне.

Полное ревью от 16.10 — в `firmware-review.md` (отдельный файл). Тут — по приоритету, что нужно сделать.

## Что уже хорошо

`fast_fix` сработал: MET-00001 за сегодня несколько раз терял питание/зависал — **ключи и привязка ни разу не
слетели**. Привязка по QR, вход владельца, смена anon ID/MAC, запись встреч с подписью, склейка, чтение
журнала, UNPAIR — всё проверено на двух брелоках, оба видят друг друга и пишут встречи.

---

## P0 — до выставки (17.10)

### 1. Брелок намертво зависает и не перезагружается

**Что видели (16.10, MET-00001):** брелок на USB+батарее пропал из эфира. USB-порт есть (`nice_nano`
перечисляется), но в Serial **ни одной строки** — даже `HB up=…`, который `BLEModule::loop()` печатает
каждые 5 с (`src/ble_module.cpp:178`). Переподключение USB не
помогло (батарея держит питание). Ожил только после перепрошивки (DFU сделал reset); ключи на месте.

То есть это **не ребут, а зависание**: реклама тоже стоит, значит, скорее всего, HardFault/LOCKUP — обработчик
крутится в бесконечном цикле, а SoftDevice с меньшим приоритетом не работает. Вчерашние «самопроизвольные
перезагрузки» с потерей времени, вероятно, то же самое (или brown-out).

**Исправление в два слоя:**

**а) Watchdog — чтобы любое зависание стало перезагрузкой за несколько секунд.** В `setup()` (`src/main.cpp:110`), до `Bluefruit.begin()`:

```cpp
// WDT: если loop() не крутится 8 с — аппаратный сброс. Работает и при HardFault.
NRF_WDT->CONFIG = (WDT_CONFIG_SLEEP_Run << WDT_CONFIG_SLEEP_Pos) |
                  (WDT_CONFIG_HALT_Pause << WDT_CONFIG_HALT_Pos);
NRF_WDT->CRV    = 8 * 32768;          // 8 с при 32.768 кГц
NRF_WDT->RREN   = WDT_RREN_RR0_Msk;
NRF_WDT->TASKS_START = 1;
```

и в начале `loop()`:

```cpp
NRF_WDT->RR[0] = WDT_RR_RR_Reload;
```

Важно: WDT нельзя остановить до сброса. Проверь, что загрузчик nice!nano кормит WDT в режиме DFU (у
Adafruit bootloader ≥0.6 это есть) — иначе прошивка по USB будет обрываться каждые 8 с. Самое долгое,
что сейчас блокирует `loop()`, — запись во флеш (до ~3 с у `UNPAIR`), 8 с с запасом.

**б) Убрать причину зависания — гонки между задачами** (пункт 2 ревью). Сейчас команды телефона выполняются
прямо в `BLEUartModule::rxCallback` (`src/ble_uart_module.cpp:138`) —
это задача Bluefruit/SoftDevice, а `loop()` — другая задача. Обе без блокировок трогают:

- `txQueue` (`std::queue<String>`): push в rxCallback → pop в `txPump()` из loop. Одновременный доступ к
  `std::queue`/`String` портит кучу → HardFault;
- журнал `StorageModule::records[]`: `record()` из loop и `clear()`/`ackUpTo()`/`flush()` из rxCallback/onDisconnect;
- `s_rec` ключей и `save()` во флеш — из обеих задач;
- `TIME_SET` вызывает `BLEModule::loop()` прямо из rxCallback (`src/ble_uart_module.cpp:508`) —
  у очереди сканера становится два потребителя;
- `UNPAIR`, `PROVISION`, `FACTORY_RESET`, `UNBIND_FINISH` делают `restartAdvertising()` и запись во флеш из
  rxCallback — ровно то, что ты сам запретил в `onDisconnect` (комментарий в `src/ble_module.cpp:99`).

Решение — тот же приём, что ты уже сделал для сканера: **в колбэке только сложить строку, выполнять в `loop()`.**

```cpp
// ble_uart_module.cpp — колбэк только собирает строки в кольцо
#define CMD_RING 4
static char              s_cmd[CMD_RING][MAX_LINE + 1];
static volatile uint8_t  s_cmdHead = 0, s_cmdTail = 0;

void BLEUartModule::rxCallback(uint16_t) {
    // ...та же сборка cmdBuffer по '\n', но вместо processCommand(cmdBuffer):
    uint8_t next = (s_cmdHead + 1) % CMD_RING;
    if (next != s_cmdTail) {
        strncpy(s_cmd[s_cmdHead], cmdBuffer.c_str(), MAX_LINE);
        s_cmd[s_cmdHead][MAX_LINE] = '\0';
        __DMB();
        s_cmdHead = next;
    }   // иначе — переполнение, телефон получит тайм-аут и повторит
}

void BLEUartModule::loop() {
    while (s_cmdTail != s_cmdHead) {
        String line(s_cmd[s_cmdTail]);
        s_cmdTail = (s_cmdTail + 1) % CMD_RING;
        processCommand(line);          // теперь всё — в одной задаче с BLEModule/Storage
    }
    txPump();
    // ...watchdog транзакции как сейчас
}
```

- В `rxCallback` также убрать `String cmdBuffer` (static `String` в колбэке = аллокации в чужой задаче) →
  `static char buf[MAX_LINE+1]; static size_t len;`.
- В `TIME_SET` убрать `BLEModule::loop();` — вместо него `BLEModule::requestRadioRestart();` (перестроит
  рекламу на anon ID в loop).
- В `UNPAIR`/`PROVISION`/`FACTORY_RESET*`/`UNBIND_FINISH` `restartAdvertising()` → `requestRadioRestart()`.
- `onDisconnect` (`src/ble_module.cpp:92`): `StorageModule::flush()` и
  `resetSession()` тоже перенести в loop — поставить флаг `s_disconnectPending`, обработать в `BLEModule::loop()`.
- `cmdBuffer` чистить в `resetSession()` (сейчас обрывок строки прошлого телефона приклеивается к первой
  команде следующего).

После этого `processCommand`, `record`, `save`, реклама и `txQueue` живут в одной задаче, гонок нет.

### 2. Возможное зависание при отключении USB (гипотеза, проверить)

Несколько раз брелок пропадал из эфира сразу после того, как USB вытаскивали, когда на Маке был открыт порт
(монитор / скрипты). На nRF52 + TinyUSB `Serial.printf()` при выдернутом кабеле может застрять: хост не
успел снять DTR, `tud_cdc_connected()` остаётся true, запись ждёт место в буфере вечно. С WDT это станет
перезагрузкой, но лучше:

- в релизной сборке `ENABLE_SERIAL 0` и `ENABLE_UART_MIRROR 0` (`headers/config.h:8`) —
  это ещё и −0.5…1 мА батареи;
- в отладочной — писать только если есть место: `if (Serial && Serial.availableForWrite() > 64) Serial.printf(...)`
  (обернуть в макрос `LOG`).

Проверка: брелок на батарее + USB, открыть `pio device monitor`, выдернуть кабель, через минуту — виден ли в эфире.

### 3. После любой перезагрузки брелок теряет время и перестаёт писать встречи

Время живёт только в RAM (`src/keystore_module.cpp:232`), а вернуть его
может только сервер (`TIME_SET`), которого пока нет. Сейчас после сброса брелок нужно перепривязывать в
приложении. Даже с сервером синк должен случиться, прежде чем брелок снова начнёт писать.

Предложение: **переживать мягкий сброс (WDT/LOCKUP/soft), не трогая безопасность** — хранить время в RAM,
которая не обнуляется при сбросе (без питания она всё равно сотрётся, так что подделать время извне нельзя):

```cpp
// .noinit — не обнуляется стартовым кодом (проверь, что секция есть в linker script ядра)
struct RetainedClock { uint32_t magic, utc, crc; };
__attribute__((section(".noinit"))) static RetainedClock s_keep;

// раз в ~1 с в loop():  s_keep = {MAGIC, KeystoreModule::utc(), crc(...)};
// в setup(), если RESETREAS = DOG | LOCKUP | SREQ (НЕ power-on/RESETPIN) и magic/crc сходятся:
//     KeystoreModule::setUtc(s_keep.utc + 2);   // + время на перезагрузку
```

Тогда зависание → WDT → брелок через ~10 с снова в эфире с anon ID и пишет встречи, без телефона и сервера.

### 4. Флеш-запись на каждую встречу

`StorageModule::record()` → `KeystoreModule::nextSeq()` → `save()` ключей (`src/keystore_module.cpp:217`)
на **каждую** новую запись. Двухслотовая запись сделала это безопасным, но это лишние секунды блокировки,
износ флеша и окно для гонок. Резервировать `seq` пачкой:

```cpp
// в KeyRecord: seqReserved (вместо сохранения seq на каждый вызов)
uint32_t KeystoreModule::nextSeq() {
    uint32_t s = s_seq++;                       // s_seq — только в RAM
    if (s_seq >= s_rec.seqReserved) {           // кончился резерв — записываем следующий блок
        s_rec.seqReserved = s_seq + 32;
        save();
    }
    return s;
}
// load(): s_seq = s_rec.seqReserved;  (после сброса пропускаем ≤32 номера — серверу нужна только монотонность)
```