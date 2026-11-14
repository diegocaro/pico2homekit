// Fake configuration for the host HAP test server (tests/hap_host.c).
#pragma once

#define WIFI_SSID     "unused"
#define WIFI_PASSWORD "unused"
#define HAP_SETUP_CODE "482-91-375"
#define HAP_BRIDGE_NAME "Test Bridge"

#define SENSOR_LIST \
    { "Living",  "A4:C1:38:00:00:01", "00112233445566778899aabbccddeeff" }, \
    { "Bedroom", "A4:C1:38:00:00:02", "00112233445566778899aabbccddeeff" },
