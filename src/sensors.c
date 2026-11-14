#include "sensors.h"

#include <stdio.h>
#include <string.h>

#include "config.h"

typedef struct { const char *name, *mac, *key; } sensor_cfg_t;
static const sensor_cfg_t cfg[] = { SENSOR_LIST };
#define N_SENSORS ((int)(sizeof(cfg) / sizeof(cfg[0])))

static sensor_t sensors[N_SENSORS];

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Reads `n` bytes of hex from `s`, skipping ':' and '-' separators.
static bool parse_hex(const char *s, uint8_t *out, int n) {
    int got = 0;
    while (*s && got < n) {
        if (*s == ':' || *s == '-' || *s == ' ') { s++; continue; }
        int hi = hexval(s[0]), lo = s[1] ? hexval(s[1]) : -1;
        if (hi < 0 || lo < 0) return false;
        out[got++] = (uint8_t)(hi << 4 | lo);
        s += 2;
    }
    while (*s == ':' || *s == '-' || *s == ' ') s++;
    return got == n && *s == '\0';
}

bool sensors_init(void) {
    for (int i = 0; i < N_SENSORS; i++) {
        sensor_t *s = &sensors[i];
        memset(s, 0, sizeof(*s));
        s->name = cfg[i].name;
        s->last_frame = -1;
        if (!parse_hex(cfg[i].mac, s->mac, 6)) {
            printf("config: bad MAC for '%s': %s\n", cfg[i].name, cfg[i].mac);
            return false;
        }
        if (!parse_hex(cfg[i].key, s->key, 16)) {
            printf("config: bad bind key for '%s' (need 32 hex chars)\n", cfg[i].name);
            return false;
        }
    }
    return true;
}

int sensors_count(void) { return N_SENSORS; }
sensor_t *sensors_get(int i) { return (i >= 0 && i < N_SENSORS) ? &sensors[i] : NULL; }

sensor_t *sensors_find(const uint8_t mac[6]) {
    for (int i = 0; i < N_SENSORS; i++)
        if (memcmp(sensors[i].mac, mac, 6) == 0) return &sensors[i];
    return NULL;
}

bool sensors_update(sensor_t *s, const mibeacon_reading_t *r, uint32_t now_ms) {
    bool changed = false;
    s->seen = true;
    s->last_update_ms = now_ms;
    if (r->has_temperature && (!s->has_temperature || s->temperature_c != r->temperature_c)) {
        s->temperature_c = r->temperature_c; s->has_temperature = true; changed = true;
    }
    if (r->has_humidity && (!s->has_humidity || s->humidity_pct != r->humidity_pct)) {
        s->humidity_pct = r->humidity_pct; s->has_humidity = true; changed = true;
    }
    if (r->has_battery && (!s->has_battery || s->battery_pct != r->battery_pct)) {
        s->battery_pct = r->battery_pct; s->has_battery = true; changed = true;
    }
    return changed;
}
