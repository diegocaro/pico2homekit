// SRP-6a as used by HomeKit Pair Setup: 3072-bit group (RFC 5054), g = 5,
// SHA-512, username "Pair-Setup", password = setup code.
#pragma once

#include <stddef.h>
#include <stdint.h>

#define SRP_LEN 384      // bytes in N, A, B, S
#define SRP_SALT_LEN 16
#define SRP_PRIV_LEN 32  // server private value b

// v = g^x mod N, x = H(salt | H("Pair-Setup:" | code)). Returns 0 on success.
int srp_make_verifier(const uint8_t salt[SRP_SALT_LEN], const char *code, uint8_t v[SRP_LEN]);

// B = (k*v + g^b) mod N.
int srp_server_public(const uint8_t v[SRP_LEN], const uint8_t b[SRP_PRIV_LEN], uint8_t B[SRP_LEN]);

// Checks the client proof M1. On success fills the shared key K and server
// proof M2 and returns 0; returns nonzero if A is invalid or M1 does not match.
// Runs a known-answer test (srp_selftest.c). Returns 0 if all values match.
int srp_selftest(void);

int srp_server_verify(const uint8_t v[SRP_LEN], const uint8_t b[SRP_PRIV_LEN],
                      const uint8_t B[SRP_LEN], const uint8_t *A, size_t A_len,
                      const uint8_t salt[SRP_SALT_LEN], const uint8_t M1[64],
                      uint8_t K[64], uint8_t M2[64]);
