#include <Arduino.h>
#include <bluefruit.h>
#include <InternalFileSystem.h>
#include "config.h"

using namespace Adafruit_LittleFS_Namespace;
#include "crypto_module.h"
#include "keystore_module.h"
#include "storage_module.h"
#include "ble_module.h"
#include "ble_uart_module.h"

static uint32_t g_resetReas = 0;
static char g_resetStr[96] = {0};
static uint32_t g_bootCount = 0;

// --- Время, переживающее мягкий сброс -------------------------------------
// .noinit не обнуляется стартовым кодом; при потере питания секция всё равно
// стирается, поэтому подделать время извне нельзя. См. firmware-fixes.md P0-3.
struct RetainedClock { uint32_t magic, utc, crc; };
__attribute__((section(".noinit"))) static RetainedClock s_keep;
static const uint32_t KEEP_MAGIC = 0x4D455443; // 'METC'
static uint32_t keepCrc(uint32_t utc) {
    return (KEEP_MAGIC ^ utc) * 2654435761u + 0x9E3779B9u;
}

// Persistent boot counter in InternalFS: survives power loss (unlike RAM),
// so a rapidly increasing value proves the MCU is reset-looping.
static void bumpBootCount() {
    static const char* BOOT_FILE = "/bootcnt";
    uint32_t n = 0;
    File f = InternalFS.open(BOOT_FILE, FILE_O_READ);
    if (f) {
        f.read((uint8_t*)&n, sizeof(n));
        f.close();
    }
    n++;
    g_bootCount = n;
    InternalFS.remove(BOOT_FILE);
    File w = InternalFS.open(BOOT_FILE, FILE_O_WRITE);
    if (w) {
        w.write((uint8_t*)&n, sizeof(n));
        w.close();
    }
}

static void logResetReason() {
    uint32_t reas = NRF_POWER->RESETREAS;
    g_resetReas = reas;
    NRF_POWER->RESETREAS = reas;  // clear on write (write 1 to clear)

    size_t n = 0;
    n += snprintf(g_resetStr + n, sizeof(g_resetStr) - n,
                  "reset_reas=0x%08X", (unsigned)reas);
    if (reas & POWER_RESETREAS_RESETPIN_Msk)  n += snprintf(g_resetStr + n, sizeof(g_resetStr) - n, " PIN");
    if (reas & POWER_RESETREAS_DOG_Msk)       n += snprintf(g_resetStr + n, sizeof(g_resetStr) - n, " WDT");
    if (reas & POWER_RESETREAS_SREQ_Msk)      n += snprintf(g_resetStr + n, sizeof(g_resetStr) - n, " SOFT");
    if (reas & POWER_RESETREAS_LOCKUP_Msk)    n += snprintf(g_resetStr + n, sizeof(g_resetStr) - n, " LOCKUP");
    if (reas & POWER_RESETREAS_OFF_Msk)       n += snprintf(g_resetStr + n, sizeof(g_resetStr) - n, " OFF");
    if (reas & POWER_RESETREAS_LPCOMP_Msk)    n += snprintf(g_resetStr + n, sizeof(g_resetStr) - n, " LPCOMP");
    if (reas & POWER_RESETREAS_DIF_Msk)       n += snprintf(g_resetStr + n, sizeof(g_resetStr) - n, " DIF");
    if (reas & POWER_RESETREAS_NFC_Msk)       n += snprintf(g_resetStr + n, sizeof(g_resetStr) - n, " NFC");
    snprintf(g_resetStr + n, sizeof(g_resetStr) - n, " gpregret=0x%08X",
             (unsigned)NRF_POWER->GPREGRET);

    LOGLN(g_resetStr);
    ULOGLN(g_resetStr);
}

