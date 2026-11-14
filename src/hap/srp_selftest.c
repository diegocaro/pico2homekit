// Known-answer test for srp.c: a full SRP exchange computed independently in
// Python (srp_selftest_vectors.h). Catches platform-specific bignum bugs.
#include <stdio.h>
#include <string.h>

#include "monocypher-ed25519.h"
#include "srp.h"
#include "srp_selftest_vectors.h"

int srp_selftest(void) {
    static uint8_t v[SRP_LEN], B[SRP_LEN];
    uint8_t h[64], K[64], M2[64];
    int fails = 0;

    if (srp_make_verifier(st_salt, SELFTEST_CODE, v) != 0) { printf("srp selftest: verifier error\n"); return 1; }
    crypto_sha512(h, v, SRP_LEN);
    if (memcmp(h, st_v_sha, sizeof(st_v_sha)) != 0) { printf("srp selftest: verifier WRONG\n"); fails++; }

    if (srp_server_public(v, st_b, B) != 0) { printf("srp selftest: B error\n"); return 1; }
    crypto_sha512(h, B, SRP_LEN);
    if (memcmp(h, st_B_sha, sizeof(st_B_sha)) != 0) { printf("srp selftest: B WRONG\n"); fails++; }

    if (srp_server_verify(v, st_b, B, st_A, SRP_LEN, st_salt, st_M1, K, M2) != 0) {
        printf("srp selftest: client proof rejected\n");
        fails++;
    } else if (memcmp(M2, st_M2, 64) != 0 || memcmp(K, st_K, 64) != 0) {
        printf("srp selftest: M2/K WRONG\n");
        fails++;
    }
    printf("srp selftest: %s\n", fails ? "FAILED" : "passed");
    return fails;
}
