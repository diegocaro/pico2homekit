#include "mibeacon.h"

#include <string.h>

#include "mbedtls/ccm.h"

// Frame control bits
#define FC_ENCRYPTED  0x0008
#define FC_MAC        0x0010
#define FC_CAPABILITY 0x0020
#define FC_OBJECT     0x0040

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

int mibeacon_frame_counter(const uint8_t *sd, size_t len) {
    return len < 5 ? -1 : sd[4];
}

bool mibeacon_frame_mac(const uint8_t *sd, size_t len, uint8_t mac[6]) {
    if (len < 11 || !(rd16(sd) & FC_MAC)) return false;
    for (int k = 0; k < 6; k++) mac[k] = sd[10 - k]; // stored LSB first at offset 5
    return true;
}

// Readings outside what the sensor can measure (or NaN/inf from the float objects)
// are dropped: they would end up as invalid JSON ("nan") or nonsense in HomeKit.
static bool temp_ok(float t) { return t >= -40.0f && t <= 100.0f; }   // false for NaN too
static bool hum_ok(float h) { return h >= 0.0f && h <= 100.0f; }

static void parse_objects(const uint8_t *p, size_t len, mibeacon_reading_t *out) {
    size_t i = 0;
    while (i + 3 <= len) {
        uint16_t type = rd16(p + i);
        uint8_t n = p[i + 2];
        const uint8_t *v = p + i + 3;
        if (i + 3 + n > len) break;
        switch (type) {
        case 0x1004: // temperature, int16 / 10
            if (n == 2) { out->temperature_c = (int16_t)rd16(v) / 10.0f; out->has_temperature = true; }
            break;
        case 0x1006: // humidity, uint16 / 10
            if (n == 2) { out->humidity_pct = rd16(v) / 10.0f; out->has_humidity = true; }
            break;
        case 0x100A: // battery %
        case 0x4803:
            if (n >= 1) { out->battery_pct = v[0]; out->has_battery = true; }
            break;
        case 0x100D: // temperature + humidity
            if (n == 4) {
                out->temperature_c = (int16_t)rd16(v) / 10.0f;
                out->humidity_pct = rd16(v + 2) / 10.0f;
                out->has_temperature = out->has_humidity = true;
            }
            break;
        case 0x4C01: // temperature, float32 (newer firmware)
            if (n == 4) { memcpy(&out->temperature_c, v, 4); out->has_temperature = true; }
            break;
        case 0x4C02: // humidity, uint8 (newer firmware)
            if (n == 1) { out->humidity_pct = v[0]; out->has_humidity = true; }
            break;
        default:
            break;
        }
        i += 3 + n;
    }
    if (out->has_temperature && !temp_ok(out->temperature_c)) out->has_temperature = false;
    if (out->has_humidity && !hum_ok(out->humidity_pct)) out->has_humidity = false;
    if (out->has_battery && out->battery_pct > 100) out->has_battery = false;
}

mibeacon_result_t mibeacon_parse(const uint8_t *sd, size_t len, const uint8_t mac[6],
                                 const uint8_t key[16], mibeacon_reading_t *out) {
    memset(out, 0, sizeof(*out));
    if (len < 5) return MIBEACON_MALFORMED;

    uint16_t fc = rd16(sd);
    unsigned version = fc >> 12;
    out->frame_counter = sd[4];
    size_t i = 5; // frame control (2), product id (2), frame counter (1)

    if (fc & FC_MAC) i += 6;
    if (fc & FC_CAPABILITY) {
        if (i >= len) return MIBEACON_MALFORMED;
        uint8_t cap = sd[i++];
        if (cap & 0x20) i += 2; // IO capability
    }
    if (!(fc & FC_OBJECT)) return MIBEACON_NO_DATA;
    if (i > len) return MIBEACON_MALFORMED;

    if (!(fc & FC_ENCRYPTED)) {
        // A sensor that has a bind key always encrypts. Accepting plaintext frames
        // for it would let anyone in Bluetooth range inject readings.
        if (key) return MIBEACON_NOT_ENCRYPTED;
        parse_objects(sd + i, len - i, out);
    } else {
        if (version < 4) return MIBEACON_UNSUPPORTED;
        if (!key) return MIBEACON_NO_KEY;
        // [ciphertext ...][ext counter: 3][MIC: 4]
        if (len < i + 1 + 3 + 4) return MIBEACON_MALFORMED;
        size_t clen = len - i - 7;
        const uint8_t *ext = sd + len - 7;
        const uint8_t *mic = sd + len - 4;

        // Nonce: MAC (LSB first) | product id (2) | frame counter (1) | ext counter (3)
        uint8_t nonce[12];
        for (int k = 0; k < 6; k++) nonce[k] = mac[5 - k];
        memcpy(nonce + 6, sd + 2, 3);
        memcpy(nonce + 9, ext, 3);
        static const uint8_t aad[1] = {0x11};

        uint8_t plain[32];
        if (clen > sizeof(plain)) return MIBEACON_MALFORMED;

        mbedtls_ccm_context ccm;
        mbedtls_ccm_init(&ccm);
        int rc = mbedtls_ccm_setkey(&ccm, MBEDTLS_CIPHER_ID_AES, key, 128);
        if (rc == 0)
            rc = mbedtls_ccm_auth_decrypt(&ccm, clen, nonce, sizeof(nonce), aad, sizeof(aad),
                                          sd + i, plain, mic, 4);
        mbedtls_ccm_free(&ccm);
        if (rc != 0) return MIBEACON_DECRYPT_FAILED;
        // 32-bit packet counter, authenticated as part of the nonce: lets the
        // caller reject replayed frames.
        out->counter = (uint32_t)sd[4] | (uint32_t)ext[0] << 8 | (uint32_t)ext[1] << 16 | (uint32_t)ext[2] << 24;
        out->has_counter = true;
        parse_objects(plain, clen, out);
    }

    if (!out->has_temperature && !out->has_humidity && !out->has_battery) return MIBEACON_NO_DATA;
    return MIBEACON_OK;
}

const char *mibeacon_result_str(mibeacon_result_t r) {
    switch (r) {
    case MIBEACON_OK: return "ok";
    case MIBEACON_NO_DATA: return "no data";
    case MIBEACON_MALFORMED: return "malformed";
    case MIBEACON_UNSUPPORTED: return "unsupported encryption";
    case MIBEACON_NO_KEY: return "no key";
    case MIBEACON_DECRYPT_FAILED: return "decryption failed (wrong key?)";
    case MIBEACON_NOT_ENCRYPTED: return "unencrypted frame for a sensor with a bind key (ignored)";
    }
    return "?";
}