// --- Демо-команды по последовательному порту (USB CDC и UART-зеркало) ---
// Без аутентификации: "UNBIND" отвязывает владельца (K_owner, флаг paired,
// журнал, RAM-часы), но СОХРАНЯЕТ провизьон (devNo/K_dev/K_pair) и seq, чтобы
// брелок можно было привязать заново. "FACTORY_RESET" стирает и провизьон.
static void handleSerialLine(const char* raw) {
    char line[32];
    size_t n = 0;
    for (const char* p = raw; *p && n < sizeof(line) - 1; p++) {
        char c = *p;
        if (c >= 'a' && c <= 'z') c -= 32;
        line[n++] = c;
    }
    line[n] = '\0';

    if (strcmp(line, "UNBIND") == 0) {
        LOGLN("SERIAL CMD: UNBIND");
        ULOGLN("SERIAL CMD: UNBIND");
        KeystoreModule::unbind();
        StorageModule::clear();
        BLEModule::restartAdvertising();
        LOGLN("SERIAL CMD: owner unbound (provision kept)");
        ULOGLN("SERIAL CMD: owner unbound (provision kept)");
    } else if (strcmp(line, "FACTORY_RESET") == 0) {
        LOGLN("SERIAL CMD: FACTORY_RESET");
        ULOGLN("SERIAL CMD: FACTORY_RESET");
        KeystoreModule::factoryReset();
        StorageModule::clear();
        BLEModule::restartAdvertising();
        LOGLN("SERIAL CMD: factory reset done");
        ULOGLN("SERIAL CMD: factory reset done");
    } else if (strcmp(line, "HELP") == 0) {
        ULOGLN("SERIAL CMD: UNBIND | FACTORY_RESET");
        LOGLN("SERIAL CMD: UNBIND | FACTORY_RESET");
    }
}

static void pollSerialCommand(Stream& s) {
    static String buf;
    while (s.available()) {
        char c = (char)s.read();
        if (c == '\n' || c == '\r') {
            buf.trim();
            if (buf.length() > 0) handleSerialLine(buf.c_str());
            buf = "";
        } else if (buf.length() < 32) {
            buf += c;
        }
    }
}

void setup() {
#if ENABLE_UART_MIRROR
    Serial1.setPins(UART_MIRROR_TX_PIN, UART_MIRROR_RX_PIN);
    Serial1.begin(115200);
#endif
    logResetReason();  // captured for both Serial and the UART mirror

#if ENABLE_SERIAL
    Serial.begin(115200);
    delay(1500);  // wait for USB CDC to enumerate before first logs
    LOGLN("=== BOOT ===");
    LOGLN(g_resetStr);
#endif

    InternalFS.begin();
    bumpBootCount();
    KeystoreModule::begin();

    // Восстанавливаем время после мягкого сброса (WDT/LOCKUP/SOFT), но НЕ
    // после power-on/RESETPIN: g_resetReas прочитан и очищен выше.
    {
        uint32_t soft = POWER_RESETREAS_DOG_Msk | POWER_RESETREAS_LOCKUP_Msk |
                        POWER_RESETREAS_SREQ_Msk;
        if ((g_resetReas & soft) && s_keep.magic == KEEP_MAGIC &&
            s_keep.crc == keepCrc(s_keep.utc)) {
            KeystoreModule::setUtc(s_keep.utc + 2);  // + время на перезагрузку
            LOGLN("Clock: restored from .noinit after soft reset");
            ULOGLN("Clock: restored from .noinit after soft reset");
        }
    }

    StorageModule::begin();
    CryptoModule::begin();

#ifdef FACTORY_WIPE_ON_BOOT
    // One-shot recovery build (env:recovery): wipe provision/owner/journal on
    // boot. DFU upload does NOT erase InternalFS, so a paired unit whose K_dev
    // is unknown can only be freed this way without an SWD probe. Flash the
    // normal firmware afterwards.
    KeystoreModule::factoryReset();
    StorageModule::clear();
    LOGLN("FACTORY_WIPE_ON_BOOT: keys + journal cleared");
#endif

    // WDT: если loop() не крутится 8 с — аппаратный сброс. Работает и при
    // HardFault/LOCKUP. Кормим в loop(); остановить до сброса нельзя.
    NRF_WDT->CONFIG = (WDT_CONFIG_SLEEP_Run << WDT_CONFIG_SLEEP_Pos) |
                      (WDT_CONFIG_HALT_Pause << WDT_CONFIG_HALT_Pos);
    NRF_WDT->CRV    = 8 * 32768;          // 8 с при 32.768 кГц
    NRF_WDT->RREN   = WDT_RREN_RR0_Msk;
    NRF_WDT->TASKS_START = 1;

    // ATT MTU: должен быть задан до Bluefruit.begin().
    Bluefruit.configPrphConn(247, BLE_GAP_EVENT_LENGTH_DEFAULT,
                             BLE_GATTS_HVN_TX_QUEUE_SIZE_DEFAULT,
                             BLE_GATTC_WRITE_CMD_TX_QUEUE_SIZE_DEFAULT);
    Bluefruit.begin(1, 1);

    // Регистрируем GATT-сервис UART ДО старта рекламы: иначе стартуем
    // advertising раньше, чем сервис добавлен в GATT-таблицу.
    BLEUartModule::begin();
    BLEModule::begin();
}

