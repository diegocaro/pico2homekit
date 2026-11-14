// Copy to config/config.h (git-ignored) and fill in.
#pragma once

#define WIFI_SSID     "your-wifi"
#define WIFI_PASSWORD "your-password"

// One entry per thermometer: { name, MAC address, 32-hex-char bind key }
#define SENSOR_LIST \
    { "Living Room", "A4:C1:38:00:00:01", "00112233445566778899aabbccddeeff" }, \
    { "Bedroom",     "A4:C1:38:00:00:02", "00112233445566778899aabbccddeeff" },
