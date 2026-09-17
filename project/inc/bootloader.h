#ifndef BOOTLOADER_H
#define BOOTLOADER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * AT32F403A 片内 1 MiB Flash 地址规划。
 *
 * 0x08000000 ┌──────────────────────────────┐
 *            │ Bootloader                   │ 124 KiB
 * 0x0801F000 ├──────────────────────────────┤
 *            │ sLib 指令区（扇区 62）         │ 2 KiB
 * 0x0801F800 ├──────────────────────────────┤
 *            │ sLib 数据区/产品公钥（63）      │ 2 KiB
 * 0x08020000 ├──────────────────────────────┤
 *            │ App A：4 KiB 头 + 380 KiB体   │
 * 0x08080000 ├──────────────────────────────┤
 *            │ 保留间隔                      │ 64 KiB
 * 0x08090000 ├──────────────────────────────┤
 *            │ App B：4 KiB 头 + 380 KiB体   │
 * 0x080F0000 ├──────────────────────────────┤
 *            │ 保留间隔                      │ 56 KiB
 * 0x080FE000 ├──────────────────────────────┤
 *            │ BCB0 / BCB1                  │ 各 4 KiB
 * 0x08100000 └──────────────────────────────┘
 *
 * sLib 两个 2 KiB 扇区在产品配置后受硬件保护，普通 Flash 擦写不能再使用。
 */
#define BOOT_FLASH_BASE 0x08000000u
#define BOOT_FLASH_SIZE 0x00100000u
#define BOOT_ERASE_SIZE 0x00000800u
#define BOOT_CODE_SIZE 0x0001F000u
#define BOOT_SLIB_BASE 0x0801F000u
#define BOOT_SLIB_PUBLIC_KEY_ADDR 0x0801F800u
#define BOOT_SLOT_HEADER_SIZE 0x00001000u /* 4 KiB 签名固件头 */
#define BOOT_SLOT_SIZE 0x00060000u        /* 4 KiB 头 + 380 KiB App 主体 */
#define BOOT_SLOT_A_BASE 0x08020000u
#define BOOT_SLOT_B_BASE 0x08090000u
#define BOOT_SLOT_A_VECTOR (BOOT_SLOT_A_BASE + BOOT_SLOT_HEADER_SIZE)
#define BOOT_SLOT_B_VECTOR (BOOT_SLOT_B_BASE + BOOT_SLOT_HEADER_SIZE)
#define BOOT_SLOT_IMAGE_CAPACITY 0x0005F000u /* App 主体最大 380 KiB */
#define BOOT_BCB0_BASE 0x080FE000u
#define BOOT_BCB1_BASE 0x080FF000u
#define BOOT_BCB_PAGE_SIZE 0x00001000u

#define BOOT_TARGET_MCU 0x403A0001u
#define BOOT_FIRMWARE_MAGIC 0x57465441u /* "ATFW" */
#define BOOT_BCB_MAGIC 0x42435441u      /* "ATCB" */
#define BOOT_FRAME_MAGIC 0x50555441u    /* "ATUP" */
#define BOOT_PROTOCOL_VERSION 1u
#define BOOT_FIRMWARE_HEADER_VERSION 1u
#define BOOT_BCB_FORMAT_VERSION 1u
#define BOOT_KEY_ID 1u
#define BOOT_HASH_SHA256 1u
#define BOOT_SIGNATURE_ECDSA_P256 1u
#define BOOT_MAX_DOWNLOAD_CHUNK 4096u

typedef enum { BOOT_SLOT_A = 0, BOOT_SLOT_B = 1, BOOT_SLOT_COUNT = 2, BOOT_SLOT_NONE = 0xff } boot_slot_id_t;

typedef enum {
    /* BCB 中记录的槽位生命周期；UPDATING/INVALID 均禁止直接启动。 */
    BOOT_SLOT_EMPTY = 0,
    BOOT_SLOT_UPDATING = 1,
    BOOT_SLOT_VALID = 2,
    BOOT_SLOT_INVALID = 3,
    BOOT_SLOT_BAD = 4
} boot_slot_state_t;

//每个槽位开头的 4 KiB 固件头。
//signature 对 signature 字段之前的清单字段做 ECDSA P-256 签名；
//image_hash 是从 load_address 开始、长度为 image_size 的 App 主体 SHA-256。
//header_crc32 只负责快速发现传输/存储损坏，不能替代数字签名。
typedef struct __attribute__((packed)) {
    uint32_t magic;                  // 固件头魔数，固定为 BOOT_FIRMWARE_MAGIC（"ATFW"） 
    uint16_t header_version;         // 固件头格式版本，当前为 BOOT_FIRMWARE_HEADER_VERSION 
    uint16_t header_size;            // 固件头总长度，固定为 4 KiB 
    uint32_t target_mcu;             // 目标 MCU 标识，防止把其他芯片固件装入本设备 
    uint32_t image_type;             // 镜像所属槽位，取 boot_slot_id_t 的 App A 或 App B 
    uint32_t firmware_version;       // 功能版本号，用于显示和版本管理 
    uint32_t security_version;       // 单调递增的安全版本号，用于防回滚检查 
    uint32_t image_size;             // App 主体实际字节数，不包含前面的 4 KiB 固件头 
    uint32_t load_address;           // App 主体在 Flash 中的装载起始地址 
    uint32_t vector_address;         // Cortex-M 中断向量表地址，当前与 load_address 相同 
    uint32_t entry_address;          // 向量表中的 Reset_Handler 地址，最低位必须为 1 
    uint32_t key_id;                 // 验签公钥编号，用于选择 sLib 中对应的产品公钥 
    uint32_t hash_algorithm;         // App 主体哈希算法标识，当前为 SHA-256 
    uint32_t signature_algorithm;    // 固件清单签名算法标识，当前为 ECDSA P-256 
    uint8_t image_hash[32];          // 从 Flash 回读 App 主体计算得到的 SHA-256 期望值 
    uint8_t build_id[16];            // 构建标识；未显式指定时取 image_hash 的前 16 字节 
    uint8_t signature[64];           // 清单的 ECDSA 原始签名，格式为 32 字节 r || 32 字节 s 
    uint8_t reserved[3928];          // 预留给后续格式扩展，当前打包工具填充为 0 
    uint32_t header_crc32;           // 前 4092 字节的 CRC32，用于发现头部传输或存储损坏 
} boot_firmware_header_t;

