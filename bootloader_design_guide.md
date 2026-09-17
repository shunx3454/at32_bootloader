可行，而且整体方向是对的。基于 **TinyUSB Vendor Class + Bulk IN/OUT + 自定义 Bootloader 协议 + 多 APP 分区 + SHA-256 + 数字签名验证**，完全可以做成一个比较正规的产品级 Bootloader。

不过有几个地方我建议从一开始就设计对：**public key 区必须防篡改、Boot 状态不能和 public key 混写、DOWNLOAD 完成后应从 Flash 回读计算 hash、多 APP 如果都要直接执行要处理链接地址问题、Flash 擦写粒度必须和分区匹配。**

TinyUSB 很适合承担这里的 USB Transport。当前官方文档明确支持 Vendor-specific class 的通用 IN/OUT endpoint，也支持通过 Microsoft OS 2.0 descriptor 让 Windows 自动绑定 WinUSB；Vendor 类还支持 buffered/direct 两种模式。TinyUSB 的 USB IRQ 事件会进入队列，实际协议处理在 `tud_task()` 上下文完成，这也很适合把 Flash 擦写与 USB ISR 解耦。([TinyUSB][1])

---

# 1. 总体架构

我建议最终分成 6 层：

```text
┌────────────────────────────────────────────┐
│                PC Upgrade Tool             │
│                                            │
│ GET_INFO / ERASE / DOWNLOAD / RESET / JUMP│
└─────────────────────┬──────────────────────┘
                      │
                 USB Bulk
                      │
┌─────────────────────▼──────────────────────┐
│                 TinyUSB                    │
│ Vendor Specific Interface                  │
│ Bulk OUT + Bulk IN                         │
├────────────────────────────────────────────┤
│ USB Boot Protocol                          │
│ Frame / CMD / SEQ / ACK / ERROR            │
├────────────────────────────────────────────┤
│ Firmware Manager                           │
│ Header / Partition / SHA256 / Signature    │
├────────────────────────────────────────────┤
│ Boot Manager                               │
│ BCB / Default APP / Validate / Jump        │
├────────────────────────────────────────────┤
│ Flash IF                                   │
│ Erase / Program / Read / Protect           │
├────────────────────────────────────────────┤
│ MCU Internal Flash                         │
└────────────────────────────────────────────┘
```

TinyUSB 不参与：

```text
ERASE 哪个 APP
固件版本是否合法
SHA256
ECDSA
APP 是否有效
跳哪个 APP
```

它只负责：

```text
USB枚举
Bulk OUT 收数据
Bulk IN 发数据
```

这个边界最好不要破坏。

---

# 2. 你的 1MB Flash 划分

如果是 STM32 风格地址，假设：

```text
FLASH_BASE = 0x08000000
FLASH_SIZE = 1MB = 0x100000
FLASH_END  = 0x08100000
```

那么你目前设想的是：

| 区域         |           起始 |           结束 |     大小 |
| ---------- | -----------: | -----------: | -----: |
| Bootloader | `0x08000000` | `0x0801EFFF` | 124 KB |
| sLib       | `0x0801F000` | `0x0801FFFF` |   4 KB |
| APP Pool   | `0x08020000` | `0x080FFFFF` | 896 KB |

这个设计有个非常好的地方：

```text
124K + 4K = 128K
```

所以：

```text
APP_BASE = 0x08020000
```

是非常干净的 128K 边界。

---

## 3. 但首先必须确认 Flash 擦除粒度

这是整个方案里第一个需要确认的硬件条件。

如果目标 MCU 的最小擦除单位是：

```text
2K / 4K page
```

那么你的：

```text
124K Boot
4K sLib
```

可以是真正独立的物理分区。

但如果 MCU 是类似某些 STM32：

```text
16K
64K
128K sector
```

那么：

```text
0x0801F000 ~ 0x0801FFFF
```

虽然逻辑上是 4K sLib，物理上可能和 Bootloader 共享一个 Flash sector。

此时不能单独：

```text
erase(sLib)
```

否则可能把 Bootloader 一起擦掉。

但如果 public key 是：

> 出厂写一次，以后永久只读

那么问题不大。

反而可以：

```text
Bootloader 124K
+
Public Key 4K
        ↓
统一处于前 128K
        ↓
Write Protect
```

这是一个不错的 Root-of-Trust 区域。

所以后续真正落地前必须根据**具体 MCU 型号**重新计算物理 sector/page。

---

# 4. 不建议把 Boot 状态存在 sLib

这一点很重要。

4K sLib 里存：

```text
public_key
key_id
算法
key version
```

可以。

但不要再拿它保存：

```text
default_app
app_valid
upgrade_state
boot_count
```

因为这些都是**高频可变信息**。

否则：

```text
修改 default APP
       ↓
需要擦 sLib
       ↓
public_key 所在 page 被擦除
```

安全性和掉电可靠性都不好。

因此建议再增加：

# Boot Control Block，BCB

例如拿最后 8K：

```text
0x080FE000 ~ 0x080FFFFF
```

