#include <stddef.h>
#include <string.h>

#include "bootloader.h"
#include "psa/crypto.h"

#define SLIB_RECORD_MAGIC 0x42494c53u /* "SLIB" */
#define SLIB_RECORD_VERSION 1u
#define SLIB_RECORD_SEC1 2u
#define SLIB_RECORD_KEY_LENGTH 65u

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t version;
    uint8_t encoding;
    uint16_t key_length;
    uint32_t key_id;
    uint8_t public_key[SLIB_RECORD_KEY_LENGTH];
} slib_public_key_record_t;

static psa_key_id_t verification_key;
static bool security_ready;

static slib_public_key_record_t const *public_key_record(void) {
    /* 产品公钥以 65 字节 SEC1 非压缩格式固定保存在受保护的 sLib 数据区。 */
    return (slib_public_key_record_t const *)(uintptr_t)BOOT_SLIB_PUBLIC_KEY_ADDR;
}

bool boot_security_init(void) {
    slib_public_key_record_t const *record = public_key_record();
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_status_t status;

    /* PSA 密钥只导入一次，后续验签复用 RAM 中的 key id。 */
    if (security_ready) {
        return true;
    }
    /* 导入前先校验记录元数据及 SEC1 的 0x04 非压缩点前缀。 */
    if (record->magic != SLIB_RECORD_MAGIC || record->version != SLIB_RECORD_VERSION ||
        record->encoding != SLIB_RECORD_SEC1 || record->key_length != SLIB_RECORD_KEY_LENGTH ||
        record->key_id != BOOT_KEY_ID || record->public_key[0] != 0x04u) {
        return false;
    }
    if (psa_crypto_init() != PSA_SUCCESS) {
        return false;
    }

    /* 最小权限：该公钥只允许验证 SHA-256 摘要上的 ECDSA P-256 签名。 */
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_VERIFY_HASH);
    psa_set_key_algorithm(&attributes, PSA_ALG_ECDSA(PSA_ALG_SHA_256));
    psa_set_key_type(&attributes, PSA_KEY_TYPE_ECC_PUBLIC_KEY(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&attributes, 256u);
    status = psa_import_key(&attributes, record->public_key, record->key_length, &verification_key);
    psa_reset_key_attributes(&attributes);
    security_ready = status == PSA_SUCCESS;
    return security_ready;
}

// firmware head 前面100字节 包含 img_hash
// manifest = HASH_SHA256(firmware head 前面100字节)
// signature = (manifest, private_key)
// VERUFY(public_key, signature, manifest) == True, 则firmware head 前面100字节 可信，且包含 img_hash
// img_hash 正确可以唯一 验证 img app 完整 
bool boot_manifest_verify(boot_firmware_header_t const *header) {
    uint8_t digest[32];
    size_t digest_length = 0u;

    if (!boot_security_init()) {
        return false;
    }
    /*
     * 签名覆盖固件头起始到 signature 字段之前的清单数据，其中已经包含
     * image_hash、版本、槽位和加载地址；signature/reserved/header_crc32 不参与签名。
     */
    if (psa_hash_compute(PSA_ALG_SHA_256, (uint8_t const *)header, offsetof(boot_firmware_header_t, signature), digest,
                         sizeof(digest), &digest_length) != PSA_SUCCESS ||
        digest_length != sizeof(digest)) {
        return false;
    }
    return psa_verify_hash(verification_key, PSA_ALG_ECDSA(PSA_ALG_SHA_256), digest, sizeof(digest), header->signature,
                           sizeof(header->signature)) == PSA_SUCCESS;
}

bool boot_flash_hash(boot_firmware_header_t const *header, uint8_t digest[32]) {
    psa_hash_operation_t operation = PSA_HASH_OPERATION_INIT;
    uint8_t const *flash = (uint8_t const *)(uintptr_t)header->load_address;
    size_t remaining = header->image_size;
    size_t digest_length = 0u;
    psa_status_t status = psa_hash_setup(&operation, PSA_ALG_SHA_256);

    /* 直接从 Flash 分块回读，验证的是实际烧录内容而不是 USB 接收缓存。 */
    while (status == PSA_SUCCESS && remaining != 0u) {
        size_t const block = remaining > 4096u ? 4096u : remaining;
        status = psa_hash_update(&operation, flash, block);
        flash += block;
        remaining -= block;
    }
    if (status == PSA_SUCCESS) {
        status = psa_hash_finish(&operation, digest, 32u, &digest_length);
    } else {
        (void)psa_hash_abort(&operation);
    }
    return status == PSA_SUCCESS && digest_length == 32u;
}

bool boot_header_validate(boot_firmware_header_t const *header, boot_slot_id_t expected_slot,
                          uint32_t minimum_security_version) {
    uint32_t const base = boot_slot_base(expected_slot);
    uint32_t const vector = boot_slot_vector(expected_slot);

    // 先验证所有边界、地址、算法和防回滚字段，再执行耗时的 ECDSA 验签。
    // entry_address 必须位于声明的镜像范围内且带 Thumb 位。
    if (base == 0u || header->magic != BOOT_FIRMWARE_MAGIC || header->header_version != BOOT_FIRMWARE_HEADER_VERSION ||
        header->header_size != sizeof(*header) || header->target_mcu != BOOT_TARGET_MCU ||
        header->image_type != (uint32_t)expected_slot || header->image_size < 8u ||
        header->image_size > BOOT_SLOT_IMAGE_CAPACITY || header->load_address != vector ||
        header->vector_address != vector || header->entry_address < vector ||
        header->entry_address >= vector + header->image_size || (header->entry_address & 1u) == 0u ||
        header->key_id != BOOT_KEY_ID || header->hash_algorithm != BOOT_HASH_SHA256 ||
        header->signature_algorithm != BOOT_SIGNATURE_ECDSA_P256 ||
        header->security_version < minimum_security_version ||
        boot_crc32(header, offsetof(boot_firmware_header_t, header_crc32)) != header->header_crc32) {
        return false;
    }

    return boot_manifest_verify(header);
}

bool boot_vector_validate(boot_slot_id_t slot, boot_firmware_header_t const *header) {
    uint32_t const vector = boot_slot_vector(slot);
    uint32_t const *vectors = (uint32_t const *)(uintptr_t)vector;
    uint32_t const msp = vectors[0];
    uint32_t const reset = vectors[1];

    /* 初始 MSP 必须位于 96 KiB SRAM 且满足 8 字节对齐，复位向量必须与签名头一致。 */
    return vector != 0u && msp >= 0x20000000u && msp <= 0x20018000u && (msp & 7u) == 0u &&
           reset == header->entry_address && (reset & 1u) != 0u && reset >= vector &&
           reset < vector + header->image_size;
}

bool boot_image_validate(boot_slot_id_t slot, boot_firmware_header_t const **header_out) {
    boot_firmware_header_t const *header = boot_slot_header(slot);
    uint8_t digest[32];

    /* 完整启动条件：签名头有效、Flash 主体哈希一致、Cortex-M 向量合法。 */
    if (!boot_header_validate(header, slot, 0u) || !boot_flash_hash(header, digest) ||
        memcmp(digest, header->image_hash, sizeof(digest)) != 0 || !boot_vector_validate(slot, header)) {
        return false;
    }
    if (header_out != NULL) {
        *header_out = header;
    }
    return true;
}
