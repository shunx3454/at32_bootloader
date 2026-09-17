#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "at32f403a_407_conf.h"
#include "bootloader.h"
#include "tusb.h"
#include "usb_boot.h"
#include "wk_system.h"

void usb_port_init(void);

enum {
    /* PC 到 Bootloader 的命令号；响应在原命令上置 BOOT_CMD_RESPONSE_BIT。 */
    BOOT_CMD_GET_INFO = 1,
    BOOT_CMD_ERASE = 2,
    BOOT_CMD_DOWNLOAD = 3,
    BOOT_CMD_RESET = 4,
    BOOT_CMD_JUMP_APP = 5,
    BOOT_CMD_RESPONSE_BIT = 0x8000
};

/* DOWNLOAD 命令通过 flags 区分清单握手、数据传输和结束校验三个阶段。 */
enum { BOOT_DOWNLOAD_START = 1u, BOOT_DOWNLOAD_DATA = 2u, BOOT_DOWNLOAD_END = 4u };

#define BOOT_FRAME_RESPONSE 0x80000000u

typedef enum {
    /* 负值错误码会原样返回 PC 工具，便于区分协议、Flash 和安全校验错误。 */
    BOOT_OK = 0,
    BOOT_ERR_FRAME = -1,
    BOOT_ERR_CRC = -2,
    BOOT_ERR_COMMAND = -3,
    BOOT_ERR_STATE = -4,
    BOOT_ERR_ARGUMENT = -5,
    BOOT_ERR_HEADER = -6,
    BOOT_ERR_ROLLBACK = -7,
    BOOT_ERR_FLASH = -8,
    BOOT_ERR_SEQUENCE = -9,
    BOOT_ERR_HASH = -10,
    BOOT_ERR_SIGNATURE = -11,
    BOOT_ERR_VECTOR = -12,
    BOOT_ERR_BCB = -13,
    BOOT_ERR_SLOT = -14
} boot_status_t;

/*
 * 升级状态机：
 * IDLE -> HEADER_ACCEPTED -> READY(已擦除，可接收数据) -> COMPLETE。
 * 任一写入或终检失败都会回到 IDLE，并把目标槽位标记为 INVALID。
 */
typedef enum { DOWNLOAD_IDLE, DOWNLOAD_HEADER_ACCEPTED, DOWNLOAD_READY, DOWNLOAD_COMPLETE } download_phase_t;

typedef struct __attribute__((packed)) {
    int32_t status;
    uint32_t detail;
    uint32_t session_id;
    uint32_t next_offset;
} boot_response_t;

typedef struct __attribute__((packed)) {
    uint32_t boot_version;
    uint32_t target_mcu;
    uint32_t uid[3];
    uint32_t flash_base;
    uint32_t flash_size;
    uint32_t erase_size;
    uint32_t slot_base[BOOT_SLOT_COUNT];
    uint32_t slot_size;
    uint32_t slot_image_capacity;
    uint32_t bcb_generation;
    uint32_t slot_firmware_version[BOOT_SLOT_COUNT];
    uint32_t slot_security_version[BOOT_SLOT_COUNT];
    uint32_t slot_image_size[BOOT_SLOT_COUNT];
    uint16_t max_chunk;
    uint8_t protocol_version;
    uint8_t slot_count;
    uint8_t default_slot;
    uint8_t slot_state[BOOT_SLOT_COUNT];
    uint8_t hash_algorithm;
    uint8_t signature_algorithm;
    uint32_t key_id;
} boot_info_t;

typedef struct {
    /* 当前会话严格记录目标槽位和下一个合法偏移，禁止乱序/跨槽写入。 */
    download_phase_t phase;
    boot_slot_id_t slot;
    uint32_t session_id;
    uint32_t next_offset;
    boot_firmware_header_t header;
} download_context_t;

#define RX_FRAME_CAPACITY (sizeof(boot_frame_header_t) + 8u + BOOT_MAX_DOWNLOAD_CHUNK)
#define TX_FRAME_CAPACITY 256u

