/*
 * Minimal mbed TLS configuration for the Kern Android simulator.
 *
 * Kern only calls primitives in libmbedcrypto: AES (CBC/CTR/ECB), GCM,
 * Base64, MD/HMAC, PKCS#5 PBKDF2, SHA-256, SHA-512 — plus the PSA Crypto
 * API, which core/crypto_utils.c is written against (AES cipher/AEAD,
 * SHA-256, PBKDF2-HMAC key derivation). Everything else (TLS, X.509, PK,
 * ECP, RSA, ChaCha20, Camellia/ARIA/DES, ...) is dead code in this build,
 * so this config disables the lot at compile time and shrinks
 * libmbedcrypto.a accordingly.
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

/* PSA Crypto core (crypto_utils.c uses the PSA API). psa_crypto_init()
 * requires an RNG: entropy from the OS (/dev/urandom) through CTR-DRBG. */
#define MBEDTLS_PSA_CRYPTO_C
#define MBEDTLS_ENTROPY_C
#define MBEDTLS_CTR_DRBG_C

/* Without MBEDTLS_PSA_CRYPTO_CONFIG, PSA support is inferred from the
 * legacy MBEDTLS_*_C switches above — which covers CBC/CTR, GCM, SHA-256
 * and HMAC, but NOT PBKDF2 or ECB: neither has a legacy-module mapping
 * (config_adjust_psa_from_legacy.h never enables them), so request their
 * built-in implementations explicitly. Omitting these compiles fine but
 * makes psa_key_derivation_setup(PSA_ALG_PBKDF2_HMAC(...)) — i.e. PIN
 * key stretching — and PSA_ALG_ECB_NO_PADDING ciphers fail at runtime
 * with PSA_ERROR_NOT_SUPPORTED. */
#define PSA_WANT_ALG_PBKDF2_HMAC 1
#define MBEDTLS_PSA_BUILTIN_ALG_PBKDF2_HMAC 1
#define PSA_WANT_ALG_ECB_NO_PADDING 1
#define MBEDTLS_PSA_BUILTIN_ALG_ECB_NO_PADDING 1

/* Note: do NOT include mbedtls/check_config.h here. Since mbedtls 3.0,
 * build_info.h handles the inclusion order — running config-adjustment
 * headers (which derive MBEDTLS_BLOCK_CIPHER_CAN_AES, MBEDTLS_MD_CAN_SHA256,
 * etc. from the C-module switches above) before check_config runs. */
