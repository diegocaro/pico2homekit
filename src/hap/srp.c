#include "srp.h"

#include <string.h>

#include "mbedtls/bignum.h"
#include "mbedtls/dhm.h"
#include "monocypher-ed25519.h"

static const uint8_t N_BIN[SRP_LEN] = MBEDTLS_DHM_RFC3526_MODP_3072_P_BIN;
static const uint8_t G = 5;
static const char USERNAME[] = "Pair-Setup";

#define CHECK(x) do { if ((ret = (x)) != 0) goto out; } while (0)

static void sha512_2(uint8_t out[64], const void *a, size_t alen, const void *b, size_t blen) {
    crypto_sha512_ctx ctx;
    crypto_sha512_init(&ctx);
    crypto_sha512_update(&ctx, a, alen);
    crypto_sha512_update(&ctx, b, blen);
    crypto_sha512_final(&ctx, out);
}

// Left-pads src (len <= SRP_LEN) into a SRP_LEN buffer.
static void pad(uint8_t out[SRP_LEN], const uint8_t *src, size_t len) {
    memset(out, 0, SRP_LEN - len);
    memcpy(out + SRP_LEN - len, src, len);
}

// k = H(N | PAD(g))
static int compute_k(mbedtls_mpi *k) {
    static uint8_t g_pad[SRP_LEN];
    uint8_t h[64];
    pad(g_pad, &G, 1);
    sha512_2(h, N_BIN, SRP_LEN, g_pad, SRP_LEN);
    return mbedtls_mpi_read_binary(k, h, sizeof(h));
}

int srp_make_verifier(const uint8_t salt[SRP_SALT_LEN], const char *code, uint8_t v_out[SRP_LEN]) {
    int ret;
    uint8_t inner[64], xh[64];
    crypto_sha512_ctx ctx;
    crypto_sha512_init(&ctx);
    crypto_sha512_update(&ctx, (const uint8_t *)USERNAME, strlen(USERNAME));
    crypto_sha512_update(&ctx, (const uint8_t *)":", 1);
    crypto_sha512_update(&ctx, (const uint8_t *)code, strlen(code));
    crypto_sha512_final(&ctx, inner);
    sha512_2(xh, salt, SRP_SALT_LEN, inner, sizeof(inner));

    mbedtls_mpi x, v, N, g;
    mbedtls_mpi_init(&x); mbedtls_mpi_init(&v); mbedtls_mpi_init(&N); mbedtls_mpi_init(&g);
    CHECK(mbedtls_mpi_read_binary(&x, xh, sizeof(xh)));
    CHECK(mbedtls_mpi_read_binary(&N, N_BIN, SRP_LEN));
    CHECK(mbedtls_mpi_lset(&g, G));
    CHECK(mbedtls_mpi_exp_mod(&v, &g, &x, &N, NULL));
    CHECK(mbedtls_mpi_write_binary(&v, v_out, SRP_LEN));
out:
    mbedtls_mpi_free(&x); mbedtls_mpi_free(&v); mbedtls_mpi_free(&N); mbedtls_mpi_free(&g);
    crypto_wipe(inner, sizeof(inner));
    crypto_wipe(xh, sizeof(xh));
    return ret;
}

int srp_server_public(const uint8_t v_bin[SRP_LEN], const uint8_t b_bin[SRP_PRIV_LEN], uint8_t B_out[SRP_LEN]) {
    int ret;
    mbedtls_mpi k, v, b, N, g, B, t;
    mbedtls_mpi_init(&k); mbedtls_mpi_init(&v); mbedtls_mpi_init(&b); mbedtls_mpi_init(&N);
    mbedtls_mpi_init(&g); mbedtls_mpi_init(&B); mbedtls_mpi_init(&t);
    CHECK(compute_k(&k));
    CHECK(mbedtls_mpi_read_binary(&v, v_bin, SRP_LEN));
    CHECK(mbedtls_mpi_read_binary(&b, b_bin, SRP_PRIV_LEN));
    CHECK(mbedtls_mpi_read_binary(&N, N_BIN, SRP_LEN));
    CHECK(mbedtls_mpi_lset(&g, G));
    CHECK(mbedtls_mpi_exp_mod(&B, &g, &b, &N, NULL));   // g^b
    CHECK(mbedtls_mpi_mul_mpi(&t, &k, &v));             // k*v
    CHECK(mbedtls_mpi_add_mpi(&t, &t, &B));
    CHECK(mbedtls_mpi_mod_mpi(&B, &t, &N));
    CHECK(mbedtls_mpi_write_binary(&B, B_out, SRP_LEN));
out:
    mbedtls_mpi_free(&k); mbedtls_mpi_free(&v); mbedtls_mpi_free(&b); mbedtls_mpi_free(&N);
    mbedtls_mpi_free(&g); mbedtls_mpi_free(&B); mbedtls_mpi_free(&t);
    return ret;
}