/* USB Vendor 升级协议的固定 32 字节帧头，头部和负载分别具有 CRC32。 */
typedef struct __attribute__((packed)) {
    uint32_t magic;              /* 协议帧魔数，固定为 BOOT_FRAME_MAGIC（"ATUP"） */
    uint8_t protocol_version;    /* USB 升级协议版本，当前为 BOOT_PROTOCOL_VERSION */
    uint8_t header_size;         /* 帧头长度，固定为 32 字节 */
    uint16_t command;            /* 命令号；响应帧在原命令上置 BOOT_CMD_RESPONSE_BIT */
    uint32_t sequence;           /* 请求序号，响应原样回显，用于主机匹配请求和响应 */
    uint32_t payload_length;     /* 紧随帧头的负载字节数 */
    uint32_t session_id;         /* 下载会话标识；START 请求为 0，后续请求使用设备返回值 */
    uint32_t flags;              /* 命令阶段或帧属性，例如 DOWNLOAD START/DATA/END、响应标志 */
    uint32_t payload_crc32;      /* 负载区 CRC32；空负载时为 CRC32 空数据结果 */
    uint32_t header_crc32;       /* 帧头前 28 字节 CRC32，不包含该字段自身 */
} boot_frame_header_t;

/*
 * Boot Control Block：记录默认槽位、槽位状态和版本信息。
 * BCB0/BCB1 采用双副本轮换提交，generation 较新的有效副本生效。
 */
typedef struct __attribute__((packed)) {
    uint32_t magic;                                  /* BCB 魔数，固定为 BOOT_BCB_MAGIC（"ATCB"） */
    uint32_t format_version;                         /* BCB 数据结构版本，当前为 BOOT_BCB_FORMAT_VERSION */
    uint32_t generation;                             /* 每次提交加 1，双副本中数值较新的记录生效 */
    uint8_t default_slot;                            /* 正常启动时优先尝试的槽位 */
    uint8_t slot_state[BOOT_SLOT_COUNT];             /* App A/B 当前状态，索引使用 boot_slot_id_t */
    uint8_t reserved0;                               /* 保留字节，使后续 32 位字段从 4 字节边界开始 */
    uint32_t flags;                                  /* BCB 功能标志预留，当前未使用 */
    uint32_t last_error;                             /* 最近一次升级或槽位校验错误码，0 表示无错误 */
    uint32_t firmware_version[BOOT_SLOT_COUNT];      /* App A/B 最近登记的功能版本号 */
    uint32_t security_version[BOOT_SLOT_COUNT];      /* App A/B 最近登记的防回滚安全版本号 */
    uint32_t image_size[BOOT_SLOT_COUNT];            /* App A/B 最近登记的主体字节数 */
    uint32_t reserved1[3];                           /* 后续 BCB 格式扩展预留，当前为 0 */
    uint32_t crc32;                                  /* 本字段之前 60 字节的 CRC32 */
} boot_control_t;

/* 这些断言保证 PC 打包工具、USB 协议与 MCU 端的二进制布局始终一致。 */
_Static_assert(sizeof(boot_firmware_header_t) == BOOT_SLOT_HEADER_SIZE, "firmware header must be 4 KiB");
_Static_assert(sizeof(boot_frame_header_t) == 32u, "protocol frame header must be 32 bytes");
_Static_assert(sizeof(boot_control_t) == 64u, "BCB record must be 64 bytes");

uint32_t boot_crc32(void const *data, size_t length);

/* Flash 地址换算及固件头、镜像、向量表的分层校验接口。 */
uint32_t boot_slot_base(boot_slot_id_t slot);
uint32_t boot_slot_vector(boot_slot_id_t slot);
boot_firmware_header_t const *boot_slot_header(boot_slot_id_t slot);
bool boot_header_validate(boot_firmware_header_t const *header, boot_slot_id_t expected_slot,
                          uint32_t minimum_security_version);
bool boot_image_validate(boot_slot_id_t slot, boot_firmware_header_t const **header_out);
bool boot_vector_validate(boot_slot_id_t slot, boot_firmware_header_t const *header);
bool boot_security_init(void);
bool boot_manifest_verify(boot_firmware_header_t const *header);
bool boot_flash_hash(boot_firmware_header_t const *header, uint8_t digest[32]);

/* BCB 只通过以下接口更新，调用者不应直接改写 current_bcb。 */
void boot_bcb_load(void);
boot_control_t const *boot_bcb_get(void);
bool boot_bcb_set_state(boot_slot_id_t slot, boot_slot_state_t state, boot_firmware_header_t const *header,
                        uint32_t error);
bool boot_bcb_set_default(boot_slot_id_t slot);

/* Flash 写接口限制在应用槽位及其后的区域，避免误写 Bootloader/sLib。 */
bool boot_flash_erase_image(boot_slot_id_t slot, uint32_t image_size);
bool boot_flash_program(uint32_t address, void const *data, size_t length);

void bootloader_run(void);
/* 成功跳转后不会返回；调用前必须完成镜像和向量表校验。 */
void boot_jump_to_slot(boot_slot_id_t slot) __attribute__((noreturn));

#endif
