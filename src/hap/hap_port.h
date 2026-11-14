// Platform layer the HAP core needs. Implemented by hap_port_pico.c on the
// Pico W and by tests/hap_host.c for testing on the computer.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void hap_port_random(uint8_t *buf, size_t len);
uint32_t hap_port_millis(void);

// Persistent storage of one blob. load returns false if nothing valid is stored.
bool hap_port_store_load(void *buf, size_t len);
bool hap_port_store_save(const void *buf, size_t len);

// Connection I/O. `handle` is what was passed to hap_conn_new().
bool hap_port_send(void *handle, const uint8_t *data, size_t len);
// Request that the connection be closed after pending data is sent. The port
// must call hap_conn_free() later, never from inside this call.
void hap_port_close(void *handle);

// Pairing state or configuration number changed: refresh the mDNS TXT record.
void hap_port_status_changed(void);