做两个 4K BCB page。

这样剩余：

```text
896K - 8K = 888K
```

两个 APP：

```text
888 / 2 = 444KB
```

于是一个非常漂亮的逻辑布局是：

```text
0x08000000
┌──────────────────────────────┐
│ Bootloader                   │ 124K
│ 0x08000000 - 0x0801EFFF     │
├──────────────────────────────┤
│ sLib / Root Public Key       │ 4K
│ 0x0801F000 - 0x0801FFFF     │
├──────────────────────────────┤ 0x08020000
│ APP A                        │
│ 444K                         │
│ 0x08020000 - 0x0808EFFF     │
├──────────────────────────────┤ 0x0808F000
│ APP B                        │
│ 444K                         │
│ 0x0808F000 - 0x080FDFFF     │
├──────────────────────────────┤
│ BCB A                        │ 4K
│ BCB B                        │ 4K
│ 0x080FE000 - 0x080FFFFF     │
└──────────────────────────────┘
0x08100000
```

这里的地址是按**4K 擦除对齐假设**给出的逻辑方案。

如果 MCU sector 不是 4K，需要重新排。

---

# 5. 为什么 BCB 做两份

因为 BCB 是 Bootloader 的关键状态。

例如：

```c
typedef struct
{
    uint32_t magic;
    uint32_t format_version;

    uint32_t generation;

    uint8_t  default_slot;
    uint8_t  slot_a_state;
    uint8_t  slot_b_state;
    uint8_t  reserved0;

    uint32_t flags;

    uint32_t last_error;

    uint32_t crc32;

} boot_control_t;
```

slot 状态：

```c
typedef enum
{
    SLOT_EMPTY,
    SLOT_UPDATING,
    SLOT_VALID,
    SLOT_INVALID,
    SLOT_BAD,

} slot_state_t;
```

两个 BCB：

```text
BCB0
BCB1
```

采用：

```text
generation + CRC32
```

启动时：

```text
BCB0 valid?
BCB1 valid?
     |
     v
选择 generation 最大的
```

更新的时候：

```text
当前 BCB0
   ↓
写 BCB1
   ↓
校验 BCB1
   ↓
BCB1 成为最新
```

不要：

```text
擦掉唯一 BCB
   ↓
写新值
```

否则中途掉电会把启动信息一起丢掉。

---

# 6. sLib 4K 应该怎么设计

虽然 public key 实际只占几十字节，但这 4K 可以设计成完整的 Secure Library Metadata：

```c
typedef struct
{
    uint32_t magic;          // "SLIB"

    uint16_t format_version;
    uint16_t header_size;

    uint32_t key_id;
    uint32_t algorithm;

    uint32_t key_size;

    uint8_t  public_key[64];

    uint32_t key_version;

    uint32_t flags;

    uint32_t crc32;

} slib_header_t;
```

如果使用：

```text
ECDSA P-256
```

可以保存 raw public key：

```text
X = 32 bytes
Y = 32 bytes
```

一共：

```text
64 bytes
```

签名也建议不用 ASN.1 DER，而用：

```text
R = 32 bytes
S = 32 bytes

signature = R || S
```

固定：

```text
64 bytes
```

Bootloader 解析会简单很多。

---

# 7. public key 最重要的不是“保密”，而是“不可修改”

Public key 本身当然是 public。

所以：

```text
RDP / 加密 public_key
```

不是核心。

真正核心的是：

> 攻击者绝对不能替换 Bootloader 信任的 public key。

否则攻击者：

```text
自己的 private key
       ↓
签恶意固件
       ↓
把 MCU public_key 换成自己的
       ↓
验签成功
```

整个签名系统立即失效。

所以建议：

```text
Bootloader
+
sLib
```

一起设置：

```text
Flash Write Protection
```

如果 MCU 支持：

```text
Secure Flash
TrustZone
PCROP
OTP
Option Bytes
```

还可以进一步强化。

---

# 8. 固件不要只是 APP.bin

建议定义：

```text
firmware.pkg

┌────────────────────────┐
│ Firmware Header 512B   │
├────────────────────────┤
│ Application Binary     │
│                        │
│                        │
└────────────────────────┘
```

我建议 Header 固定：

```text
512 bytes
```

这样 APP Vector Table 可以从：

```text
slot_base + 0x200
```

开始。

例如 APP A：

```text
Slot:
0x08020000

Firmware Header:
0x08020000 ~ 0x080201FF

Vector Table:
0x08020200
```

APP B：

```text
Slot:
0x0808F000

Header:
0x0808F000 ~ 0x0808F1FF

Vector Table:
0x0808F200
```

当然最终 Vector Table alignment 要根据具体 Cortex-M 核确认。

---

# 9. Firmware Header 推荐结构

类似：