/* TinyUSB 回调运行在裸机轮询上下文，收发缓存无需中断锁。 */
static uint8_t rx_frame[RX_FRAME_CAPACITY] __attribute__((aligned(4)));
static size_t rx_length;
static uint8_t tx_frame[TX_FRAME_CAPACITY] __attribute__((aligned(4)));
static size_t tx_length;
static size_t tx_offset;
static download_context_t download;
static bool reset_pending;
static uint32_t reset_started;

static uint32_t minimum_security_version(void) {
    boot_control_t const *bcb = boot_bcb_get();
    uint32_t minimum = 0u;

    /* 取所有 VALID 镜像的最高安全版本，拒绝安装更低版本以实现防回滚。 */
    for (size_t slot = 0u; slot < BOOT_SLOT_COUNT; ++slot) {
        if (bcb->slot_state[slot] == BOOT_SLOT_VALID && bcb->security_version[slot] > minimum) {
            minimum = bcb->security_version[slot];
        }
    }
    return minimum;
}

static void queue_response(boot_frame_header_t const *request, boot_status_t status, uint32_t detail, void const *extra,
                           size_t extra_length) {
    boot_frame_header_t response_header;
    boot_response_t response = {
        .status = status, .detail = detail, .session_id = download.session_id, .next_offset = download.next_offset};
    size_t const payload_length = sizeof(response) + extra_length;

    if (sizeof(response_header) + payload_length > sizeof(tx_frame)) {
        return;
    }

    // 相应头格式 | frame head (32字节)| response (16字节)| extra 可选 |

    /* 响应复用请求的命令号和序列号，PC 可安全匹配流水线中的请求。 */
    memset(&response_header, 0, sizeof(response_header));
    response_header.magic = BOOT_FRAME_MAGIC;
    response_header.protocol_version = BOOT_PROTOCOL_VERSION;
    response_header.header_size = sizeof(response_header);
    response_header.command = request->command | BOOT_CMD_RESPONSE_BIT;
    response_header.sequence = request->sequence;
    response_header.payload_length = payload_length;
    response_header.session_id = download.session_id;
    response_header.flags = BOOT_FRAME_RESPONSE;

    // 先拷贝负载 payload
    memcpy(tx_frame + sizeof(response_header), &response, sizeof(response));
    if (extra_length != 0u) {
        memcpy(tx_frame + sizeof(response_header) + sizeof(response), extra, extra_length);
    }

    /* 先计算负载 CRC，再计算不含 header_crc32 自身的帧头 CRC。 */
    response_header.payload_crc32 = boot_crc32(tx_frame + sizeof(response_header), payload_length);
    response_header.header_crc32 = boot_crc32(&response_header, offsetof(boot_frame_header_t, header_crc32));
    memcpy(tx_frame, &response_header, sizeof(response_header));
    tx_length = sizeof(response_header) + payload_length;
    tx_offset = 0u;
}

static void respond_info(boot_frame_header_t const *request) {
    boot_control_t const *bcb = boot_bcb_get();
    uint32_t const *uid = (uint32_t const *)(uintptr_t)0x1ffff7e8u;
    boot_info_t info;

    /* 返回芯片 UID、Flash 布局和 BCB 摘要，PC 工具据此检查目标兼容性。 */
    memset(&info, 0, sizeof(info));
    info.boot_version = 0x00010000u;
    info.target_mcu = BOOT_TARGET_MCU;
    memcpy(info.uid, uid, sizeof(info.uid));
    info.flash_base = BOOT_FLASH_BASE;
    info.flash_size = BOOT_FLASH_SIZE;
    info.erase_size = BOOT_ERASE_SIZE;
    info.slot_base[BOOT_SLOT_A] = BOOT_SLOT_A_BASE;
    info.slot_base[BOOT_SLOT_B] = BOOT_SLOT_B_BASE;
    info.slot_size = BOOT_SLOT_SIZE;
    info.slot_image_capacity = BOOT_SLOT_IMAGE_CAPACITY;
    info.bcb_generation = bcb->generation;
    memcpy(info.slot_firmware_version, bcb->firmware_version, sizeof(info.slot_firmware_version));
    memcpy(info.slot_security_version, bcb->security_version, sizeof(info.slot_security_version));
    memcpy(info.slot_image_size, bcb->image_size, sizeof(info.slot_image_size));
    info.max_chunk = BOOT_MAX_DOWNLOAD_CHUNK;
    info.protocol_version = BOOT_PROTOCOL_VERSION;
    info.slot_count = BOOT_SLOT_COUNT;
    info.default_slot = bcb->default_slot;
    memcpy(info.slot_state, bcb->slot_state, sizeof(info.slot_state));
    info.hash_algorithm = BOOT_HASH_SHA256;
    info.signature_algorithm = BOOT_SIGNATURE_ECDSA_P256;
    info.key_id = BOOT_KEY_ID;
    queue_response(request, BOOT_OK, 0u, &info, sizeof(info));
}

