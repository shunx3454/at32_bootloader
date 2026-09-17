#include "bootloader.h"

uint32_t boot_crc32(void const *data, size_t length) {
    uint8_t const *bytes = (uint8_t const *)data;
    uint32_t crc = 0xffffffffu;

    /* 标准反射式 CRC-32（多项式 0xEDB88320），与 PC 端 zlib.crc32 一致。 */
    for (size_t i = 0u; i < length; ++i) {
        crc ^= bytes[i];
        for (unsigned bit = 0u; bit < 8u; ++bit) {
            uint32_t const mask = 0u - (crc & 1u);
            crc = (crc >> 1u) ^ (0xedb88320u & mask);
        }
    }
    return ~crc;
}
