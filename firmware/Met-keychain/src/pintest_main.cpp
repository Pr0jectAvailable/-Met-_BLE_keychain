// Minimal hardware sanity test — NO BLE / SoftDevice involved.
// Selected by build_src_filter in env:pintest (all other src files excluded).

#include <Arduino.h>
#include <Adafruit_TinyUSB.h>  // force LDF to link the CDC/Serial implementation

static void printResetReason() {
    uint32_t r = NRF_POWER->RESETREAS;
    NRF_POWER->RESETREAS = r;  // write 1 to clear
    Serial.print("reset_reas=0x");
    Serial.println(r, HEX);
    if (r & POWER_RESETREAS_RESETPIN_Msk) Serial.println("  PIN");
    if (r & POWER_RESETREAS_DOG_Msk)      Serial.println("  WDT");
    if (r & POWER_RESETREAS_SREQ_Msk)     Serial.println("  SOFT");
    if (r & POWER_RESETREAS_LOCKUP_Msk)   Serial.println("  LOCKUP");
    if (r & POWER_RESETREAS_OFF_Msk)      Serial.println("  OFF");
    if (r & POWER_RESETREAS_DIF_Msk)      Serial.println("  DIF");
}

void setup() {
    Serial.begin(115200);
    uint32_t t0 = millis();
    while (!Serial && (millis() - t0) < 3000) { delay(10); }

    pinMode(LED_BUILTIN, OUTPUT);
    pinMode(6, OUTPUT);   // D0 / P0.06
    pinMode(8, OUTPUT);   // D1 / P0.08
    pinMode(13, OUTPUT);  // commonly LED on nRF52 clones

    Serial.println();
    Serial.println("=== Met PIN TEST (no BLE) ===");
    printResetReason();
}

void loop() {
    static uint32_t n = 0;
    bool on = (n & 1);
    digitalWrite(LED_BUILTIN, on);
    digitalWrite(6, on);
    digitalWrite(8, on);
    digitalWrite(13, on);
    if ((n & 1) == 0) {
        Serial.print("tick ");
        Serial.println(n >> 1);
    }
    n++;
    delay(500);  // 1 Hz square wave
}