static void download_fail(boot_status_t error) {
    /* 已经开始改写 Flash 后发生错误时，必须持久化 INVALID，禁止启动半成品。 */
    if (download.slot < BOOT_SLOT_COUNT && download.phase >= DOWNLOAD_READY) {
        (void)boot_bcb_set_state(download.slot, BOOT_SLOT_INVALID, &download.header, (uint32_t)(-error));
    }
    download.phase = DOWNLOAD_IDLE;
}

static boot_status_t handle_download_start(uint8_t const *payload, size_t payload_length,
                                           boot_frame_header_t const *request) {
    uint32_t const minimum = minimum_security_version();
    uint32_t seed[5];

    // 想要开始下载，payload 必须是一个完整的 firmware head 
    // 第一帧 |transframe head(32字节)| firmware head (4096字节 payload)| 
    if (payload_length != sizeof(download.header) || request->session_id != 0u) {
        return BOOT_ERR_ARGUMENT;
    }
    
    /* 擦除前先在 RAM 中完成槽位、防回滚、头 CRC 和 ECDSA 签名验证。 */
    memcpy(&download.header, payload, sizeof(download.header));
    if (download.header.image_type >= BOOT_SLOT_COUNT) {
        return BOOT_ERR_SLOT;
    }
    download.slot = (boot_slot_id_t)download.header.image_type;
    if (download.header.security_version < minimum) {
        return BOOT_ERR_ROLLBACK;
    }
    if (!boot_header_validate(&download.header, download.slot, minimum)) {
        return BOOT_ERR_HEADER;
    }

    /*
     * session_id 用于把后续 ERASE/DATA/END 绑定到本次握手；它不是密码学随机数，
     * 安全性由签名和严格状态机保证。
     */
    seed[0] = request->sequence;
    seed[1] = wk_timebase_get();
    seed[2] = download.header.header_crc32;
    seed[3] = *(uint32_t const *)(uintptr_t)0x1ffff7e8u;
    seed[4] = boot_bcb_get()->generation;
    download.session_id = boot_crc32(seed, sizeof(seed));
    if (download.session_id == 0u) {
        download.session_id = 1u;
    }
    download.next_offset = 0u;
    download.phase = DOWNLOAD_HEADER_ACCEPTED;
    return BOOT_OK;
}

static boot_status_t handle_erase(uint8_t const *payload, size_t payload_length, boot_frame_header_t const *request) {
    if (download.phase != DOWNLOAD_HEADER_ACCEPTED || payload_length != 4u ||
        request->session_id != download.session_id || payload[0] != (uint8_t)download.slot) {
        return BOOT_ERR_STATE;
    }
    /* 先落盘 UPDATING 状态，再擦除槽位；掉电后不会把残缺镜像当作有效 App。 */
    if (!boot_bcb_set_state(download.slot, BOOT_SLOT_UPDATING, &download.header, 0u)) {
        return BOOT_ERR_BCB;
    }
    /* 擦除范围包含 4 KiB 头和主体；头部写入成功后才允许接收数据包。 */
    if (!boot_flash_erase_image(download.slot, download.header.image_size) ||
        !boot_flash_program(boot_slot_base(download.slot), &download.header, sizeof(download.header))) {
        download_fail(BOOT_ERR_FLASH);
        return BOOT_ERR_FLASH;
    }
    download.phase = DOWNLOAD_READY;
    return BOOT_OK;
}

