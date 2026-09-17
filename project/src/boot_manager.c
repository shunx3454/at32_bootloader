#include <stdio.h>

#include "at32f403a_407_wk_config.h"
#include "bootloader.h"
#include "usb_boot.h"
#include "wk_system.h"

#define BOOT_KEY_DEBOUNCE_MS 30u
#define BOOT_KEY_HOLD_MS 3000u

static bool upgrade_key_held(void) {
    uint32_t start;

    /* PA0 上拉、按下为低：先消抖，再要求持续按住 3 秒，避免误入升级模式。 */
    if (gpio_input_data_bit_read(KEY_GPIO_PORT, KEY_PIN) != RESET) {
        return false;
    }
    wk_delay_ms(BOOT_KEY_DEBOUNCE_MS);
    if (gpio_input_data_bit_read(KEY_GPIO_PORT, KEY_PIN) != RESET) {
        return false;
    }

    printf("[BOOT] PA0 low; hold for 3 seconds to enter upgrade mode.\r\n");
    start = wk_timebase_get();
    while ((wk_timebase_get() - start) < BOOT_KEY_HOLD_MS) {
        if (gpio_input_data_bit_read(KEY_GPIO_PORT, KEY_PIN) != RESET) {
            return false;
        }
    }
    return true;
}

static bool try_slot(boot_slot_id_t slot) {
    boot_control_t const *bcb = boot_bcb_get();
    boot_firmware_header_t const *header = NULL;
    bool const known = bcb->slot_state[slot] == BOOT_SLOT_VALID;

    /* BCB 未记录且槽位也没有 ATFW 头时直接跳过，避免对空 Flash 做耗时哈希。 */
    if (!known && boot_slot_header(slot)->magic != BOOT_FIRMWARE_MAGIC) {
        return false;
    }
    printf("[BOOT] Validating App %c from Flash...\r\n", slot == BOOT_SLOT_A ? 'A' : 'B');
    /* 每次启动都从 Flash 重新计算哈希并验签，不盲目信任 BCB 的 VALID 标志。 */
    if (!boot_image_validate(slot, &header)) {
        printf("[BOOT] App %c validation failed.\r\n", slot == BOOT_SLOT_A ? 'A' : 'B');
        if (known) {
            (void)boot_bcb_set_state(slot, BOOT_SLOT_INVALID, NULL, 1u);
        }
        return false;
    }
    /* 允许发现由调试器/工装完整写入、但尚未登记到 BCB 的有效签名镜像。 */
    if (!known && !boot_bcb_set_state(slot, BOOT_SLOT_VALID, header, 0u)) {
        printf("[BOOT] Warning: could not persist discovered App state.\r\n");
    }
    printf("[BOOT] Starting App %c, version=%lu, security=%lu.\r\n", slot == BOOT_SLOT_A ? 'A' : 'B',
           (unsigned long)header->firmware_version, (unsigned long)header->security_version);
    boot_jump_to_slot(slot);
}

__attribute__((naked, noreturn))
static void boot_branch_to_app(uint32_t app_msp __attribute__((unused)),
                               uint32_t app_reset __attribute__((unused)))
{
    /*
     * 裸汇编跳板保证切换 MSP 后不再执行可能访问 Bootloader 旧栈帧的 C 代码。
     * AAPCS 规定前两个参数位于 r0/r1；Reset Handler 地址最低位必须为 1，
     * bx 会据此保持 Thumb 状态。
     */
    __asm volatile(
        "msr msp, r0      \n"
        "dsb              \n"
        "isb              \n"
        "bx  r1           \n"
    );
}

void boot_jump_to_slot(boot_slot_id_t slot) {
    uint32_t const vector = boot_slot_vector(slot);
    uint32_t const *vectors = (uint32_t const *)(uintptr_t)vector;

    /* 清理运行环境期间禁止中断，防止中断在 VTOR/MSP 切换到一半时进入。 */
    __disable_irq();

    /* 停止 Bootloader 的 1 ms 时基，避免 App 启动阶段继承 SysTick。 */
    SysTick->CTRL = 0u;
    SysTick->LOAD = 0u;
    SysTick->VAL = 0u;
    /* 关闭全部外部中断并清除 NVIC pending 位。 */
    for (size_t index = 0u; index < 8u; ++index) {
        NVIC->ICER[index] = 0xffffffffu;
        NVIC->ICPR[index] = 0xffffffffu;
    }

    /* 清除 Cortex-M 系统异常中的 SysTick 和 PendSV 挂起状态。 */
    SCB->ICSR = SCB_ICSR_PENDSTCLR_Msk | SCB_ICSR_PENDSVCLR_Msk;

    /* 后续异常必须从目标 App 的向量表取入口。 */
    SCB->VTOR = vector;

    /* 恢复接近硬件复位后的特权级、中断屏蔽和主栈选择状态。 */
    __set_BASEPRI(0u);
    __set_FAULTMASK(0u);
    __set_CONTROL(0u);

    /* 确保前面的系统寄存器写入在跳转前全部生效。 */
    __DSB();
    __ISB();

    /* 硬件复位时 PRIMASK=0；此时 SysTick/PendSV/NVIC pending 均已清理。 */
    __enable_irq();

    /* 从这里开始仅用汇编切换 MSP 并跳入 App Reset_Handler，成功后不返回。 */
    boot_branch_to_app(vectors[0], vectors[1]);

    while (1) {
    }
}

void bootloader_run(void) {
    /* 按键判定必须在正常启动槽位之前完成。 */
    bool const force_upgrade = upgrade_key_held();

    printf("[BOOT] AT32F403A secure bootloader 1.0.0.\r\n");
    
    // 查找 bcb 结构块，赋值到 current_bcb 和 current_address
    // 一个有效选有效，两个有效选最新的，无效则默认缺省状态
    boot_bcb_load();

    // 加载 sLib的 公钥，并导入 mbedTLS 的 key，设置相关属性
    if (!boot_security_init()) {
        printf("[BOOT] Public key record/PSA initialization failed.\r\n");
    }

    if (!force_upgrade) {
        /* 默认槽失败时自动验证备用槽，实现 A/B 回退启动。 */
        boot_slot_id_t const preferred = (boot_slot_id_t)boot_bcb_get()->default_slot;
        boot_slot_id_t const alternate = preferred == BOOT_SLOT_A ? BOOT_SLOT_B : BOOT_SLOT_A;
        (void)try_slot(preferred);
        (void)try_slot(alternate);
    } else {
        printf("[BOOT] Upgrade key accepted.\r\n");
        tmr_period_value_set(TMR2, 5000);
    }

    /* 没有可启动镜像或按键强制升级时，常驻 TinyUSB Vendor 升级循环。 */
    printf("[BOOT] Entering TinyUSB Vendor upgrade mode.\r\n");

    
    if (!usb_boot_init()) {
        printf("[BOOT] USB initialization failed.\r\n");
        tmr_period_value_set(TMR2, 1000);
        while (1) {
        }
    }
    while (1) {
        usb_boot_task();
    }
}
