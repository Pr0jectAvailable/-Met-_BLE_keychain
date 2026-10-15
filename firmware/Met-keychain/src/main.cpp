#include <Arduino.h>
#include <bluefruit.h>
#include "config.h"
#include "crypto_module.h"
#include "keystore_module.h"
#include "storage_module.h"
#include "ble_module.h"
#include "ble_uart_module.h"

static uint32_t g_resetReas = 0;
static char g_resetStr[64] = {0};

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

    LOGLN(g_resetStr);
    ULOGLN(g_resetStr);
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
    KeystoreModule::begin();
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

    // ATT MTU: должен быть задан до Bluefruit.begin().
    Bluefruit.configPrphConn(247, BLE_GAP_EVENT_LENGTH_DEFAULT,
                             BLE_GATTS_HVN_TX_QUEUE_SIZE_DEFAULT,
                             BLE_GATTC_WRITE_CMD_TX_QUEUE_SIZE_DEFAULT);
    Bluefruit.begin(1, 1);

    BLEModule::begin();
    BLEUartModule::begin();
}

void loop() {
    BLEUartModule::loop();
    BLEModule::loop();
    StorageModule::loop();

    static bool bootLogged = false;
    if (!bootLogged) {
        bootLogged = true;
        ULOGLN("====================");
        ULOGLN("=== BOOT ===");
        ULOGLN(g_resetStr);
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