static boot_status_t handle_download_data(uint8_t const *payload, size_t payload_length,
                                          boot_frame_header_t const *request) {
    uint32_t offset;
    uint32_t length;

    if (download.phase != DOWNLOAD_READY || request->session_id != download.session_id || payload_length < 8u) {
        return BOOT_ERR_STATE;
    }

    // 下载 firmware.bin 状态时，每个 payload 前8字节会记录 offset + length
    // 真正的 firmware.bin 内容从 payload + 8 开始
    memcpy(&offset, payload, sizeof(offset));
    memcpy(&length, payload + 4u, sizeof(length));

    /*
     * 仅接受从 next_offset 开始的连续数据，防止丢包、重放或偏移构造造成越界写。
     * 除最后一包外，偏移和长度都必须满足 32 位 Flash 编程对齐要求。
     */
    if (length == 0u || length > BOOT_MAX_DOWNLOAD_CHUNK || payload_length != 8u + length ||
        offset != download.next_offset || offset > download.header.image_size ||
        length > download.header.image_size - offset || (offset & 3u) != 0u ||
        ((length & 3u) != 0u && offset + length != download.header.image_size)) {
        return BOOT_ERR_SEQUENCE;
    }
    if (!boot_flash_program(download.header.load_address + offset, payload + 8u, length)) {
        download_fail(BOOT_ERR_FLASH);
        return BOOT_ERR_FLASH;
    }
    download.next_offset += length;
    return BOOT_OK;
}

static boot_status_t handle_download_end(boot_frame_header_t const *request) {
    boot_firmware_header_t const *flash_header = boot_slot_header(download.slot);
    uint8_t digest[32];

    if (download.phase != DOWNLOAD_READY || request->session_id != download.session_id ||
        download.next_offset != download.header.image_size) {
        return BOOT_ERR_STATE;
    }
    /*
     * 结束阶段不信任传输过程中的数据：重新从 Flash 回读计算 SHA-256，随后再次
     * 验证清单签名和向量表，全部通过后才把 BCB 状态提交为 VALID。
     */
    if (!boot_flash_hash(flash_header, digest) || memcmp(digest, flash_header->image_hash, sizeof(digest)) != 0) {
        download_fail(BOOT_ERR_HASH);
        return BOOT_ERR_HASH;
    }
    if (!boot_manifest_verify(flash_header)) {
        download_fail(BOOT_ERR_SIGNATURE);
        return BOOT_ERR_SIGNATURE;
    }
    if (!boot_vector_validate(download.slot, flash_header)) {
        download_fail(BOOT_ERR_VECTOR);
        return BOOT_ERR_VECTOR;
    }
    if (!boot_bcb_set_state(download.slot, BOOT_SLOT_VALID, flash_header, 0u)) {
        download_fail(BOOT_ERR_BCB);
        return BOOT_ERR_BCB;
    }
    download.phase = DOWNLOAD_COMPLETE;
    return BOOT_OK;
}

static void handle_frame(boot_frame_header_t const *request, uint8_t const *payload) {
    boot_status_t status = BOOT_ERR_COMMAND;

    switch (request->command) {
    case BOOT_CMD_GET_INFO:
        if (request->payload_length == 0u) {
            respond_info(request);
        } else {
            queue_response(request, BOOT_ERR_ARGUMENT, 0u, NULL, 0u);
        }
        return;

    case BOOT_CMD_ERASE:
        status = handle_erase(payload, request->payload_length, request);
        break;

    case BOOT_CMD_DOWNLOAD:
        if (request->flags == BOOT_DOWNLOAD_START) {
            // 
            status = handle_download_start(payload, request->payload_length, request);
        } else if (request->flags == BOOT_DOWNLOAD_DATA) {
            status = handle_download_data(payload, request->payload_length, request);
        } else if (request->flags == BOOT_DOWNLOAD_END && request->payload_length == 0u) {
            status = handle_download_end(request);
        } else {
            status = BOOT_ERR_ARGUMENT;
        }
        break;

    case BOOT_CMD_JUMP_APP:
        /* 跳转请求仍执行完整镜像校验；可选 payload[1] 同时设置默认槽位。 */
        if (request->payload_length == 4u && payload[0] < BOOT_SLOT_COUNT &&
            boot_bcb_get()->slot_state[payload[0]] == BOOT_SLOT_VALID &&
            boot_image_validate((boot_slot_id_t)payload[0], NULL)) {
            status = BOOT_OK;
            if (payload[1] != 0u && !boot_bcb_set_default((boot_slot_id_t)payload[0])) {
                status = BOOT_ERR_BCB;
            }
            if (status == BOOT_OK) {
                /* 先把成功响应发完，再延迟复位，由正常启动路径完成最终跳转。 */
                reset_pending = true;
                reset_started = wk_timebase_get();
            }
        } else {
            status = BOOT_ERR_SLOT;
        }
        break;

    case BOOT_CMD_RESET:
        if (request->payload_length == 0u) {
            status = BOOT_OK;
            reset_pending = true;
            reset_started = wk_timebase_get();
        } else {
            status = BOOT_ERR_ARGUMENT;
        }
        break;

    default:
        break;
    }
    queue_response(request, status, 0u, NULL, 0u);
}