```c
#define FW_HEADER_SIZE 512

typedef struct
{
    uint32_t magic;

    uint16_t header_version;
    uint16_t header_size;

    uint32_t target_mcu;

    uint32_t image_type;

    uint32_t firmware_version;
    uint32_t security_version;

    uint32_t image_size;

    uint32_t load_address;
    uint32_t vector_address;
    uint32_t entry_address;

    uint32_t key_id;
    uint32_t hash_algorithm;
    uint32_t signature_algorithm;

    uint8_t  image_hash[32];

    uint8_t  build_id[16];

    uint8_t  signature[64];

    uint8_t  reserved[...];

    uint32_t header_crc32;

} firmware_header_t;
```

关键字段包括：

```text
Magic
Header Version

Target MCU

Firmware Version
Security Version

Image Size

Load Address
Vector Address
Entry Address

Key ID

SHA256

Signature
```

---

# 10. 签名方式要稍微修改你的原始思路

你刚才描述的是：

> 下载完成 → 算固件 hash → public_key 对 hash 验签

大方向正确。

但最好不要：

```text
Sign(
    SHA256(APP.bin)
)
```

就结束了。

因为这样只认证了 APP 内容，没有认证：

```text
版本
目标 MCU
地址
大小
镜像类型
security version
```

正确做法应该是：

```text
Application
     ↓
SHA256
     ↓
image_hash
```

然后构造：

```text
Signed Manifest

target_mcu
firmware_version
security_version
image_size
load_address
vector_address
image_hash
key_id
...
```

再：

```text
SHA256(Signed Manifest)
          ↓
      manifest_hash
          ↓
Private Key Sign
          ↓
     signature
```

Bootloader：

```text
Firmware Header
       |
       v
重新构造 Manifest
       |
       v
SHA256
       |
       v
ECDSA Verify
       ^
       |
Public Key
```

这样攻击者改：

```text
image_size
load_address
firmware_version
target_mcu
```

都会导致：

```text
Signature Invalid
```

---

# 11. 推荐密码算法

对于 MCU Bootloader，我会优先：

```text
SHA-256
+
ECDSA P-256
```

原因主要是大量 MCU 都有对应硬件加速或者成熟软件实现。

定义：

```text
Hash:
SHA-256
32 bytes

Public Key:
P-256
64 bytes X||Y

Signature:
ECDSA
64 bytes R||S
```

如果 MCU 有：

```text
HASH Accelerator
PKA
ECC Accelerator
```

优先使用硬件。

如果没有，用精简的软件 crypto。

124K Bootloader 通常有希望放下：

```text
TinyUSB Vendor
Boot protocol
Flash driver
SHA256
ECDSA Verify
Boot Manager
```

但**不要直接把完整 mbedTLS 默认配置全部编进去**，否则 124K 很容易浪费很多空间。

只保留：

```text
SHA256
P-256
ECDSA Verify
必要 bignum
```

---

# 12. TinyUSB USB Device 设计

推荐：

```text
USB Device
└── Interface 0
      Class = 0xFF Vendor Specific

      EP1 OUT
      Bulk

      EP1 IN
      Bulk
```

TinyUSB 官方已经提供 Vendor descriptor template，本身就是 Vendor Specific Interface + Bulk IN/OUT 的形式。([GitHub][2])

大概：

```c
#define CFG_TUD_VENDOR 1
```

Descriptor：

```text
Interface
bInterfaceClass = 0xFF
```

Endpoint：

```text
0x01 OUT
0x81 IN
```

USB FS：

```text
Max Packet = 64
```

USB HS：

```text
Max Packet = 512
```

不过你的 Bootloader `DOWNLOAD DATA` 完全没必要限制为 USB packet 大小。

例如协议层：

```text
Download Chunk = 4096 bytes
```

TinyUSB 自动把它拆成很多 USB packet。

---

# 13. Windows 建议直接使用 WinUSB

很适合这个 Bootloader：

```text
PC Upgrade Tool
        |
      WinUSB
        |
 USB Vendor Class
        |
      TinyUSB
```

TinyUSB 官方文档目前明确说明 Vendor class 可以配合 Microsoft OS 2.0 compatible descriptor，让 Windows 加载 WinUSB，而无需自己做传统 INF 驱动。([TinyUSB][1])

PC 工具就可以基于：

```text
Windows  -> WinUSB
Linux    -> libusb
macOS    -> libusb
```

---

# 14. 自定义协议帧

不要假定：

> 一次 `tud_vendor_read()` 就一定对应一个完整协议包。

USB Bulk 有 packet/transfer 分段，所以应该设计真正的 Stream Parser。

推荐固定 32-byte header：

```c
typedef struct
{
    uint32_t magic;

    uint8_t  protocol_version;
    uint8_t  header_size;

    uint16_t command;

    uint32_t sequence;

    uint32_t payload_length;

    uint32_t session_id;

    uint32_t flags;

    uint32_t payload_crc32;

    uint32_t header_crc32;

} boot_frame_header_t;
```

正好：

```text
32 bytes
```

帧：

```text
+------------------------+
| Frame Header 32B       |
+------------------------+
| Payload N bytes        |
+------------------------+
```

---

# 15. 为什么协议还保留 CRC32

