#ifndef AT32_MBEDTLS_CRYPTO_CONFIG_H
#define AT32_MBEDTLS_CRYPTO_CONFIG_H

/* 只启用 Bootloader 所需的 PSA Crypto 核心，尽量缩小嵌入式代码和 RAM 占用。 */
#define MBEDTLS_PSA_CRYPTO_C

/*
 * AT32F403A 没有密码学 TRNG，因此平台回调明确报告熵不足，而不是返回不安全的
 * 伪随机字节。SHA-256 和使用外部导入公钥的 ECDSA 验签不依赖随机数，仍可使用。
 */
#define MBEDTLS_PSA_CRYPTO_EXTERNAL_RNG

/* 静态分配两个免堆 key slot；80 字节足以容纳 65 字节 SEC1 P-256 公钥。 */
#define MBEDTLS_PSA_KEY_SLOT_COUNT              2
#define MBEDTLS_PSA_STATIC_KEY_SLOTS
#define MBEDTLS_PSA_STATIC_KEY_SLOT_BUFFER_SIZE 80

/* 固件主体和签名清单使用 SHA-256。 */
#define PSA_WANT_ALG_SHA_256                    1

/* 只启用 NIST P-256 公钥和 ECDSA 验签，不包含 MCU 端私钥签名功能。 */
#define MBEDTLS_PSA_P256M_DRIVER_ENABLED
#define PSA_WANT_ALG_ECDSA                      1
#define PSA_WANT_ECC_SECP_R1_256                1
#define PSA_WANT_KEY_TYPE_ECC_PUBLIC_KEY        1

#endif
