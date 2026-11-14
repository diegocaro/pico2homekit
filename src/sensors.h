// Configured thermometers and their latest readings.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "mibeacon.h"

typedef struct {
    const char *name;
    uint8_t mac[6];   // most-significant byte first
    uint8_t key[16];
    // latest state
    bool seen;
    int last_frame;   // -1 = none yet
    bool has_counter; // last authenticated 32-bit packet counter (replay protection)
    uint32_t last_counter;
    uint32_t last_update_ms;
    float temperature_c;
    float humidity_pct;
    uint8_t battery_pct;
    bool has_temperature, has_humidity, has_battery;
} sensor_t;

// Parses SENSOR_LIST from config.h. Returns false (and prints why) on a bad entry.
bool sensors_init(void);
int sensors_count(void);
sensor_t *sensors_get(int i);
sensor_t *sensors_find(const uint8_t mac[6]);
// Merge a decoded reading. Returns true if any value changed.
bool sensors_update(sensor_t *s, const mibeacon_reading_t *r, uint32_t now_ms);