有人会问：

> USB 本身不是有 CRC 和重传了吗？

是的。

但这个 CRC 的作用不一样。

USB CRC：

```text
解决 USB 链路错误
```

协议 CRC：

```text
解决：
PC软件BUG
buffer错误
frame解析错误
内存错误
错误拼包
```

所以可以保留。

真正固件安全验证还是：

```text
SHA256
+
ECDSA
```

---

# 16. Bootloader 命令集

你规划的：

```text
GET_INFO
ERASE
DOWNLOAD
RESET
JUMP_APP
```

已经够用。

我认为没必要额外暴露 VERIFY。

因为：

> VERIFY 应该是 DOWNLOAD END 的强制内部阶段。

这是正确设计。

---

# 17. GET_INFO

Request：

```text
GET_INFO
```

Payload：

```text
empty
```

Response 建议至少返回：

```text
Protocol Version

Bootloader Version

MCU ID
Chip Revision
Unique ID

Flash Base
Flash Size
Erase Unit

USB Speed

Slot Count

APP A:
    address
    size
    state
    firmware version

APP B:
    address
    size
    state
    firmware version

Default Slot

Hash Algorithm
Signature Algorithm
Public Key ID

Max Download Chunk
```

例如：

```text
BOOT     : 1.2.0
PROTO    : 1
MCU      : STM32xxxx
FLASH    : 1024K

SLOT A
address  : 0x08020000
size     : 444K
state    : VALID
version  : 2.1.3

SLOT B
address  : 0x0808F000
size     : 444K
state    : EMPTY

DEFAULT  : A

HASH     : SHA256
SIGN     : ECDSA-P256
KEY-ID   : 1

MAX_CHUNK: 4096
```

这样 PC 工具无需硬编码 Flash map。

---

# 18. ERASE

不要让 PC 直接发送：

```text
ERASE 0x08020000 0x10000
```

这是危险设计。

应该：

```text
ERASE(SLOT_A)
```

或者：

```text
ERASE(SLOT_B)
```

Bootloader 自己决定：

```text
Start Address
End Address
Sector List
```

因此 Bootloader 永远拒绝访问：

```text
Bootloader
sLib
BCB
任意越界区域
```

也就是：

```c
if (!partition_is_app(slot))
    return BOOT_ERR_PROTECTED;
```

这是非常重要的一层安全边界。

---

# 19. DOWNLOAD 建议一个命令，三种 Flag

你不必增加：

```text
DOWNLOAD_START
DOWNLOAD_DATA
DOWNLOAD_END
```

三个 CMD。

保持：

```text
CMD_DOWNLOAD
```

然后：

```text
flags = START
flags = DATA
flags = END
```

即可。

---

# 20. DOWNLOAD START

第一包：

```text
CMD      = DOWNLOAD
FLAGS    = START
Payload  = Firmware Header
```

Bootloader：

```text
Firmware Header
       ↓
检查 Magic
       ↓
检查 Header Version
       ↓
检查 target_mcu
       ↓
检查 image_size
       ↓
检查 load_address
       ↓
检查 slot 范围
       ↓
检查 key_id
       ↓
验证 Manifest Signature
```

注意一个很不错的优化：

## 在真正下载 APP 之前先验证 Header Signature。

因为 Header 已经携带：

```text
image_hash
+
signature
```

所以 Bootloader 可以先确认：

> 这个固件包确实是厂家签名的。

只是此时还不能确认实际 APP body 是否匹配 `image_hash`。

这样可以尽早拒绝非法包。

---

# 21. ERASE 和 DOWNLOAD START 的顺序

因此我甚至推荐：

```text
GET_INFO
    ↓
DOWNLOAD START
    ↓
验证 Header Signature
    ↓
ERASE
    ↓
DOWNLOAD DATA
```

而不是：

```text
ERASE
↓
发现固件签名不合法
```

否则合法 APP 已经被无意义擦掉。

所以协议状态机允许：

```text
DOWNLOAD START
       ↓
HEADER_ACCEPTED
       ↓
ERASE
       ↓
READY_FOR_DATA
```

比较合理。

---

# 22. DOWNLOAD DATA

例如每块：

```text
4096 bytes
```

Payload：

```c
typedef struct
{
    uint32_t offset;

    uint32_t data_length;

    uint8_t data[];

} download_data_t;
```

例如：

```text
DOWNLOAD DATA
session = 0x12345678
offset  = 0
size    = 4096
```

下一帧必须：

```text
offset = 4096
```

第一版我建议：

> 强制顺序下载，不支持乱序。

这样 MCU 的逻辑非常简单：

```c
if (packet.offset != ctx.next_offset)
{
    return BOOT_ERR_SEQUENCE;
}
```

---

# 23. Session ID

DOWNLOAD START 成功后：

```text
MCU生成 session_id
```

例如：

```text
0x72A49183
```

后面的：

```text
ERASE
DOWNLOAD DATA
DOWNLOAD END
```

都必须带：

```text
session_id
```

