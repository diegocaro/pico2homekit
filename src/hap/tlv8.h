// TLV8 encoding used by HomeKit pairing (values > 255 bytes are split into
// consecutive items of the same type).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    TLV_METHOD = 0x00,
    TLV_IDENTIFIER = 0x01,
    TLV_SALT = 0x02,
    TLV_PUBLIC_KEY = 0x03,
    TLV_PROOF = 0x04,
    TLV_ENCRYPTED_DATA = 0x05,
    TLV_STATE = 0x06,
    TLV_ERROR = 0x07,
    TLV_SIGNATURE = 0x0A,
    TLV_PERMISSIONS = 0x0B,
    TLV_FLAGS = 0x13,
    TLV_SEPARATOR = 0xFF,
};

enum {
    TLV_ERR_UNKNOWN = 1,
    TLV_ERR_AUTHENTICATION = 2,
    TLV_ERR_BACKOFF = 3,
    TLV_ERR_MAX_PEERS = 4,
    TLV_ERR_MAX_TRIES = 5,
    TLV_ERR_UNAVAILABLE = 6,
    TLV_ERR_BUSY = 7,
};

typedef struct {
    uint8_t *buf;
    size_t cap;
    size_t len;
    bool overflow;
} tlv_writer_t;

void tlv_init(tlv_writer_t *w, uint8_t *buf, size_t cap);
void tlv_add(tlv_writer_t *w, uint8_t type, const void *value, size_t len);
void tlv_add_u8(tlv_writer_t *w, uint8_t type, uint8_t value);

// Copies the (reassembled) value of the first item of `type` into out.
// Returns its length, -1 if absent, -2 if larger than cap.
int tlv_get(const uint8_t *buf, size_t len, uint8_t type, uint8_t *out, size_t cap);
// Returns the 1-byte value of `type`, or -1.
int tlv_get_u8(const uint8_t *buf, size_t len, uint8_t type);
