// Accessory database the HAP core serves. Implemented by the application
// (src/bridge.c).
#pragma once

#include <stddef.h>
#include <stdint.h>

// HAP status codes
#define HAP_STATUS_OK 0
#define HAP_STATUS_INSUFFICIENT_PRIVILEGES -70401
#define HAP_STATUS_COMMUNICATION_FAILURE -70402
#define HAP_STATUS_READ_ONLY -70404
#define HAP_STATUS_WRITE_ONLY -70405
#define HAP_STATUS_NO_NOTIFICATION -70406
#define HAP_STATUS_NOT_FOUND -70409
#define HAP_STATUS_INVALID_VALUE -70410

// Writes the full /accessories JSON into buf. Returns its length, or 0 if it does not fit.
size_t hap_db_accessories_json(char *buf, size_t cap);

// Writes the JSON value of a characteristic (e.g. "23.4") into buf.
// Returns HAP_STATUS_OK or an error status.
int hap_db_read(unsigned aid, unsigned iid, char *buf, size_t cap);

// Handles a write of a JSON value. Returns HAP_STATUS_OK or an error status.
int hap_db_write(unsigned aid, unsigned iid, const char *value, size_t len);

// Notification slot (0..63) for an event-capable characteristic,
// HAP_STATUS_NO_NOTIFICATION if it does not support events, or
// HAP_STATUS_NOT_FOUND if it does not exist.
int hap_db_event_slot(unsigned aid, unsigned iid);

// Hash of the accessory layout; when it changes the configuration number is bumped.
uint32_t hap_db_layout_hash(void);