static void parse_rx_stream(void) {
    while (tx_length == 0u && rx_length >= sizeof(boot_frame_header_t)) {
        boot_frame_header_t header;
        size_t frame_length;

        memcpy(&header, rx_frame, sizeof(header));
        /*
         * USB 是字节流：遇到非法帧头时每次丢弃一个字节重新同步；合法但未收全的
         * 帧则保留在缓存中等待下一轮数据。
         */
        if (header.magic != BOOT_FRAME_MAGIC || header.protocol_version != BOOT_PROTOCOL_VERSION ||
            header.header_size != sizeof(header) || header.payload_length > 8u + BOOT_MAX_DOWNLOAD_CHUNK ||
            boot_crc32(&header, offsetof(boot_frame_header_t, header_crc32)) != header.header_crc32) {
            memmove(rx_frame, rx_frame + 1u, --rx_length);
            continue;
        }

        // frame可能很长，没收完跳过
        frame_length = sizeof(header) + header.payload_length;
        if (rx_length < frame_length) {
            return;
        }

        // 完整 frame 到达
        if (boot_crc32(rx_frame + sizeof(header), header.payload_length) != header.payload_crc32) {
            queue_response(&header, BOOT_ERR_CRC, 0u, NULL, 0u);
        } else {
            handle_frame(&header, rx_frame + sizeof(header));
        }
        rx_length -= frame_length;
        memmove(rx_frame, rx_frame + frame_length, rx_length);
    }
}

bool usb_boot_init(void) {
    tusb_rhport_init_t const device = {.role = TUSB_ROLE_DEVICE, .speed = TUSB_SPEED_FULL};

    /* 每次进入升级模式都从无会话状态开始，旧 session_id 不可继续使用。 */
    memset(&download, 0, sizeof(download));
    download.slot = BOOT_SLOT_NONE;
    usb_port_init();
    return tusb_init(0, &device);
}

void usb_boot_task(void) {
    /* TinyUSB 裸机模式要求主循环持续调用 tud_task() 推进控制传输和端点状态。 */
    tud_task();

    /* 响应可能分多次写入 512 字节 Vendor TX FIFO，全部发送后才接收下一请求。 */
    if (tx_length != 0u && tud_vendor_mounted()) {
        uint32_t const written = tud_vendor_write(tx_frame + tx_offset, tx_length - tx_offset);
        tx_offset += written;
        (void)tud_vendor_write_flush();
        if (tx_offset == tx_length) {
            tx_length = 0u;
            tx_offset = 0u;
        }
    }

    /* 半双工处理可避免单缓冲响应被后续请求覆盖。 */
    if (tx_length == 0u && tud_vendor_available() != 0u && rx_length < sizeof(rx_frame)) {
        uint32_t const count = tud_vendor_read(rx_frame + rx_length, sizeof(rx_frame) - rx_length);
        rx_length += count;
        parse_rx_stream();
    }

    /* 等待响应进入主机并留出 250 ms，再由系统复位重新执行安全启动选择。 */
    if (reset_pending && tx_length == 0u && (wk_timebase_get() - reset_started) >= 250u) {
        nvic_system_reset();
    }
}
