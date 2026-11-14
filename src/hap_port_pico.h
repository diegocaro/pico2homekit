// Pico W platform layer for the HAP core: lwIP TCP server, mDNS (_hap._tcp)
// and flash storage.
#pragma once

#include <stdint.h>

#define HAP_TCP_PORT 5001

// Start listening and advertising. Call once Wi-Fi has an IP address.
void hap_server_start(const char *name);
// Call from the main loop: performs deferred connection closes.
void hap_server_poll(void);
