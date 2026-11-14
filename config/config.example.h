// Copy to config/config.h (git-ignored) and fill in.
#pragma once

#define WIFI_SSID     "your-wifi"
#define WIFI_PASSWORD "your-password"

// HomeKit: the 8-digit code you type in the Home app (format XXX-XX-XXX; pick
// your own, trivial codes like 123-45-678 or 111-11-111 are rejected).
#define HAP_SETUP_CODE  "531-27-864"
#define HAP_BRIDGE_NAME "Pico Bridge"

// One entry per thermometer: { name, MAC address, 32-hex-char bind key }
#define SENSOR_LIST \
    { "Living Room", "A4:C1:38:00:00:01", "00112233445566778899aabbccddeeff" }, \
    { "Bedroom",     "A4:C1:38:00:00:02", "00112233445566778899aabbccddeeff" },
