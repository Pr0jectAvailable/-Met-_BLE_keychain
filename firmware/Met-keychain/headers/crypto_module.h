#ifndef CRYPTO_MODULE_H
#define CRYPTO_MODULE_H

#include <Arduino.h>

// Криптография брелока: HMAC-SHA256, HKDF-SHA256, SHA-256, аппаратный ГСЧ.
// Использует Adafruit_nRFCrypto (CryptoCell CC310 на nRF52840).
class CryptoModule {
public:
    static void begin();

    static bool random(uint8_t* out, size_t len);
    static bool sha256(const uint8_t* data, size_t len, uint8_t out[32]);
    static bool hmacSha256(const uint8_t* key, size_t keyLen,
                           const uint8_t* data, size_t dataLen,
                           uint8_t out[32]);
    static bool hmacSha256_2(const uint8_t* key, size_t keyLen,
                             const uint8_t* a, size_t aLen,
                             const uint8_t* b, size_t bLen,
                             uint8_t out[32]);
    static bool hkdfSha256(const uint8_t* ikm, size_t ikmLen,
                           const char* info,
                           uint8_t* out, size_t outLen);
    static bool anonId(const uint8_t kDev[16], uint32_t epoch,
                       uint8_t out[16]);
};

#endif
