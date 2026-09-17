#include <string.h>

#include "tusb.h"

/*
 * Vendor 必须继续占接口 0，因为现有 PC 升级工具固定 claim interface 0。
 * CDC ACM 是一个包含控制/数据两个接口的功能，IAD 会把接口 1、2 关联起来。
 */
enum {
    ITF_NUM_VENDOR = 0,
    ITF_NUM_CDC_CONTROL,
    ITF_NUM_CDC_DATA,
    ITF_NUM_TOTAL
};

enum {
    STRID_LANGID,
    STRID_MANUFACTURER,
    STRID_PRODUCT,
    STRID_SERIAL,
    STRID_VENDOR_INTERFACE,
    STRID_CDC_INTERFACE
};

#define USB_VID 0xCAFE
#define USB_PID 0x4002
#define USB_BCD 0x0100

/* OUT 端点 0x01 接收升级命令，IN 端点 0x81 返回响应；方向由 bit7 区分。 */
#define EPNUM_VENDOR_OUT 0x01
#define EPNUM_VENDOR_IN 0x81

/*
 * CDC 通知使用中断 IN 端点 0x82；数据通道使用 Bulk OUT 0x03/IN 0x83。
 * 每个双向 Bulk 端点使用相同端点号，符合 AT32F403A FSDEV 的端点组织方式。
 */
#define EPNUM_CDC_NOTIFICATION 0x82
#define EPNUM_CDC_OUT          0x03
#define EPNUM_CDC_IN           0x83

#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_VENDOR_DESC_LEN + TUD_CDC_DESC_LEN)

static tusb_desc_device_t const desc_device =
{
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    /*
     * 复合设备在 Device Descriptor 使用 Misc/Common/IAD，通知主机按 IAD
     * 对多个接口进行功能分组，而不是把每个接口视为独立设备。
     */
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = USB_VID,
    .idProduct = USB_PID,
    .bcdDevice = USB_BCD,
    .iManufacturer = STRID_MANUFACTURER,
    .iProduct = STRID_PRODUCT,
    .iSerialNumber = STRID_SERIAL,
    .bNumConfigurations = 1
};

uint8_t const desc_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0, 100),

    /* 接口 0：保持原有 Vendor Bulk 升级接口及端点地址不变。 */
    TUD_VENDOR_DESCRIPTOR(ITF_NUM_VENDOR, STRID_VENDOR_INTERFACE,
                          EPNUM_VENDOR_OUT, EPNUM_VENDOR_IN, 64),

    /*
     * 接口 1、2：CDC ACM。
     * TUD_CDC_DESCRIPTOR 展开顺序为：
     *   IAD -> CDC 控制接口 -> CDC 类专用功能描述符 -> 通知端点
     *       -> CDC 数据接口 -> Bulk OUT/IN 端点。
     */
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC_CONTROL, STRID_CDC_INTERFACE,
                       EPNUM_CDC_NOTIFICATION, 8,
                       EPNUM_CDC_OUT, EPNUM_CDC_IN, 64)
};

_Static_assert(sizeof(desc_configuration) == CONFIG_TOTAL_LEN,
               "composite USB configuration descriptor length mismatch");

static char const *string_desc_arr[] =
{
    (char const[]){0x09, 0x04},
    "Artery",
    "AT32F403A Secure Bootloader",
    NULL,
    "Firmware Upgrade",
    "CDC ACM (Descriptor Demo)"
};

static uint16_t desc_string[32];

uint8_t const *tud_descriptor_device_cb(void) {
    return (uint8_t const *)&desc_device;
}

uint8_t const *tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
    return desc_configuration;
}

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;
    size_t count;

    if (index == STRID_LANGID) {
        memcpy(&desc_string[1], string_desc_arr[0], 2);
        count = 1;
    } else if (index == STRID_SERIAL) {
        /* 使用 96 位芯片 UID 生成稳定且每颗 MCU 唯一的 USB 序列号。 */
        static char const hex[] = "0123456789ABCDEF";
        volatile uint32_t const *uuid = (volatile uint32_t const *)0x1FFFF7E8u;
        count = 0;
        for (size_t word = 0; word < 3; word++) {
            uint32_t value = uuid[word];
            for (int shift = 28; shift >= 0; shift -= 4) {
                desc_string[1 + count++] = (uint16_t)hex[(value >> shift) & 0x0Fu];
            }
        }
    } else {
        if (index >= (sizeof(string_desc_arr) / sizeof(string_desc_arr[0]))) {
            return NULL;
        }

        char const *str = string_desc_arr[index];
        count = strlen(str);
        /* desc_string 共 32 个 UTF-16 单元，首单元保留给长度和描述符类型。 */
        if (count > 31u) {
            count = 31u;
        }

        for (size_t i = 0; i < count; i++) {
            desc_string[1 + i] = (uint16_t)str[i];
        }
    }

    desc_string[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2u * count + 2u));
    return desc_string;
}