可以避免 PC 旧包、重连等情况下把两个升级过程混在一起。

---

# 24. 下载期间可以同步计算 SHA256，但不能只相信它

例如：

```text
USB DATA
   ↓
SHA256_Update()
   ↓
Flash Write
```

这个可以用于提前发现问题。

但我建议最终 VERIFY：

> **必须重新从 Flash 回读数据计算 SHA256。**

这是很重要的区别。

错误设计：

```text
PC data
 ↓
SHA256
 ↓
Flash write

Hash正确
=> 认为Flash正确
```

这里可能存在：

```text
Flash program failure
bit error
错误地址
Cache问题
```

而 hash 仍然正确，因为你 hash 的是 RAM 中的数据。

正确：

```text
Download Complete
      ↓
Flash programming complete
      ↓
从 Flash Read Back
      ↓
SHA256
      ↓
image_hash
```

这样真正验证的是：

> Flash 里最终存在的东西。

---

# 25. DOWNLOAD END 自动 VERIFY

你提出：

> DOWNLOAD 完成自动 VERIFY

我完全赞成。

流程：

```text
DOWNLOAD END
     |
     v
检查 downloaded_size
     |
     v
Flash Read Back
     |
     v
SHA256(APP)
     |
     v
flash_hash
     |
     +-------- != header.image_hash
     |
     +------> HASH_ERROR
     |
     v
重新验证 Header Manifest Signature
     |
     v
Public Key Verify
     |
     +-------- FAIL
     |
     +------> SIGNATURE_ERROR
     |
     v
检查 Vector Table
     |
     v
检查 MSP / Reset_Handler
     |
     v
更新 BCB
SLOT = VALID
     |
     v
DOWNLOAD SUCCESS
```

整个下载事务直到：

```text
BCB SLOT_VALID
```

写成功前，都不算安装完成。

---

# 26. 升级时 slot 状态

这是掉电安全的核心。

开始之前：

```text
SLOT A = VALID
SLOT B = EMPTY
DEFAULT = A
```

升级 B：

```text
SLOT B = UPDATING
```

然后：

```text
erase B
download B
verify B
```

如果中途掉电：

```text
SLOT A = VALID
SLOT B = UPDATING
```

重新上电：

```text
拒绝启动 B
继续 A
```

非常安全。

只有全部验证结束：

```text
SLOT B = VALID
```

才允许：

```text
JUMP_APP(B)
```

---

# 27. JUMP_APP 的设计

建议 Payload：

```c
typedef struct
{
    uint8_t slot_id;

    uint8_t set_default;

    uint8_t reserved[2];

} jump_app_req_t;
```

例如：

```text
JUMP_APP
slot_id     = B
set_default = 1
```

Bootloader：

```text
B 是否存在？
      ↓
B Header是否合法？
      ↓
B hash/signature是否合法？
      ↓
更新 default_slot = B
      ↓
Reset
```

这里我反而**不推荐直接在 USB 回调后裸跳 APP**。

更稳的方法是：

```text
JUMP_APP
   ↓
写 BCB
   ↓
返回 ACK
   ↓
等待 IN transaction 完成
   ↓
NVIC_SystemReset()
   ↓
Bootloader
   ↓
选择 B
   ↓
Jump B
```

这样 APP 获得的是非常接近正常 Reset 的硬件环境。

比：

```text
USB运行中
DMA运行中
PLL运行中
IRQ打开
      ↓
直接 branch APP
```

可靠很多。

---

# 28. RESET

定义很简单：

```text
RESET
   ↓
ACK
   ↓
USB TX complete
   ↓
NVIC_SystemReset()
```

Reset 以后执行正常 Boot Policy：

```text
Key?
Default Slot?
APP Valid?
```

---

# 29. 你要求“下次默认为自动跳转”

这个就由 BCB：

```text
default_slot
```

解决。

例如用户：

```text
JUMP_APP(B)
```

默认语义就定义为：

```text
default_slot = B
```

那么以后：

```text
Power On
   ↓
Key 未长按
   ↓
Default = B
   ↓
Validate B
   ↓
Jump B
```

无需 PC 再发命令。

也可以扩展一个 flag：

```text
PERSISTENT = 1
```

与：

```text
ONE_SHOT = 1
```

但第一版没必要。

---

# 30. 上电长按 KEY 3 秒的正确实现

不能设计成：

```text
每次启动
无条件等3秒
再检查KEY
```

否则产品每次开机都慢 3 秒。

应该：

```text
Reset
  |
  v
读取 KEY
  |
  +---- 未按 ----> 立即正常 Boot
  |
  v
按下
  |
debounce 20~50ms
  |
  v
持续检测
  |
  +---- <3s松开 ---> 正常 Boot
  |
  v
持续 >=3秒
  |
  v
Upgrade Mode
```

于是正常用户：

```text
KEY没按
```

启动几乎没有额外延迟。

只有按键的人才等待 3 秒。

---

# 31. Boot 流程建议

完整：

