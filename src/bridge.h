// HomeKit accessory layout: a bridge (aid 1) plus one accessory per
// thermometer (aid 2..) with temperature, humidity and battery services.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define BRIDGE_MAX_SENSORS 8   // 8 notification slots each in a 64-bit mask

void bridge_init(const char *bridge_name);
// Call after a sensor's readings changed; sends HomeKit events for changed values.
void bridge_sensor_updated(int sensor_index);
// Set when a controller asks the accessory to identify itself (blink LED).
bool bridge_take_identify_request(void);
