/*
 * Minimal mbed TLS configuration for the Kern Android simulator.
 *
 * Kern only calls primitives in libmbedcrypto: AES (CBC/CTR/ECB), GCM,
 * Base64, MD/HMAC, PKCS#5 PBKDF2, SHA-256, SHA-512. Everything else
 * (TLS, X.509, PK, ECP, RSA, ChaCha20, Camellia/ARIA/DES, PSA, ...) is
 * dead code in this build, so this config disables the lot at compile
 * time and shrinks libmbedcrypto.a accordingly.
 *
 * Wired in via MBEDTLS_CONFIG_FILE cache var in app/src/main/cpp/CMakeLists.txt.
 */
#pragma once

/* Architectural assists. */
#define MBEDTLS_HAVE_ASM

/* Block-cipher modes called via mbedtls_aes_crypt_cbc / _ctr / _ecb. */
#define MBEDTLS_CIPHER_MODE_CBC
#define MBEDTLS_CIPHER_MODE_CTR

/* Hash algorithms used by Kern (directly and via HMAC/PBKDF2). */
#define MBEDTLS_SHA224_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_SHA384_C
#define MBEDTLS_SHA512_C

/* Symmetric primitives. */
#define MBEDTLS_AES_C
#define MBEDTLS_GCM_C

/* Encodings & key derivation. */
#define MBEDTLS_BASE64_C
#define MBEDTLS_MD_C
#define MBEDTLS_PKCS5_C

/* Note: do NOT include mbedtls/check_config.h here. Since mbedtls 3.0,
 * build_info.h handles the inclusion order — running config-adjustment
 * headers (which derive MBEDTLS_BLOCK_CIPHER_CAN_AES, MBEDTLS_MD_CAN_SHA256,
 * etc. from the C-module switches above) before check_config runs. */