```text
                    RESET
                      |
                      v
               Minimal HW Init
                      |
                      v
                  KEY Down?
                 /         \
               NO           YES
               |             |
               |       Hold >= 3 sec?
               |          /      \
               |        NO        YES
               |         |          |
               |         |          v
               |         |     Upgrade Mode
               |         |
               +---------+
                      |
                      v
                 Load BCB
                      |
                      v
              Default Slot Valid?
                /           \
              YES            NO
               |              |
               v              v
        Verify Default    Find Other
               |          Valid Slot
               |              |
               v              |
             Jump             |
                              |
                        None Valid?
                         /      \
                       NO        YES
                       |          |
                      Jump    Upgrade Mode
```

---

# 32. APP 每次启动都要不要重新算 SHA256？

这里有两个策略。

### 性能优先

下载时：

```text
Hash + Signature Verify
```

然后：

```text
BCB = VALID
```

正常每次启动：

```text
Header
Vector Table
BCB
```

快速判断。

优点：

```text
启动快
```

缺点：

如果 APP Flash 在下载后又被意外修改，就发现不了。

---

### 安全优先

每次启动：

```text
Header
 ↓
Hash entire application
 ↓
Signature verify
 ↓
Jump
```

对于四百 KB 固件：

```text
SHA256(400KB)
```

通常并没有大到无法接受，特别是硬件 SHA 加速存在时。

所以如果你这个项目是真的在建立 Secure Boot Chain，我更推荐：

> 每次 Boot 都重新从 Flash 计算 image hash。

安全逻辑就是：

```text
Root Public Key
      |
      v
Signed Manifest
      |
      v
Expected Image Hash
      |
      v
Flash Image
```

---

# 33. 一个非常重要的多 APP 问题

你说：

> 如果有多个 APP，能够选择指定跳转。

这完全可以。

但是有个经常被忽略的问题：

# Cortex-M 应用通常不是位置无关代码。

例如 APP A：

```text
0x08020200
```

APP B：

```text
0x0808F200
```

如果是**两个不同 APP**：

```text
APP_A.ld
FLASH ORIGIN = 0x08020200

APP_B.ld
FLASH ORIGIN = 0x0808F200
```

没问题。

Bootloader 根据 slot 跳。

---

# 34. 但如果 A/B 是“同一个 APP 的升级双分区”

事情就不同。

假设同一个 `app.bin` 是链接到：

```text
0x08020200
```

你直接把它写到：

```text
0x0808F200
```

通常：

> 不能直接运行。

因为 binary 里面可能存在大量：

```text
绝对地址
Vector Table
链接地址
常量地址
函数地址
```

所以真正 A/B 升级有几种选择：

### 方案 A：分别链接

生成：

```text
app_slot_a.bin
app_slot_b.bin
```

分别：

```text
0x08020200
0x0808F200
```

这是最容易实现的直接执行方案。

### 方案 B：位置无关代码

PIC。

在 Cortex-M 裸机项目中复杂度会明显上升，我通常不作为第一选择。

### 方案 C：硬件 Bank Swap / Remap

如果 MCU 支持：

```text
Dual Bank
Bank Swap
Address Remap
```

这是很漂亮的 A/B 方案。

### 方案 D：B 是下载暂存区

```text
APP A = Execution Slot
APP B = Upgrade Slot
```

B 验证成功后：

```text
Bootloader
   ↓
copy B -> A
   ↓
Verify A
   ↓
Jump A
```

这样应用永远链接到同一个地址。

---

# 35. 如果你所谓“多个 APP”是真正不同的应用

那最简单：

```text
APP A
独立链接地址

APP B
独立链接地址
```

Firmware Header：

```text
image_type = APP_A
```

或者：

```text
image_type = APP_B
```

并且：

```text
load_address
```

被签名保护。

这样不会把：

```text
APP_A firmware
```

错误刷进：

```text
APP_B
```

---

# 36. APP 跳转前的检查

至少：

```text
Header valid

Hash valid

Signature valid

Vector address valid

MSP valid

Reset_Handler valid
```

MSP：

```text
必须落在合法 SRAM 区
```

不要简单只判断：

```c
0x20000000
```

因为现在不少 STM32 有：

```text
DTCM
AXI SRAM
SRAM1
SRAM2
SRAM3
```

应该维护真正的 SRAM Range Table。

Reset_Handler：

```text
bit0 == 1
```

因为 Cortex-M 是 Thumb：

```c
(reset_handler & 1) == 1
```

并且：

```text
reset_handler & ~1
```

必须落在当前 APP 的可执行范围。

---

# 37. JUMP_APP 最终实现

如果使用我推荐的：

```text
JUMP_APP -> Reset -> Bootloader -> Jump
```

真正的 jump 函数可以比较干净：