void loop() {
    // Кормим WDT: если loop() встанет дольше 8 с — аппаратный сброс.
    NRF_WDT->RR[0] = WDT_RR_RR_Reload;

    // Раз в ~1 с обновляем .noinit-копию времени для восстановления после
    // мягкого сброса (см. firmware-fixes.md P0-3).
    static uint32_t s_keepMs = 0;
    if (millis() - s_keepMs > 1000) {
        s_keepMs = millis();
        if (KeystoreModule::timeValid()) {
            uint32_t u = KeystoreModule::utc();
            s_keep.magic = KEEP_MAGIC;
            s_keep.utc = u;
            s_keep.crc = keepCrc(u);
        }
    }

    BLEUartModule::loop();
    BLEModule::loop();
    StorageModule::loop();

    static bool bootLogged = false;
    if (!bootLogged) {
        bootLogged = true;
        ULOGLN("====================");
        ULOGLN("=== BOOT ===");
        ULOGLN(g_resetStr);
        ULOG("bootcnt=%u\n", (unsigned)g_bootCount);
        ULOG("fw=%s proto=%u\n", FW_VERSION, PROTO_VERSION);
        ULOG("dev=%s paired=%u timeok=%u\n", KeystoreModule::devNo(),
             KeystoreModule::paired(), KeystoreModule::timeValid());
        ULOG("encounters=%d seq=%u\n",
             StorageModule::count(), (unsigned)KeystoreModule::peekSeq());
        ULOG("rssiThr=%d adv=%ums/%ddBm scan=%u/%ums\n",
             KeystoreModule::rssiThreshold(),
             KeystoreModule::advIntervalMs(), KeystoreModule::advTxPowerDbm(),
             KeystoreModule::scanWindowMs(), KeystoreModule::scanPeriodMs());
        ULOGLN("====================");

#if ENABLE_SERIAL
        LOGLN("====================");
        LOGLN("=== BOOT ===");
        LOGLN(g_resetStr);
        LOG("bootcnt=%u\n", (unsigned)g_bootCount);
        LOG("fw=%s proto=%u\n", FW_VERSION, PROTO_VERSION);
        LOG("dev=%s paired=%u timeok=%u\n", KeystoreModule::devNo(),
            KeystoreModule::paired(), KeystoreModule::timeValid());
        LOG("encounters=%d seq=%u\n",
            StorageModule::count(), (unsigned)KeystoreModule::peekSeq());
        LOG("rssiThr=%d adv=%ums/%ddBm scan=%u/%ums\n",
            KeystoreModule::rssiThreshold(),
            KeystoreModule::advIntervalMs(), KeystoreModule::advTxPowerDbm(),
            KeystoreModule::scanWindowMs(), KeystoreModule::scanPeriodMs());
        LOGLN("====================");
#endif
    }

    delay(2);
}
