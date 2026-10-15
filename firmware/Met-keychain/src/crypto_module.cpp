#include "crypto_module.h"
#include "config.h"
#include "Adafruit_nRFCrypto.h"

static nRFCrypto_Hmac   s_hmac;
static nRFCrypto_Hash   s_hash;
static nRFCrypto_Random s_rnd;
static bool             s_ready = false;

void CryptoModule::begin() {
    nRFCrypto.begin();
    s_rnd.begin();
    s_ready = true;
}

bool CryptoModule::random(uint8_t* out, size_t len) {
    if (!out || len == 0) return false;
    if (!s_ready && !s_rnd.begin()) return false;
    size_t done = 0;
    while (done < len) {
        uint16_t chunk = (uint16_t)((len - done) > 255 ? 255 : (len - done));
        if (!s_rnd.generate(out + done, chunk)) return false;
        done += chunk;
    }
    return true;
}

bool CryptoModule::sha256(const uint8_t* data, size_t len, uint8_t out[32]) {
    uint32_t result[16];
    s_hash.begin(CRYS_HASH_SHA256_mode);
    s_hash.update((uint8_t*)data, len);
    uint8_t rlen = s_hash.end(result);
    if (rlen != 32) return false;
    memcpy(out, result, 32);
    return true;
}

bool CryptoModule::hmacSha256(const uint8_t* key, size_t keyLen,
                              const uint8_t* data, size_t dataLen,
                              uint8_t out[32]) {
    uint32_t result[16];
    if (!s_hmac.begin(CRYS_HASH_SHA256_mode, (uint8_t*)key, (uint16_t)keyLen)) return false;
    if (dataLen > 0 && !s_hmac.update((uint8_t*)data, dataLen)) return false;
    uint8_t rlen = s_hmac.end(result);
    if (rlen != 32) return false;
    memcpy(out, result, 32);
    return true;
}

bool CryptoModule::hmacSha256_2(const uint8_t* key, size_t keyLen,
                                const uint8_t* a, size_t aLen,
                                const uint8_t* b, size_t bLen,
                                uint8_t out[32]) {
    uint32_t result[16];
    if (!s_hmac.begin(CRYS_HASH_SHA256_mode, (uint8_t*)key, (uint16_t)keyLen)) return false;
    if (aLen > 0 && !s_hmac.update((uint8_t*)a, aLen)) return false;
    if (bLen > 0 && !s_hmac.update((uint8_t*)b, bLen)) return false;
    uint8_t rlen = s_hmac.end(result);
    if (rlen != 32) return false;
    memcpy(out, result, 32);
    return true;
}

// HKDF-SHA256 (RFC 5869): extract + expand. salt = 32 нулевых байт.
bool CryptoModule::hkdfSha256(const uint8_t* ikm, size_t ikmLen,
                              const char* info,
                              uint8_t* out, size_t outLen) {
    if (outLen == 0 || outLen > 255 * 32) return false;
    uint8_t salt[32];
    memset(salt, 0, sizeof(salt));

    uint8_t prk[32];
    if (!hmacSha256(salt, 32, ikm, ikmLen, prk)) return false;

    size_t infoLen = info ? strlen(info) : 0;
    uint8_t t[32];
    size_t tLen = 0;
    size_t done = 0;
    uint8_t counter = 1;
    while (done < outLen) {
        uint8_t block[32 + 64 + 1];
        size_t n = 0;
        if (tLen > 0) { memcpy(block + n, t, tLen); n += tLen; }
        if (infoLen > 0) {
            size_t cp = infoLen > 64 ? 64 : infoLen;
            memcpy(block + n, info, cp); n += cp;
        }
        block[n++] = counter;
        if (!hmacSha256(prk, 32, block, n, t)) return false;
        tLen = 32;
        size_t cp = outLen - done;
        if (cp > 32) cp = 32;
        memcpy(out + done, t, cp);
        done += cp;
        counter++;
    }
    return true;
}

bool CryptoModule::anonId(const uint8_t kDev[16], uint32_t epoch,
                          uint8_t out[16]) {
    uint8_t epochBe[4];
    epochBe[0] = (uint8_t)(epoch >> 24);
    epochBe[1] = (uint8_t)(epoch >> 16);
    epochBe[2] = (uint8_t)(epoch >> 8);
    epochBe[3] = (uint8_t)(epoch);
    uint8_t mac[32];
    if (!hmacSha256(kDev, 16, epochBe, 4, mac)) return false;
    memcpy(out, mac, 16);
    return true;
}
