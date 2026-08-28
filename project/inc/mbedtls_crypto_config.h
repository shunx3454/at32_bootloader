#ifndef AT32_MBEDTLS_CRYPTO_CONFIG_H
#define AT32_MBEDTLS_CRYPTO_CONFIG_H

/* PSA Crypto core with a deliberately small embedded algorithm profile. */
#define MBEDTLS_PSA_CRYPTO_C

/*
 * AT32F403A has no cryptographic TRNG. The platform callback therefore
 * reports insufficient entropy instead of supplying insecure random bytes.
 * Hashing and operations with imported symmetric keys remain available.
 */
#define MBEDTLS_PSA_CRYPTO_EXTERNAL_RNG

/* Embedded memory profile: two heap-free key slots. */
#define MBEDTLS_PSA_KEY_SLOT_COUNT              2
#define MBEDTLS_PSA_STATIC_KEY_SLOTS
#define MBEDTLS_PSA_STATIC_KEY_SLOT_BUFFER_SIZE 80

/* SHA-256 hashing. */
#define PSA_WANT_ALG_SHA_256                    1

/* ECDSA verification over NIST P-256. */
#define MBEDTLS_PSA_P256M_DRIVER_ENABLED
#define PSA_WANT_ALG_ECDSA                      1
#define PSA_WANT_ECC_SECP_R1_256                1
#define PSA_WANT_KEY_TYPE_ECC_PUBLIC_KEY        1

#endif