```c
static void boot_jump(uint32_t vector_addr)
{
    uint32_t new_msp;
    uint32_t reset_handler;

    new_msp       = *(uint32_t *)(vector_addr + 0);
    reset_handler = *(uint32_t *)(vector_addr + 4);

    __disable_irq();

    SysTick->CTRL = 0;
    SysTick->LOAD = 0;
    SysTick->VAL  = 0;

    boot_usb_deinit();

    boot_disable_peripherals();

    boot_clear_nvic();

    SCB->VTOR = vector_addr;

    __DSB();
    __ISB();

    __set_MSP(new_msp);

    ((void (*)(void))reset_handler)();
}
```

具体到 Cortex-M7 还要考虑：

```text
I-Cache
D-Cache
DMA
MPU
```

所以真正实现需要根据 MCU 调整。

---

# 38. Upgrade 状态机

推荐明确做状态机：

```text
BOOT_IDLE
    |
    | DOWNLOAD START
    v
HEADER_VERIFY
    |
    v
HEADER_ACCEPTED
    |
    | ERASE
    v
ERASING
    |
    v
READY
    |
    | DOWNLOAD DATA
    v
DOWNLOADING
    |
    | DOWNLOAD END
    v
VERIFY_HASH
    |
    v
VERIFY_SIGNATURE
    |
    v
VERIFY_VECTOR
    |
    v
COMMIT
    |
    v
COMPLETE
```

错误统一：

```text
ERROR
```

例如：

```c
typedef enum
{
    UPGRADE_IDLE,

    UPGRADE_HEADER_ACCEPTED,

    UPGRADE_ERASING,

    UPGRADE_READY,

    UPGRADE_DOWNLOADING,

    UPGRADE_VERIFY_HASH,

    UPGRADE_VERIFY_SIGNATURE,

    UPGRADE_COMMIT,

    UPGRADE_COMPLETE,

    UPGRADE_ERROR,

} upgrade_state_t;
```

---

# 39. Boot Protocol 的错误码一定要设计

例如：

```c
typedef enum
{
    BOOT_OK = 0,

    BOOT_ERR_INVALID_COMMAND,
    BOOT_ERR_INVALID_STATE,

    BOOT_ERR_BAD_FRAME,
    BOOT_ERR_BAD_CRC,
    BOOT_ERR_BAD_SEQUENCE,

    BOOT_ERR_INVALID_SLOT,
    BOOT_ERR_PROTECTED_AREA,

    BOOT_ERR_INVALID_HEADER,
    BOOT_ERR_WRONG_TARGET,
    BOOT_ERR_IMAGE_TOO_LARGE,

    BOOT_ERR_ERASE,
    BOOT_ERR_PROGRAM,

    BOOT_ERR_HASH_MISMATCH,
    BOOT_ERR_SIGNATURE,

    BOOT_ERR_INVALID_VECTOR,

    BOOT_ERR_SESSION,

} boot_status_t;
```

PC 升级工具才能明确告诉用户：

```text
不是“升级失败”
```

而是：

```text
Firmware target mismatch

Signature invalid

Image too large

Flash erase failed

Hash mismatch
```

调试体验会差很多。

---

# 40. TinyUSB 线程模型

裸机：

```c
int main(void)
{
    boot_minimal_init();

    if (!boot_should_enter_upgrade())
    {
        boot_auto_start();
    }

    boot_usb_init();

    while (1)
    {
        tud_task();

        usb_transport_task();

        boot_protocol_task();

        upgrade_task();

        watchdog_feed();
    }
}
```

TinyUSB 官方集成方式就是在设备模式初始化 stack，然后由 `tud_task()` 处理从 USB IRQ 排队过来的设备事件。([TinyUSB][3])

这样不要在：

```c
tud_vendor_rx_cb()
```

里直接：

```text
Erase 128KB Flash
ECDSA verify
SHA256 500KB
```

callback 最好只是：

```text
收数据
放队列
通知 protocol task
```

---

# 41. Flash 擦写和 USB 的一个硬件陷阱

这个必须专门提醒。

有些 MCU 在：

```text
Internal Flash Erase / Program
```

期间，如果 CPU 还从**同一个 Flash Bank**取指，会 stall。

于是：

```text
Bootloader正在Flash运行
       |
       v
Erase APP
       |
       v
Flash Busy
       |
       v
CPU取指受阻
       |
       v
USB IRQ不能及时处理
```

可能出现：

```text
USB NAK
短暂卡顿
甚至超时
```

所以具体 MCU 要研究：

```text
Read While Write
Single Bank / Dual Bank
Flash operation blocking time
```

常见解决方式：

1. Host timeout 给足；
2. Flash program 分小块；
3. Flash critical routine 放 RAM；
4. 能用 Dual Bank 就尽量在另一 Bank 擦写；
5. USB protocol 状态机不要把一次耗时操作误判为链路断开。

这也是为什么最终 MCU 型号非常关键。

---

# 42. 最终升级时序

我会把你的完整协议定成这样：

