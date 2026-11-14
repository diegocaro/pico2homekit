#include "tlv8.h"

#include <string.h>

void tlv_init(tlv_writer_t *w, uint8_t *buf, size_t cap) {
    w->buf = buf;
    w->cap = cap;
    w->len = 0;
    w->overflow = false;
}

void tlv_add(tlv_writer_t *w, uint8_t type, const void *value, size_t len) {
    const uint8_t *v = value;
    do {
        size_t n = len > 255 ? 255 : len;
        if (w->len + 2 + n > w->cap) { w->overflow = true; return; }
        w->buf[w->len++] = type;
        w->buf[w->len++] = (uint8_t)n;
        if (n) memcpy(w->buf + w->len, v, n);
        w->len += n;
        v += n;
        len -= n;
    } while (len > 0);
}

void tlv_add_u8(tlv_writer_t *w, uint8_t type, uint8_t value) { tlv_add(w, type, &value, 1); }

int tlv_get(const uint8_t *buf, size_t len, uint8_t type, uint8_t *out, size_t cap) {
    size_t i = 0;
    while (i + 2 <= len) {
        uint8_t t = buf[i], n = buf[i + 1];
        if (i + 2 + n > len) return -1;
        if (t == type) {
            size_t total = 0;
            // Reassemble: following items of the same type continue a 255-byte fragment.
            for (;;) {
                if (total + n > cap) return -2;
                memcpy(out + total, buf + i + 2, n);
                total += n;
                size_t next = i + 2 + n;
                if (n != 255 || next + 2 > len || buf[next] != type) break;
                i = next;
                n = buf[i + 1];
                if (i + 2 + n > len) return -1;
            }
            return (int)total;
        }
        i += 2 + n;
    }
    return -1;
}

int tlv_get_u8(const uint8_t *buf, size_t len, uint8_t type) {
    uint8_t v;
    return tlv_get(buf, len, type, &v, 1) == 1 ? v : -1;
}
