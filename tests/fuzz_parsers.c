// Random-input fuzzing of the parsers that see attacker-controlled bytes:
// BLE advertisements (mibeacon_parse) and pairing TLVs (tlv_get). Build with
// -fsanitize=address,undefined (make fuzz); any out-of-bounds access aborts.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hap/tlv8.h"
#include "mibeacon.h"

static uint32_t rng = 12345;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

int main(int argc, char **argv) {
    long iters = argc > 1 ? atol(argv[1]) : 2000000;
    const uint8_t mac[6] = {0xA4, 0xC1, 0x38, 1, 2, 3}, key[16] = {1, 2, 3};
    for (long i = 0; i < iters; i++) {
        size_t len = rnd() % 40;
        uint8_t *buf = malloc(len ? len : 1);  // exact size so ASan sees any overread
        for (size_t k = 0; k < len; k++) buf[k] = (uint8_t)rnd();
        if (len >= 2 && (i & 1)) { buf[0] |= 0x40; buf[1] = (uint8_t)((buf[1] & 0x0F) | 0x50); }  // v5, objects
        mibeacon_reading_t r;
        mibeacon_parse(buf, len, mac, (i & 2) ? key : NULL, &r);
        mibeacon_frame_counter(buf, len);
        free(buf);

        size_t tl = rnd() % 600;
        uint8_t *t = malloc(tl ? tl : 1), out[512];
        for (size_t k = 0; k < tl; k++) t[k] = (k % 2 == 0 && (rnd() & 1)) ? (uint8_t)(rnd() % 8) : (uint8_t)rnd();
        tlv_get(t, tl, (uint8_t)(rnd() % 8), out, (rnd() & 1) ? sizeof(out) : 1 + rnd() % 64);
        tlv_get_u8(t, tl, (uint8_t)(rnd() % 8));
        free(t);
    }
    printf("fuzz_parsers: %ld iterations, no sanitizer reports\n", iters);
    return 0;
}
