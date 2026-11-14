// Host unit test for the MiBeacon parser: `make test`.
// Vectors from github.com/Bluetooth-Devices/xiaomi-ble tests (MIT).
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "mibeacon.h"

static int failures;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static void test_lywsd03mmc_encrypted(void) {
    // Service data (after 0xFE95) from a LYWSD03MMC, MiBeacon v5 encrypted.
    const uint8_t sd[] = "XX[\x05P\xf4\x83\x02" "8\xc1\xa4\x95\xefXv<&\x00\x00\x97\xe2\xab\xb5";
    const uint8_t mac[6] = {0xA4, 0xC1, 0x38, 0x02, 0x83, 0xF4};
    const uint8_t key[16] = {0xe9, 0xea, 0x89, 0x5f, 0xac, 0x7c, 0xca, 0x6d,
                             0x30, 0x53, 0x24, 0x32, 0xa5, 0x16, 0xf3, 0xa8};
    mibeacon_reading_t r;

    CHECK(mibeacon_parse(sd, sizeof(sd) - 1, mac, key, &r) == MIBEACON_OK);
    // Decrypts to object 0x1006, raw 467 -> 46.7 % (xiaomi-ble truncates this to 46).
    CHECK(r.has_humidity && fabsf(r.humidity_pct - 46.7f) < 0.01f);
    CHECK(!r.has_temperature);

    uint8_t bad_key[16];
    memcpy(bad_key, key, 16);
    bad_key[0] ^= 1;
    CHECK(mibeacon_parse(sd, sizeof(sd) - 1, mac, bad_key, &r) == MIBEACON_DECRYPT_FAILED);
    CHECK(mibeacon_parse(sd, sizeof(sd) - 1, mac, NULL, &r) == MIBEACON_NO_KEY);
    CHECK(mibeacon_parse(sd, 4, mac, key, &r) == MIBEACON_MALFORMED);
}

static void test_unencrypted_objects(void) {
    // fc=0x5040 (v5, object included, not encrypted), pid 0x055B, counter 7,
    // object 0x100D temp 23.4 C / hum 51.2 %, object 0x100A batt 87 %
    const uint8_t sd[] = {0x40, 0x50, 0x5B, 0x05, 0x07,
                          0x0D, 0x10, 0x04, 0xEA, 0x00, 0x00, 0x02,
                          0x0A, 0x10, 0x01, 87};
    const uint8_t mac[6] = {0};
    mibeacon_reading_t r;
    CHECK(mibeacon_parse(sd, sizeof(sd), mac, NULL, &r) == MIBEACON_OK);
    CHECK(r.has_temperature && fabsf(r.temperature_c - 23.4f) < 0.01f);
    CHECK(r.has_humidity && fabsf(r.humidity_pct - 51.2f) < 0.01f);
    CHECK(r.has_battery && r.battery_pct == 87);
    CHECK(r.frame_counter == 7);
}

int main(void) {
    test_lywsd03mmc_encrypted();
    test_unencrypted_objects();
    printf(failures ? "%d failure(s)\n" : "all tests passed\n", failures);
    return failures != 0;
}