```text
PC                                      Bootloader
 |                                           |
 |------------- GET_INFO ------------------->|
 |<------------ INFO ------------------------|
 |                                           |
 |-------- DOWNLOAD START + HEADER ---------->|
 |                                      Check header
 |                                      Check target
 |                                      Verify signature
 |<---------- SESSION_ID + OK ---------------|
 |                                           |
 |------------- ERASE SLOT ----------------->|
 |                                      Mark UPDATING
 |                                      Erase target
 |<---------------- OK ----------------------|
 |                                           |
 |-------- DOWNLOAD DATA offset=0 ---------->|
 |                                      Program Flash
 |<--------------- ACK ----------------------|
 |                                           |
 |-------- DOWNLOAD DATA offset=4096 ------->|
 |                                      Program Flash
 |<--------------- ACK ----------------------|
 |                                           |
 |                    ...                    |
 |                                           |
 |------------- DOWNLOAD END --------------->|
 |                                           |
 |                                      Read Flash
 |                                      SHA256
 |                                      Compare hash
 |                                      Verify signature
 |                                      Verify Vector
 |                                      BCB = VALID
 |                                           |
 |<-------- DOWNLOAD SUCCESS ----------------|
 |                                           |
 |------------- JUMP_APP(B) ---------------->|
 |                                      default = B
 |<--------------- ACK ----------------------|
 |                                           |
 |                                      System Reset
 |                                           |
 |                                      Bootloader
 |                                      Verify B
 |                                      Jump B
```

这个流程我认为已经非常接近可以实际写代码的规格了。

---

# 43. 软件工程建议

最终目录建议：

```text
bootloader/
│
├── main/
│   └── boot_main.c
│
├── boot/
│   ├── boot_manager.c
│   ├── boot_policy.c
│   ├── boot_jump.c
│   ├── boot_key.c
│   └── boot_bcb.c
│
├── usb/
│   ├── tusb_config.h
│   ├── usb_descriptors.c
│   ├── usb_transport.c
│   └── tinyusb/
│
├── protocol/
│   ├── boot_protocol.c
│   ├── boot_frame.c
│   └── boot_command.c
│
├── upgrade/
│   ├── upgrade_manager.c
│   ├── firmware_header.c
│   ├── firmware_verify.c
│   └── partition.c
│
├── security/
│   ├── slib.c
│   ├── sha256.c
│   └── ecdsa_verify.c
│
├── flash/
│   ├── flash_if.c
│   └── flash_map.h
│
└── bsp/
    ├── key.c
    ├── led.c
    └── board.c
```

依赖关系保持：

```text
TinyUSB
   ↓
usb_transport
   ↓
boot_protocol
   ↓
upgrade_manager
   ↓
+----------------+
| firmware_verify|
| partition      |
| flash_if       |
+----------------+
```

不要出现这种依赖：

```text
TinyUSB callback
       ↓
HAL_FLASH_Erase()
       ↓
ECDSA()
```

这样后期很难维护。

---

# 44. 我会给这套设计的最终定义

你的需求可以正式定义成：

```text
USB:
TinyUSB Vendor Specific
Bulk IN / Bulk OUT
WinUSB/libusb

Boot Entry:
KEY continuously pressed >= 3 seconds
OR no valid APP

Flash:
Bootloader       124K
sLib/Public Key    4K
APP partitions
BCB redundant metadata

Protocol:
GET_INFO
ERASE
DOWNLOAD
RESET
JUMP_APP

DOWNLOAD:
START
DATA
END

Integrity:
Protocol CRC32

Firmware Integrity:
SHA-256

Firmware Authenticity:
ECDSA P-256

Trust Anchor:
write-protected Public Key

Upgrade Transaction:
EMPTY
 -> UPDATING
 -> VERIFIED
 -> VALID

Multi APP:
Partition table
+
Default Slot in BCB
+
JUMP_APP(slot)

Boot:
Key > 3s
    -> Bootloader

otherwise
    -> Default APP
    -> fallback APP
    -> Bootloader
```

### 有三个实现点我认为是必须坚持的

**第一，DOWNLOAD 的最终 hash 一定从 Flash 回读计算，而不是只算 USB 收到的数据。**

**第二，public key 所在 sLib 必须不可被普通升级命令修改，并尽量用硬件 Write Protect 保护。**

**第三，如果 APP A/B 是同一个程序的冗余镜像，要提前解决“APP 链接地址不同”的问题；不能默认同一个 Cortex-M `.bin` 随便烧到两个地址都能运行。**

只要这三个问题处理正确，这个架构不仅“可行”，而且已经可以作为实际产品 Bootloader 的基础架构。下一步最适合直接把它进一步固化成 **`flash_map.h + firmware_header.h + boot_protocol.h + BCB 数据结构 + 完整状态机`**，然后才开始写 TinyUSB Vendor 的收发代码。

[1]: https://docs.tinyusb.org/en/latest/?utm_source=chatgpt.com "TinyUSB"
[2]: https://github.com/hathach/tinyusb/blob/master/src/device/usbd.h?utm_source=chatgpt.com "tinyusb/src/device/usbd.h at master · hathach/tinyusb · GitHub"
[3]: https://docs.tinyusb.org/en/latest/porting.html?utm_source=chatgpt.com "Porting - TinyUSB"
