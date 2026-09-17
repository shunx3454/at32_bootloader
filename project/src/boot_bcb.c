#include <string.h>

#include "at32f403a_407_conf.h"
#include "bootloader.h"

static boot_control_t current_bcb;
static uint32_t current_address;

// magic、格式版本、默认槽位和 CRC 均正确时，该 Flash 副本才可参与选举
static bool record_valid(boot_control_t const *record) {
    return record->magic == BOOT_BCB_MAGIC && record->format_version == BOOT_BCB_FORMAT_VERSION &&
           record->default_slot < BOOT_SLOT_COUNT &&
           boot_crc32(record, offsetof(boot_control_t, crc32)) == record->crc32;
}

// 有符号差值比较允许 generation 在 uint32_t 回绕后继续判断新旧
static bool generation_newer(uint32_t left, uint32_t right) { return (int32_t)(left - right) > 0; }

// 一个 BCB 页面是 4 KiB，需要连续擦除两个 2 KiB 物理扇区。
static bool erase_bcb_page(uint32_t address) {
    bool ok = true;

    flash_unlock();
    for (uint32_t offset = 0u; offset < BOOT_BCB_PAGE_SIZE; offset += BOOT_ERASE_SIZE) {
        if (flash_sector_erase(address + offset) != FLASH_OPERATE_DONE) {
            ok = false;
            break;
        }
    }
    flash_lock();
    return ok;
}

//   ## 一次完整升级会轮转几次

//   例如当前槽B开始升级：

//   START
//     └─ 只验证固件头，不修改BCB

//   ERASE
//     └─ B槽状态改为UPDATING
//        └─ 提交一次BCB （A/B信息都会记录）

//   END验证成功
//     └─ B槽状态改为VALID
//        └─ 再提交一次BCB （A/B信息都会记录）

//   JUMP_APP并设置默认槽
//     └─ default_slot改为B
//        └─ 再提交一次BCB （A/B信息都会记录）

//   假设升级前BCB1是当前副本：
//   ERASE：     写BCB0，generation=N+1，B=UPDATING
//   END：       写BCB1，generation=N+2，B=VALID
//   JUMP_APP：  写BCB0，generation=N+3，default=B
static bool commit(boot_control_t const *next) {
    /*
     * 始终写入当前有效副本的另一页：先擦除目标页，再写入并回读校验。
     * 只有新副本完整有效后才切换 RAM 中的 current_address，因此写入中途掉电时
     * 旧副本仍可用于恢复。
     */
    uint32_t const destination = current_address == BOOT_BCB0_BASE ? BOOT_BCB1_BASE : BOOT_BCB0_BASE;
    boot_control_t staged = *next;

    staged.magic = BOOT_BCB_MAGIC;
    staged.format_version = BOOT_BCB_FORMAT_VERSION;
    staged.generation = current_bcb.generation + 1u;
    staged.crc32 = boot_crc32(&staged, offsetof(boot_control_t, crc32));

    if (!erase_bcb_page(destination) || !boot_flash_program(destination, &staged, sizeof(staged)) ||
        !record_valid((boot_control_t const *)(uintptr_t)destination)) {
        return false;
    }
    current_bcb = staged;
    current_address = destination;
    return true;
}

void boot_bcb_load(void) {
    boot_control_t const *record0 = (boot_control_t const *)(uintptr_t)BOOT_BCB0_BASE;
    boot_control_t const *record1 = (boot_control_t const *)(uintptr_t)BOOT_BCB1_BASE;
    bool const valid0 = record_valid(record0);
    bool const valid1 = record_valid(record1);

    // 两份都有效时选 generation 更新的一份；仅一份有效时自动回退到它。
    if (valid0 && (!valid1 || generation_newer(record0->generation, record1->generation))) {
        current_bcb = *record0;
        current_address = BOOT_BCB0_BASE;
    } else if (valid1) {
        current_bcb = *record1;
        current_address = BOOT_BCB1_BASE;
    } else {
        /* 首次启动或两份 BCB 均损坏时只在 RAM 建立缺省状态，暂不擦写 Flash。 */
        memset(&current_bcb, 0, sizeof(current_bcb));
        current_bcb.magic = BOOT_BCB_MAGIC;
        current_bcb.format_version = BOOT_BCB_FORMAT_VERSION;
        current_bcb.default_slot = BOOT_SLOT_A;
        current_bcb.slot_state[BOOT_SLOT_A] = BOOT_SLOT_EMPTY;
        current_bcb.slot_state[BOOT_SLOT_B] = BOOT_SLOT_EMPTY;
        current_address = 0u;
    }
}

boot_control_t const *boot_bcb_get(void) { return &current_bcb; }

bool boot_bcb_set_state(boot_slot_id_t slot, boot_slot_state_t state, boot_firmware_header_t const *header,
                        uint32_t error) {
    boot_control_t next = current_bcb;

    if (slot >= BOOT_SLOT_COUNT) {
        return false;
    }
    next.slot_state[slot] = (uint8_t)state;
    next.last_error = error;
    /* 下载开始/完成时同步保存清单版本，失败路径可传 NULL 仅更新状态和错误码。 */
    if (header != NULL) {
        next.firmware_version[slot] = header->firmware_version;
        next.security_version[slot] = header->security_version;
        next.image_size[slot] = header->image_size;
    }
    return commit(&next);
}

bool boot_bcb_set_default(boot_slot_id_t slot) {
    boot_control_t next = current_bcb;

    /* 只有已经完成全部安全校验的 VALID 槽位才能成为默认启动项。 */
    if (slot >= BOOT_SLOT_COUNT || current_bcb.slot_state[slot] != BOOT_SLOT_VALID) {
        return false;
    }
    next.default_slot = (uint8_t)slot;
    return commit(&next);
}
