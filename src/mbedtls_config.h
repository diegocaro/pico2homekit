// Minimal mbedTLS 3.x config: only the crypto we use.
#ifndef MBEDTLS_CONFIG_H
#define MBEDTLS_CONFIG_H

// MiBeacon v4/v5 decryption (AES-128-CCM)
#define MBEDTLS_AES_C
#define MBEDTLS_CIPHER_C
#define MBEDTLS_CCM_C
#define MBEDTLS_AES_ROM_TABLES        // keep tables in flash, save RAM
#define MBEDTLS_AES_FEWER_TABLES

#endif