int srp_server_verify(const uint8_t v_bin[SRP_LEN], const uint8_t b_bin[SRP_PRIV_LEN],
                      const uint8_t B_bin[SRP_LEN], const uint8_t *A_bin, size_t A_len,
                      const uint8_t salt[SRP_SALT_LEN], const uint8_t M1[64],
                      uint8_t K[64], uint8_t M2[64]) {
    if (A_len == 0 || A_len > SRP_LEN) return -1;
    int ret;
    static uint8_t A_pad[SRP_LEN], S_bin[SRP_LEN];
    uint8_t u_h[64], hN[64], hg[64], hI[64], M1_calc[64];
    pad(A_pad, A_bin, A_len);

    mbedtls_mpi A, v, b, N, u, S, t;
    mbedtls_mpi_init(&A); mbedtls_mpi_init(&v); mbedtls_mpi_init(&b); mbedtls_mpi_init(&N);
    mbedtls_mpi_init(&u); mbedtls_mpi_init(&S); mbedtls_mpi_init(&t);
    CHECK(mbedtls_mpi_read_binary(&A, A_pad, SRP_LEN));
    CHECK(mbedtls_mpi_read_binary(&N, N_BIN, SRP_LEN));
    // Safeguard: A mod N must not be 0.
    CHECK(mbedtls_mpi_mod_mpi(&t, &A, &N));
    if (mbedtls_mpi_cmp_int(&t, 0) == 0) { ret = -1; goto out; }

    sha512_2(u_h, A_pad, SRP_LEN, B_bin, SRP_LEN);  // u = H(PAD(A) | PAD(B))
    CHECK(mbedtls_mpi_read_binary(&u, u_h, sizeof(u_h)));
    CHECK(mbedtls_mpi_read_binary(&v, v_bin, SRP_LEN));
    CHECK(mbedtls_mpi_read_binary(&b, b_bin, SRP_PRIV_LEN));

    // S = (A * v^u)^b mod N
    CHECK(mbedtls_mpi_exp_mod(&S, &v, &u, &N, NULL));
    CHECK(mbedtls_mpi_mul_mpi(&t, &A, &S));
    CHECK(mbedtls_mpi_mod_mpi(&t, &t, &N));
    CHECK(mbedtls_mpi_exp_mod(&S, &t, &b, &N, NULL));
    CHECK(mbedtls_mpi_write_binary(&S, S_bin, SRP_LEN));
    crypto_sha512(K, S_bin, SRP_LEN);               // K = H(PAD(S))

    // M1 = H(H(N) xor H(g) | H(I) | s | A | B | K)
    crypto_sha512(hN, N_BIN, SRP_LEN);
    crypto_sha512(hg, &G, 1);
    for (int i = 0; i < 64; i++) hN[i] ^= hg[i];
    crypto_sha512(hI, (const uint8_t *)USERNAME, strlen(USERNAME));
    crypto_sha512_ctx ctx;
    crypto_sha512_init(&ctx);
    crypto_sha512_update(&ctx, hN, 64);
    crypto_sha512_update(&ctx, hI, 64);
    crypto_sha512_update(&ctx, salt, SRP_SALT_LEN);
    crypto_sha512_update(&ctx, A_bin, A_len);
    crypto_sha512_update(&ctx, B_bin, SRP_LEN);
    crypto_sha512_update(&ctx, K, 64);
    crypto_sha512_final(&ctx, M1_calc);
    if (crypto_verify64(M1_calc, M1) != 0) { ret = -1; goto out; }

    // M2 = H(A | M1 | K)
    crypto_sha512_init(&ctx);
    crypto_sha512_update(&ctx, A_bin, A_len);
    crypto_sha512_update(&ctx, M1, 64);
    crypto_sha512_update(&ctx, K, 64);
    crypto_sha512_final(&ctx, M2);
    ret = 0;
out:
    mbedtls_mpi_free(&A); mbedtls_mpi_free(&v); mbedtls_mpi_free(&b); mbedtls_mpi_free(&N);
    mbedtls_mpi_free(&u); mbedtls_mpi_free(&S); mbedtls_mpi_free(&t);
    crypto_wipe(S_bin, sizeof(S_bin));
    if (ret != 0) crypto_wipe(K, 64);
    return ret;
}
