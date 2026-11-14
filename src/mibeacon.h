// Xiaomi MiBeacon (BLE service data UUID 0xFE95) parser with v4/v5 AES-CCM decryption.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MIBEACON_UUID 0xFE95

typedef struct {
    uint8_t frame_counter;
    bool has_temperature;
    bool has_humidity;
    bool has_battery;
    float temperature_c;
    float humidity_pct;
    uint8_t battery_pct;
} mibeacon_reading_t;

typedef enum {
    MIBEACON_OK = 0,
    MIBEACON_NO_DATA,        // valid frame without sensor objects (e.g. pairing beacon)
    MIBEACON_MALFORMED,
    MIBEACON_UNSUPPORTED,    // encrypted with v2/v3 scheme
    MIBEACON_NO_KEY,         // encrypted but no key supplied
    MIBEACON_DECRYPT_FAILED, // wrong key or corrupted frame
} mibeacon_result_t;

// Frame counter without parsing (cheap pre-check for de-duplication).
// Returns -1 if the frame is too short.
int mibeacon_frame_counter(const uint8_t *sd, size_t len);

// If the frame carries the device MAC, writes it (most-significant byte first) and returns true.
// Needed where the BLE stack hides addresses (macOS CoreBluetooth).
bool mibeacon_frame_mac(const uint8_t *sd, size_t len, uint8_t mac[6]);

// sd/len: service data *after* the 2-byte 0xFE95 UUID (starts at frame control).
// mac: device address, most-significant byte first (as printed: A4:C1:38:...).
// key: 16-byte bind key, or NULL.
mibeacon_result_t mibeacon_parse(const uint8_t *sd, size_t len, const uint8_t mac[6],
                                 const uint8_t key[16], mibeacon_reading_t *out);

const char *mibeacon_result_str(mibeacon_result_t r);
