#include <string.h>

#include "at32f403a_407_conf.h"
#include "bootloader.h"

uint32_t boot_slot_base(boot_slot_id_t slot) {
    return slot == BOOT_SLOT_A ? BOOT_SLOT_A_BASE : slot == BOOT_SLOT_B ? BOOT_SLOT_B_BASE : 0u;
}

uint32_t boot_slot_vector(boot_slot_id_t slot) {
    uint32_t const base = boot_slot_base(slot);
    // 4 KiB 固件头不参与执行，App 向量表紧随其后
    return base == 0u ? 0u : base + BOOT_SLOT_HEADER_SIZE;
}

boot_firmware_header_t const *boot_slot_header(boot_slot_id_t slot) {
    return (boot_firmware_header_t const *)(uintptr_t)boot_slot_base(slot);
}

bool boot_flash_program(uint32_t address, void const *data, size_t length) {
    uint8_t const *source = (uint8_t const *)data;
    uint32_t const flash_end = BOOT_FLASH_BASE + BOOT_FLASH_SIZE;
    bool ok = true;

    // AT32 Flash 以 32 位字编程。写地址必须对齐，并禁止写入 Bootloader/sLib；
    // 长度检查采用 flash_end - address，避免 address + length 整数溢出。
    if ((address & 3u) != 0u || address < BOOT_SLOT_A_BASE || length > (size_t)(flash_end - address)) {
        return false;
    }

    flash_unlock();
    for (size_t offset = 0u; offset < length; offset += 4u) {
        /* 最后一包不足 4 字节时保持未提供字节为擦除态 0xFF。 */
        uint32_t word = 0xffffffffu;
        size_t const remaining = length - offset;
        size_t const count = remaining < sizeof(word) ? remaining : sizeof(word);
        memcpy(&word, source + offset, count);
        if (flash_word_program(address + (uint32_t)offset, word) != FLASH_OPERATE_DONE) {
            ok = false;
            break;
        }
    }
    flash_lock();

    /* 每次调用结束都从 Flash 回读，尽早发现供电或编程失败。 */
    return ok && memcmp((void const *)(uintptr_t)address, data, length) == 0;
}

bool boot_flash_erase_image(boot_slot_id_t slot, uint32_t image_size) {
    uint32_t const base = boot_slot_base(slot);
    uint32_t const total = BOOT_SLOT_HEADER_SIZE + image_size;
    uint32_t erase_length;
    bool ok = true;

    if (base == 0u || image_size == 0u || image_size > BOOT_SLOT_IMAGE_CAPACITY) {
        return false;
    }
    /* 擦除范围同时覆盖 4 KiB 固件头和实际镜像，并向上对齐到 2 KiB 扇区。 */
    erase_length = (total + BOOT_ERASE_SIZE - 1u) & ~(BOOT_ERASE_SIZE - 1u);

    flash_unlock();
    for (uint32_t offset = 0u; offset < erase_length; offset += BOOT_ERASE_SIZE) {
        if (flash_sector_erase(base + offset) != FLASH_OPERATE_DONE) {
            ok = false;
            break;
        }
    }
    flash_lock();
    return ok;
}
