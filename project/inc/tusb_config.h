#ifndef TUSB_CONFIG_H_
#define TUSB_CONFIG_H_

#ifdef __cplusplus
extern "C" {
#endif

/* AT32F403A 裸机、全速 USB Device 配置，不启用 RTOS 和 Host 功能。 */
#define CFG_TUSB_OS                    OPT_OS_NONE
#define CFG_TUSB_DEBUG                 0
#define CFG_TUD_ENABLED                1
#define CFG_TUH_ENABLED                0
#define CFG_TUD_MAX_SPEED              OPT_MODE_FULL_SPEED

#define CFG_TUSB_MEM_ALIGN             __attribute__((aligned(4)))

#define CFG_TUD_ENDPOINT0_SIZE         64

/*
 * 枚举为 CDC ACM + Vendor Specific 复合设备。CDC 仅用于展示复合描述符，
 * Bootloader 升级协议仍只使用 Vendor 类。
 */
#define CFG_TUD_CDC                    1
#define CFG_TUD_VENDOR                 1
#define CFG_TUD_MSC                    0
#define CFG_TUD_HID                    0
#define CFG_TUD_MIDI                   0
#define CFG_TUD_MIDI2                  0
#define CFG_TUD_AUDIO                  0
#define CFG_TUD_VIDEO                  0
#define CFG_TUD_DFU                    0
#define CFG_TUD_DFU_RUNTIME            0

/* CDC 未接入业务逻辑，保留一个 FS 最大包大小的最小收发 FIFO。 */
#define CFG_TUD_CDC_RX_BUFSIZE         64
#define CFG_TUD_CDC_TX_BUFSIZE         64
#define CFG_TUD_CDC_RX_EPSIZE          64
#define CFG_TUD_CDC_TX_EPSIZE          64
#define CFG_TUD_CDC_NOTIFY             1

/* FSDEV 单端点最大包为 64 字节，TinyUSB 软件 FIFO 用 512 字节吸收突发数据。 */
#define CFG_TUD_VENDOR_RX_BUFSIZE      512
#define CFG_TUD_VENDOR_TX_BUFSIZE      512
#define CFG_TUD_VENDOR_RX_EPSIZE       64
#define CFG_TUD_VENDOR_TX_EPSIZE       64

#ifdef __cplusplus
}
#endif

#endif
