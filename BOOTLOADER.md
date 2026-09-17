# AT32F403A Bootloader

## Flash layout

| Region | Address | Size |
|---|---:|---:|
| Bootloader | `0x08000000` | 124 KiB |
| sLib instruction/data | `0x0801F000` | 4 KiB |
| App A package/header | `0x08020000` | 4 KiB |
| App A vector/body | `0x08021000` | 380 KiB |
| App B package/header | `0x08090000` | 4 KiB |
| App B vector/body | `0x08091000` | 380 KiB |
| BCB0 / BCB1 | `0x080FE000` / `0x080FF000` | 4 KiB each |

App A 和 App B 使用独立链接地址。Bootloader 不复制或重定位 App，校验后直接进入各自的 Reset_Handler。链接脚本确保 Bootloader 永远不会进入 sLib 或 App 区域。

## Build Bootloader

```sh
cmake --preset Debug
cmake --build --preset Debug
```

工程只构建 Bootloader，输出位于 `build/Debug/at32_usb_dfu.elf` 和
`build/Debug/at32_usb_dfu.bin`。`apps/` 中的 App ELF/BIN 仅作为升级输入，
不会被 CMake 构建。

## Sign/package

私钥只由 PC 工具读取，不进入仓库和 MCU：

```sh
python3 tools/firmware_pack.py \
  --input apps/auto_foc_triangle_0x08021000_380K.bin \
  --slot A \
  --private-key /home/qxun/secure_keys/ecdsa_private.pem \
  --public-key /home/qxun/secure_keys/ecdsa_public.pem \
  --firmware-version 1 \
  --security-version 1 \
  --output build/app_a_v1.atfw
```

签名覆盖 target MCU、slot、版本、安全版本、大小、链接地址、入口地址和 App SHA-256。包头 CRC32 只负责传输/格式错误检测，不替代 ECDSA。

## Upgrade

PA0 已配置内部上拉。复位时未按键会立刻验证并启动默认 App；PA0 保持低电平 3 秒进入升级模式；没有有效 App 时也会进入升级模式。

Linux 上可直接使用系统 `libusb-1.0`，也支持 PyUSB。执行：

```sh
sudo install -m 0644 udev/60-at32-secure-bootloader.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules
sudo udevadm trigger --subsystem-match=usb --attr-match=idVendor=cafe

python3 tools/usb_upgrade.py info
python3 tools/usb_upgrade.py upgrade build/app_a_v1.atfw --jump
python3 tools/usb_upgrade.py jump B
```

安装规则后重新插拔 USB，`/dev/bus/usb/...` 应属于 `root:plugdev` 且组可读写。

USB 枚举为 CDC ACM + Vendor Specific 复合设备，共 3 个接口：接口 0 是实际升级使用的 Vendor Bulk（OUT `0x01`、IN `0x81`）；CDC ACM 通过 IAD 将接口 1（控制）和接口 2（数据）组合为一个功能，使用通知 IN `0x82` 和数据 OUT `0x03`/IN `0x83`。CDC 仅用于演示复合设备描述符，没有接入 Bootloader 业务逻辑。

升级协议最大数据块为 4096 字节。升级顺序固定为签名头预验证、标记 `UPDATING`、擦除、顺序写入、从 Flash 回读 SHA-256、再次验签、校验向量表、最后提交双副本 BCB。任何一步失败都不会把 slot 标记为 `VALID`。

## 面试级项目复盘与实现原理

### 1. 项目目标与边界

这是一个运行在 AT32F403A、Cortex-M4、1 MiB 片内 Flash 上的裸机安全 A/B Bootloader。它解决的不只是“通过 USB 写 Flash”，而是下列问题：

- Bootloader、受保护公钥、App A、App B 和 BCB 之间的物理隔离。
- App A/B 使用独立链接地址，不做运行时重定位，校验后原地执行（XIP）。
- PC 端私钥签名，MCU 只保存公钥并执行 SHA-256 + ECDSA P-256 验签。
- 下载中断、掉电、错包或损坏镜像不得被启动。
- 默认 App 失效时自动验证备用 App，两者都失效时回到 USB 恢复模式。
- USB 枚为 CDC ACM + Vendor Specific 复合设备，升级业务只使用 Vendor Bulk。

项目不构建 App。`apps/` 中的 ELF/BIN 只是测试输入，App 应在各自工程中使用正确的独立链接地址构建。

### 2. 分层架构和源码职责

```text
PC firmware_pack.py
    └─ 生成 4 KiB 签名头 + App 主体的 .atfw

PC usb_upgrade.py
    └─ USB Vendor Bulk 请求/响应协议
             │
TinyUSB + AT32 FSDEV DCD
    └─ USB 枚举、端点、FIFO 和数据收发
             │
usb_boot.c
    └─ 帧解析、命令分发、下载会话状态机
             │
boot_flash.c / boot_security.c / boot_bcb.c
    └─ Flash 擦写、哈希验签、双副本持久化
             │
boot_manager.c
    └─ 启动选择、A/B 回退、CPU 状态清理和 App 跳转
```

关键文件：

| 文件 | 职责 |
|---|---|
| `project/inc/bootloader.h` | Flash 地址、镜像头、USB 帧头、BCB 结构和公共接口 |
| `project/src/boot_manager.c` | PA0 进入升级、默认/备用槽选择、启动前完整校验、跳转 App |
| `project/src/boot_flash.c` | 2 KiB 扇区擦除、32 位编程、边界与 Flash 回读校验 |
| `project/src/boot_security.c` | sLib 公钥导入、PSA SHA-256、ECDSA P-256 验签和向量表检查 |
| `project/src/boot_bcb.c` | BCB0/BCB1 选举、世代号和乒乓轮换提交 |
| `project/src/usb_boot.c` | Vendor 协议帧、命令、响应、下载状态机 |
| `project/src/usb_descriptors.c` | CDC + Vendor 复合设备、IAD、接口和端点描述符 |
| `project/src/usb_port.c` | AT32 USBFS 时钟、ACC 校准、中断映射和 TinyUSB ISR |
| `tools/firmware_pack.py` | App 地址预检、哈希、PC 私钥签名、`.atfw` 打包 |
| `tools/usb_upgrade.py` | libusb/PyUSB 上位机、协议编解码和升级流程 |

### 3. Flash 规划与对齐原则

AT32F403AxG 本项目按 1 MiB Flash、96 KiB SRAM、2 KiB 最小擦除扇区设计：

```text
0x08000000  +----------------------------------+
            | Bootloader, 124 KiB              |
0x0801F000  +----------------------------------+
            | sLib instruction, sector 62, 2K |
0x0801F800  +----------------------------------+
            | sLib data/public key, sector 63 |
0x08020000  +----------------------------------+
            | App A header, 4 KiB              |
0x08021000  +----------------------------------+
            | App A body/vector, <= 380 KiB    |
0x08080000  +----------------------------------+
            | reserved, 64 KiB                 |
0x08090000  +----------------------------------+
            | App B header, 4 KiB              |
0x08091000  +----------------------------------+
            | App B body/vector, <= 380 KiB    |
0x080F0000  +----------------------------------+
            | reserved, 56 KiB                 |
0x080FE000  +----------------------------------+
            | BCB0, 4 KiB                      |
0x080FF000  +----------------------------------+
            | BCB1, 4 KiB                      |
0x08100000  +----------------------------------+
```

Bootloader 链接脚本只声明 `0x08000000 + 124K`，因此链接器不会把普通代码放入 sLib 或 App 区。sLib 启用后，扇区 62/63 不再是可供普通代码分配的 Flash。

每个 slot 是 `0x60000` 字节，其中第一个 4 KiB 专用于固件头，App 向量表和主体从 `slot_base + 0x1000` 开始。因此 App A/B 链接地址分别必须是 `0x08021000` 和 `0x08091000`。

4 KiB 固件头的价值在于：

- 头和 App 主体都落在整齐的 4 KiB 边界，同时满足 2 KiB 擦除对齐。
- App 链接时不需要把非执行清单混入向量表。
- 头部可以预留算法、密钥 ID、版本策略和产品扩展字段。
- PC 产出的 `.atfw` 就是 `4096-byte header || raw app body`，设备不需要 ELF 解析器。

### 4. 固件包格式和签名边界

`boot_firmware_header_t` 固定为 4096 字节：

| 偏移 | 大小 | 内容 | 安全意义 |
|---:|---:|---|---|
| 0 | 4 | magic `ATFW` | 格式识别 |
| 4 | 2+2 | header version/size | 协议演进与 4 KiB 约束 |
| 8 | 4 | target MCU | 防止跨型号误烧 |
| 12 | 4 | image type/slot | 将镜像和 A/B 物理地址绑定 |
| 16 | 4 | firmware version | 业务版本 |
| 20 | 4 | security version | 安全防回滚版本 |
| 24 | 4 | image size | 主体长度与越界检查 |
| 28 | 12 | load/vector/entry address | 将签名固件绑定到执行地址 |
| 40 | 12 | key/hash/signature algorithm IDs | 防止算法或密钥混用 |
| 52 | 32 | image SHA-256 | 绑定 App 主体 |
| 84 | 16 | build ID | 构建识别 |
| 100 | 64 | raw ECDSA signature | P-256 `r || s` |
| 164 | 3928 | reserved | 后续扩展，当前为 0 |
| 4092 | 4 | header CRC32 | 前 4092 字节的损坏检查 |

签名只覆盖 `signature` 字段之前的 100 字节：

```text
manifest_digest = SHA256(header[0:100])
signature       = ECDSA-P256-SIGN(private_key, manifest_digest)
```

这 100 字节已经包含 `image_hash`，所以一个 `(signature, public_key)` 形成了两级信任关系：

```text
public_key
    └─ 验证 signature，证明清单前100字节未被篡改
                  └─ 其中可信的 image_hash
                              └─ 验证 Flash 中的 App 主体
```

不是“一次 ECDSA 同时直接验证两个 SHA-256”，而是 ECDSA 验证包含主体哈希的清单，再用该哈希验证主体。

`header_crc32` 和 USB 帧 CRC32 只负责快速发现格式错误、传输损坏和 Flash 随机错误；CRC 可被攻击者重新计算，不能代替数字签名。

### 5. PC 打包与密钥生命周期

产品私钥是 PEM 文件，只存在 PC 端的受控签名环境；MCU 中仅保存 65 字节 SEC1 非压缩公钥：

```text
0x04 || X[32] || Y[32]
```

`firmware_pack.py` 在签名前会：

1. 检查 App 大小是否在 380 KiB 容量内。
2. 读取 bin 头两个向量，检查 MSP 是否在 96 KiB SRAM 中且 8 字节对齐。
3. 检查 Reset Handler 是否带 Thumb 位并位于目标 App 地址范围。
4. 计算 App 主体 SHA-256。
5. 可选将私钥导出的公钥和 `--public-key` 比较，避免用错产品密钥。
6. 调用 OpenSSL 生成 DER ECDSA 签名，再转换为 MCU PSA 使用的 64 字节 `r || s`。
7. 对前 4092 字节计算与 MCU 一致的 zlib CRC32。

如果出现 `reset handler ... is outside App B`，通常不是打包器错误，而是输入 bin 仍按 App A 地址链接，或传入了不含正确向量表的 bin。打包器不会修改绝对地址，也不会对 App 做重定位。

### 6. sLib 公钥与 Mbed TLS PSA 验签

sLib 规划为：

- 扇区 62，`0x0801F000`：sLib instruction。
- 扇区 63，`0x0801F800`：sLib data，从 `0x0801F800` 开始保存公钥记录。

公钥记录含 `SLIB` magic、记录版本、SEC1 编码、65 字节长度、`key_id=1` 和公钥。制造测试源码 `slib_provision.c` 要求 USART1/PA10 在 60 秒内连续收到 `PROVISION SLIB`，然后配置 62/63/63 范围、擦写记录、回读验证并复位。量产配置必须保证电源稳定，并在配置前确认密钥和保护范围；受保护扇区不应再被当作普通代码或数据区。

当前常规 Bootloader CMake 目标没有链接 `slib_provision.c`，避免量产配置逻辑出现在日常升级固件中。

Bootloader 启动时用 PSA Crypto 将该 SEC1 公钥导入为：

- 曲线：NIST P-256 / `secp256r1`。
- 用途：仅 `VERIFY_HASH`。
- 算法：ECDSA with SHA-256。
- 密钥位数：256 bit。

Mbed TLS 4.2 只构建 TF-PSA-Crypto 静态加密子集，不包含 TLS、X.509、密钥生成和 MCU 端私钥签名业务。密钥槽采用静态内存，当前芯片没有接入密码学 TRNG，随机回调明确返回 `PSA_ERROR_INSUFFICIENT_ENTROPY`。纯 SHA-256 和公钥验签不需要随机数，所以不影响当前功能。

如果未来加入设备端密钥生成、签名、随机 challenge 或会话加密，必须先接入真实熵源，不能把 SysTick、UID 或伪随机数当作密码学随机数。

### 7. 启动决策状态机

```text
MCU reset
   |
   +-- PA0 low after 30 ms debounce and held for 3 s?
   |       `-- yes: force USB upgrade mode
   |
   +-- load newest valid BCB0/BCB1
   +-- import protected P-256 public key
   +-- validate default slot directly from Flash
   |       `-- valid: jump to App
   +-- validate alternate slot directly from Flash
   |       `-- valid: jump to App
   `-- enter TinyUSB upgrade loop
```

`try_slot()` 的设计要点：

- BCB 中是 `VALID` 也不会盲目跳转，每次启动都从 Flash 重新验头、计算主体哈希、验签和检查向量。
- 已登记 `VALID` 的槽位验证失败时，将其持久化为 `INVALID`，然后尝试备用槽。
- BCB 中未登记，但 Flash 中存在完整合法 `ATFW` 镜像时，允许发现该镜像并补登记为 `VALID`，便于工装和调试器预烧。
- 如果公钥记录或 PSA 初始化失败，所有签名校验都失败，系统不会降级成无签名启动。

### 8. BCB 持久化状态机

每个 slot 的持久化状态是：

| 状态 | 含义 | 是否允许启动 |
|---|---|---:|
| `EMPTY` | 未登记镜像 | 否 |
| `UPDATING` | 已开始升级，内容可能不完整 | 否 |
| `VALID` | 完整安全校验通过 | 是，但启动前仍重验 |
| `INVALID` | 终检、写入或启动校验失败 | 否 |
| `BAD` | 为永久故障策略预留 | 否，当前流程未自动进入 |

正常升级的状态变化：

```text
EMPTY / INVALID / VALID
          |
          | ERASE: first persist metadata
          v
       UPDATING
          |
          | all DATA + Flash hash + signature + vector pass
          v
         VALID
```

BCB 本身是 64 字节记录，包括 magic、格式版本、`generation`、默认槽、A/B 状态、最近错误、固件/安全版本、镜像大小和 CRC32。BCB CRC 保护的是一致性，不是攻击者篡改防护；真正决定代码能否执行的仍是镜像数字签名。

### 9. BCB0/BCB1 乒乓轮换和掉电恢复

BCB0 与 BCB1 不是日志数组，而是两个互为备份的 4 KiB 页。每页需要擦除两个 2 KiB 物理扇区，并在页起始位置写一份 64 字节 BCB。

首次两份都无效时，只在 RAM 建立 `generation=0/default=A/A,B=EMPTY`，不立即写 Flash。之后的提交为：

```text
commit #1 -> BCB0 generation=1
commit #2 -> BCB1 generation=2
commit #3 -> BCB0 generation=3
commit #4 -> BCB1 generation=4
```

每次提交的原子性步骤：

1. 把当前 BCB 拷贝到 RAM 并修改。
2. `generation = current + 1`，重算 CRC32。
3. 擦除当前副本的另一页。
4. 写入新记录并从 Flash 回读验证。
5. 只有新页完整有效后才切换 RAM 中的当前地址。

启动时分别检查两份 magic、格式版本、默认槽范围和 CRC：仅一份有效就选它，两份都有效就选 `generation` 更新的。`(int32_t)(left - right) > 0` 允许 32 位世代号从 `0xffffffff` 回绕到 0 后仍判断新旧。

掉电场景：

- 擦除或写新页时掉电：新页 CRC 无效，旧页仍完整。
- 新页已完整写入、RAM 尚未切换时掉电：复位后通过较新 `generation` 选到新页。
- 只有一页受损：选择好页；下次提交时会重写受损页。

一次带 `--jump` 的成功升级通常产生三次 BCB 提交：ERASE 写 `UPDATING`，END 写 `VALID`，JUMP 更改 `default_slot`。BCB 不在每个 DATA 包到达时写入，避免不必要的 Flash 磨损。

### 10. USB 复合设备、IAD 与 FSDEV

CDC ACM 是 Communications Device Class，不是 USB Misc 类。但当一个设备同时包含 Vendor 和由两个接口构成的 CDC 功能时，Device Descriptor 使用 `Misc/Common/IAD`，告诉主机根据 IAD 将 CDC 控制接口和数据接口分组。

```text
Device: class MISC, subclass COMMON, protocol IAD

Interface 0: Vendor Specific - actual boot protocol
    OUT 0x01, Bulk, 64 bytes
    IN  0x81, Bulk, 64 bytes

IAD: CDC function contains interfaces 1 and 2
    Interface 1: CDC ACM control
        IN 0x82, Interrupt notification, 8 bytes
    Interface 2: CDC data
        OUT 0x03, Bulk, 64 bytes
        IN  0x83, Bulk, 64 bytes
```

Vendor 保持在 interface 0，是因为当前 PC 工具固定 claim interface 0。CDC 只用于展示复合描述符、IAD 和系统枚举，不承载 Bootloader 升级命令。

TinyUSB 配置中 EP0 和 FS Bulk 最大包都是 64 字节，Vendor 软件 RX/TX FIFO 各为 512 字节，CDC FIFO 各为 64 字节。一个 4 KiB 协议块会被 USB FS 分成多个 64 字节包，TinyUSB 和上层组帧缓冲区会将其重组，所以“USB 端点包”和“Bootloader 协议帧”不是同一层次。

AT32F403A 设备控制器使用 TinyUSB `stm32_fsdev` 类型驱动，原因是它的 USBFS 寄存器和端点/PMA 模型属于 FSDEV 架构。`usb_port_init()` 中看不到 PA11/PA12 普通 GPIO 初始化，是因为 USBFS 外设时钟开启后硬件自动接管 USB_DM/DP 引脚。该函数完成的是：

- 将 USB 中断映射到专用向量，避免与 CAN1 共用入口。
- 使用 HICK 作为 USB 时钟，由 ACC 根据 SOF 自动校准。
- 开启 USBFS/ACC 时钟并设置 USB 中断优先级。
- 将 USBFS 中断统一交给 `tud_int_handler(0)`。

Vendor API 在本项目中的职责：

- `tud_vendor_mounted()`：Vendor 接口是否已被主机配置，用于判断能否发响应。
- `tud_vendor_available()`：RX FIFO 中有多少主机 OUT 数据可读。
- `tud_vendor_read()`：把 Vendor OUT FIFO 数据搬到 Bootloader 组帧缓冲区。
- `tud_vendor_write()`：把响应放入 Vendor IN FIFO，返回已接受字节数。
- `tud_vendor_write_flush()`：请求 TinyUSB 尽快提交尚未凑满一包的 IN 数据。

### 11. USB 升级帧格式

协议使用小端字节序，每帧是：

```text
[32-byte boot_frame_header_t] [payload_length bytes]
```

| 偏移 | 大小 | 字段 | 作用 |
|---:|---:|---|---|
| 0 | 4 | magic `ATUP` | 字节流重同步 |
| 4 | 1 | protocol version | 协议版本 |
| 5 | 1 | header size | 当前固定 32 |
| 6 | 2 | command | 命令；响应置 `0x8000` |
| 8 | 4 | sequence | 请求/响应匹配 |
| 12 | 4 | payload length | 负载长度 |
| 16 | 4 | session ID | 绑定一次下载会话 |
| 20 | 4 | flags | START/DATA/END 或响应标志 |
| 24 | 4 | payload CRC32 | 负载损坏检查 |
| 28 | 4 | header CRC32 | 前 28 字节损坏检查 |

标准响应 payload 至少包含 16 字节：

```text
int32_t  status
uint32_t detail
uint32_t session_id
uint32_t next_offset
```

`GET_INFO` 会在这 16 字节之后追加芯片 UID、Flash/slot 布局、BCB generation、A/B 状态和版本、块大小、算法代号和 key ID。

设备端 RX 缓冲区大小为：

```text
32-byte frame header + 8-byte DATA prefix + 4096-byte chunk = 4136 bytes
```

解析器如果遇到错误 magic、版本、长度或帧头 CRC，每次丢弃一字节继续寻找 `ATUP`；如果帧头合法但 payload 尚未收齐，则保留已有数据等待下一轮。payload 收齐后再校验 CRC 并分发命令。

当前采用半双工的 stop-and-wait 模式：设备尚有响应未放入 TinyUSB TX FIFO 时不读取下一条命令，PC 也是每发一帧就等待对应 `sequence` 的响应，因此当前不做请求流水线。

### 12. 命令集和上位机流转

| 命令 | 值 | 说明 |
|---|---:|---|
| `GET_INFO` | 1 | 只读查询设备、slot 和 BCB 信息 |
| `ERASE` | 2 | 在已接受签名头后擦除目标 slot |
| `DOWNLOAD` | 3 | 由 flags 区分 START、DATA、END |
| `RESET` | 4 | 回复成功后延时系统复位 |
| `JUMP_APP` | 5 | 重验目标 slot，可设默认槽，然后复位 |

完整下载流程：

```text
PC                                             MCU
 |                                              |
 | DOWNLOAD/START + complete 4 KiB header       |
 |--------------------------------------------->| validate fields/CRC/signature
 |<---------------------------------------------| OK, new session_id, offset=0
 |                                              |
 | ERASE(slot, session)                         |
 |--------------------------------------------->| BCB=UPDATING, erase, write header
 |<---------------------------------------------| OK
 |                                              |
 | DOWNLOAD/DATA(offset,length,data)             |
 |--------------------------------------------->| program + immediate read-back
 |<---------------------------------------------| OK, next_offset
 |                 repeat in <=4096-byte chunks |
 |                                              |
 | DOWNLOAD/END                                 |
 |--------------------------------------------->| hash Flash, verify signature/vector
 |                                              | BCB=VALID
 |<---------------------------------------------| OK
 |                                              |
 | JUMP_APP(slot,set_default=1)                 |
 |--------------------------------------------->| full image validation, update BCB
 |<---------------------------------------------| OK, schedule reset
 |                                              | reset after about 250 ms
```

#### START

START 请求使用 `session_id=0`，payload 必须是完整 4 KiB 固件头。MCU 在擦除前检查槽位、地址、长度、算法、头 CRC、安全版本和 ECDSA 签名。成功后由请求序号、1 ms 时基、头 CRC、UID 和 BCB generation 派生非零 `session_id`。它是防止旧会话包串入的协议标识，不是密码学随机数，也不是身份认证令牌。

#### ERASE

ERASE 必须处于 `HEADER_ACCEPTED`，槽位和 session 必须匹配。MCU 先提交 `UPDATING`，再把 `4 KiB header + image_size` 向上按 2 KiB 扇区对齐擦除，然后写入固件头。先写 `UPDATING` 是掉电安全的关键：一旦开始改动镜像，目标槽就不再具备启动资格。

#### DATA

DATA payload 前 8 字节是 `offset` 和 `length`，后面是最多 4096 字节数据。MCU 只接受 `offset == next_offset` 的连续写入，检查容量和 32 位 Flash 编程对齐，每块写入后立即 `memcmp` Flash 回读内容，成功才增加 `next_offset`。

#### END

END 必须满足 `next_offset == image_size`。MCU 不信任 USB RX 缓冲区中曾经出现过的数据，而是直接对 Flash 中实际写入的主体分块计算 SHA-256，再比较签名头内 `image_hash`、重验清单签名和向量表。全部通过才把 BCB 改为 `VALID`。

#### JUMP_APP 与 RESET

`JUMP_APP` 不直接在 USB 处理路径中切换 MSP，而是先完整验证目标镜像、可选提交默认槽、发出成功响应，然后延时约 250 ms 系统复位。复位后复用正常启动验证路径，并获得比直接跳转更干净的硬件状态。`RESET` 不改变默认槽。

### 13. USB 下载会话状态机

USB 会话状态只保存在 RAM，复位后清空；BCB slot 状态保存在 Flash，复位后仍存在。两者不能混淆。

```text
DOWNLOAD_IDLE
      |
      | DOWNLOAD/START: signed header accepted
      v
DOWNLOAD_HEADER_ACCEPTED
      |
      | ERASE: BCB=UPDATING, slot erased, header programmed
      v
DOWNLOAD_READY <--------------------+
      |                              |
      | DOWNLOAD/DATA                | more DATA
      +------------------------------+
      |
      | DOWNLOAD/END: all final checks pass
      v
DOWNLOAD_COMPLETE
```

失败处理的实际语义：

- START 验头失败时尚未修改 Flash，不改 BCB。
- ERASE 前先写 `UPDATING`。如果擦除或写 4 KiB 头时失败，由于 RAM phase 还未进入 READY，持久化状态可能保持 `UPDATING`；它仍然是安全的不可启动状态。
- READY 后 Flash 编程、哈希、签名、向量表或 BCB 终提交失败时，尝试将目标槽写为 `INVALID`，会话回到 IDLE。
- offset 不匹配只返回 `BOOT_ERR_SEQUENCE`，当前会话保持 READY，允许主机发送正确的 `next_offset`。
- payload CRC 错误只拒绝该帧，不立即破坏下载会话。

当前 `handle_download_start()` 没有强制 phase 必须是 IDLE，因此一个合法的新 START 可以覆盖 RAM 中旧会话。这可以解释为“重新开始”，但在产品化协议中应将该语义明确化，或只允许 IDLE/COMPLETE 接受新 START。

### 14. 错误码与定位思路

| 状态码 | 名称 | 常见原因 |
|---:|---|---|
| 0 | `OK` | 命令成功 |
| -1 | `FRAME` | 预留帧错误；当前错帧头多通过逐字节重同步处理 |
| -2 | `CRC` | payload CRC 不匹配 |
| -3 | `COMMAND` | 未知命令 |
| -4 | `STATE` | phase、session 或命令次序不对 |
| -5 | `ARGUMENT` | payload 长度或 flags 错误 |
| -6 | `HEADER` | 头字段、CRC 或预验签失败 |
| -7 | `ROLLBACK` | security version 低于当前策略下限 |
| -8 | `FLASH` | 擦除、编程或立即回读失败 |
| -9 | `SEQUENCE` | DATA offset/长度/边界/对齐不满足 |
| -10 | `HASH` | Flash 主体 SHA-256 不匹配 |
| -11 | `SIGNATURE` | END 阶段清单验签失败 |
| -12 | `VECTOR` | MSP/Reset Handler 非法 |
| -13 | `BCB` | BCB 提交失败 |
| -14 | `SLOT` | 槽位越界、非 VALID 或镜像校验失败 |

### 15. Cortex-M4 App 跳转原理

App 向量表的第一个 32 位字是初始 MSP，第二个是带 Thumb bit 的 Reset Handler。跳转前检查：

- MSP 在 `0x20000000..0x20018000` 的 96 KiB SRAM 范围内，并且 8 字节对齐。
- Reset Handler 必须和签名头的 `entry_address` 一致。
- Reset Handler bit0 必须为 1，表示 Thumb 状态。
- Reset Handler 去掉 Thumb 语义后属于声明的 App 镜像范围。

`boot_jump_to_slot()` 按如下顺序清理 Bootloader 状态：

1. 关闭全局中断。
2. 停止并清空 SysTick。
3. 禁止所有 NVIC 外部中断并清 pending。
4. 清除 PendSV 和 SysTick 挂起。
5. `SCB->VTOR = app_vector`。
6. 恢复 `BASEPRI=0`、`FAULTMASK=0`、`CONTROL=0`。
7. 执行 DSB/ISB，然后恢复 `PRIMASK=0`。
8. 进入 naked 汇编跳板，`MSR MSP,r0` 后直接 `BX r1`。

切换 MSP 后不能继续执行普通 C 函数尾声，因为编译器可能用旧栈帧恢复寄存器；naked 汇编跳板避免了这个隐患。

### 16. App 独立链接与 GDB 调试

App A 和 App B 的绝对代码、向量表、中断入口和常量地址不同，因此需要两套链接产物，不能把 App A bin 放入 App B 后期望它直接执行。

常规 GDB `load` 只下载 App 主体时，会改变 `0x08021000` 或 `0x08091000` 中的代码，但 slot 头仍保留上一个包的 `image_hash/signature`。Bootloader 从 Flash 回读哈希时必然失败，所以不会跳入新调试的 App。这是安全校验在正常工作，不是 OpenOCD 额外擦除了相邻头部。

GDB 可以不下载代码直接调试 Flash 中现有 App。若为了单独调试 App A 而临时绕过 Bootloader 决策，可在 reset halt 后用：

```gdb
monitor reset halt
set *(unsigned int *)0xE000ED08 = 0x08021000
set $sp = *(unsigned int *)0x08021000
set $pc = (*(unsigned int *)0x08021004) & 0xfffffffe
```

App B 将向量地址替换为 `0x08091000`。这种方式只验证 App 本身，没有经过 Bootloader 安全链；要做完整联调，应将新 App 重新打包签名，同时更新 4 KiB 头和主体。

### 17. 掉电安全与不变量

本项目的主要不变量是：

> 只有 Flash 中的固件头、主体哈希、数字签名和向量表全部通过校验后，slot 才能被提交为 `VALID`；启动时还必须再验一次。

| 掉电位置 | Flash/BCB 结果 | 下次启动 |
|---|---|---|
| START 之前或刚验完头 | 目标 slot 未修改 | 原镜像仍可用 |
| BCB 已写 UPDATING，尚未擦除 | 目标 slot 不可启动 | 尝试另一 slot 或 USB |
| 擦除或 DATA 途中 | 不完整镜像 + UPDATING/INVALID | 不会跳转半成品 |
| END 校验失败 | INVALID | 尝试另一 slot 或 USB |
| BCB 新副本写入中 | 旧 BCB 副本仍完整 | 选有效旧副本 |
| VALID 已完整提交 | 新镜像具备启动资格 | 重验后启动 |

### 18. 超时、断线和幂等恢复设计

下列是已确定的产品化增强方向，**当前 MCU 代码尚未实现会话/残帧超时，PC 端虽设置了 libusb 超时，但超时后会直接退出，尚无状态查询和幂等重试**。

#### PC 端

- 区分 USB OUT 写超时、帧头等待超时、payload 超时和整个事务的绝对截止时间。
- 当前 START/ERASE/DATA/END 分别使用 30 s/120 s/10 s/30 s，普通命令 5 s。
- 过期响应可能在下一次事务到达，PC 应在总截止时间内丢弃不匹配的旧 sequence，而不是将第一个过期帧当作致命错误。
- 超时后不能无条件重发非幂等命令。

#### MCU 端

- RX 缓冲区中存在合法帧头但 payload 长时间未收齐时，例如 2~5 s 后清空残帧并重新寻找 `ATUP`。
- START、ERASE 或 DATA 成功后启动/刷新会话活动时间；例如 60 s 没有合法进展则中止会话。
- `HEADER_ACCEPTED` 超时只清 RAM；`READY` 超时将 slot 记为 INVALID 再清会话；`COMPLETE` 超时只清会话，保留 VALID。
- TinyUSB unmount/断开时需清 RX/TX 残留和会话，处理规则和会话超时一致。
- 计时采用 `(uint32_t)(now - started) >= timeout`，保持 32 位 tick 回绕安全；耗时 ERASE/END 完成后再刷新活动时间，避免操作本身被误判为空闲。

#### 丢响应时的关键歧义

```text
PC sends DATA(offset=4096)
MCU programs successfully and next_offset becomes 8192
USB IN response is lost
PC still believes next_offset is 4096
```

盲目重发会被当前严格 offset 检查拒绝。推荐后续同时实现：

- `GET_STATUS`：返回 phase、session、slot、`next_offset` 和 `last_error`。
- DATA 幂等重发：如果 `offset + length == next_offset`，回读 Flash；内容一致就返回当前成功偏移，不再编程。
- START/ERASE/END 的重复请求也要定义明确幂等语义，PC 再进行有界 2~3 次重试。

协议超时不能代替独立看门狗。若要防范 Flash 控制器或程序异常永久卡住，还应配置硬件 WDT，并在有界擦写/哈希循环内按设计喂狗。

### 19. 当前安全模型与已知边界

当前已经具备：

- 私钥不下发，MCU 仅保存受保护公钥。
- 签名绑定芯片类型、槽位、地址、长度、版本、算法和主体哈希。
- 擦除前先验签，擦除前先落盘 `UPDATING`。
- 每块 Flash 立即回读，END 阶段再对 Flash 完整哈希。
- 启动时不盲信 BCB，因此单纯伪造 `VALID` 不能让未签名代码执行。
- 所有来自 PC 的长度、偏移、槽位、地址和算法字段都在使用前检查。

需要正确说明的边界：

1. **防回滚下限不是不可逆计数器。** START 使用当前 BCB 中所有 `VALID` slot 的最高 `security_version`。如果高版本槽被失效化或 BCB 被重置，该下限可以降低。严格产品防回滚需要 OTP/eFuse/受保护单调计数器。
2. **session ID 不是认证。** USB 物理访问者可以获得并使用 session；固件授权依赖 ECDSA 签名。
3. **BCB 没有数字签名。** 篡改 BCB 不能绕过镜像验签，但可造成拒绝服务、默认槽改变，或影响当前软件防回滚下限。
4. **Bootloader 本体仍需要量产级保护策略。** 升级协议的 Flash API 不会写 Bootloader/sLib，但 SWD 或其他 Flash 通路的攻击面需要通过芯片读保护、写保护和调试口量产策略管理。
5. **当前没有断点续传、幂等重试和 MCU 会话超时。** 这影响长时升级和断线恢复可用性，但不会使半成品变成可启动镜像。
6. **CDC 是演示功能。** 它不应被描述为当前升级通道或日志通道。

### 20. 面试可以如何讲这个项目

一分钟概括：

> 我在 AT32F403A 上实现了一个 TinyUSB Vendor Bulk 安全 A/B Bootloader。每个 App 独立链接，slot 前有 4 KiB 签名头；PC 使用 ECDSA P-256 私钥签名包含 App SHA-256 的清单，MCU 仅从 sLib 受保护区读公钥验签。升级按 START、ERASE、DATA、END 状态机执行，擦除前先把 BCB 写成 UPDATING，最后从 Flash 回读哈希、再验签和验向量，通过后才写 VALID。BCB 用双页 generation 乒乓提交抗掉电，启动时每次重验默认 App，失败就回退到备用 App 或 USB 恢复模式。

常见追问：

**为什么有 CRC32 还要 SHA-256？**

CRC32 用于快速发现非恶意损坏和帧边界错误，SHA-256 提供密码学完整性，ECDSA 再为哈希和清单提供来源认证。

**为什么不把私钥放在 MCU？**

MCU 只需判断固件是否由产品方授权，公钥足够。私钥留在受控 PC/HSM 签名环境，可避免单台设备被提取后泄露整个产品签名权。

**为什么要从 Flash 回读 hash？**

对 USB 接收缓冲区计算 hash 只能证明“曾经收到过”正确数据，不能证明 Flash 擦写成功。从 Flash 回读才能验证将来实际执行的字节。

**为什么 BCB 要两份？**

Flash 不能原地原子更新，擦除后写入中途掉电会破坏唯一副本。乒乓提交保证写新副本时仍有一份可恢复的旧状态。

**为什么 BCB 是 VALID 还要每次验签？**

BCB 不是代码完整性证据，Flash 可能位翻转、被调试器改写或 BCB 本身被篡改。重验可确保每次真正要执行的字节仍属于受信固件。

**为什么 App A/B 要独立链接？**

常规 Cortex-M 固件含绝对地址和中断向量，本项目选择原地执行而非动态重定位，所以每个 slot 必须使用对应地址链接。

**为什么 JUMP_APP 先复位而不直接跳？**

复位能让 USB、时钟、中断和外设回到硬件初始状态，并复用唯一的安全启动路径。直接跳转仅保留给正常启动选择阶段。

**为什么 CDC 用 IAD？**

CDC ACM 功能由控制和数据两个接口组成，IAD 告诉主机这两个接口属于同一功能。Device 级 MISC/IAD 则使复合设备的功能分组更明确。

**如何证明方案抗掉电？**

要根据不变量逐个分析 START、BCB UPDATING、擦除、DATA、END、BCB 双副本提交中的掉电点，并用实机在每个点断电复测，而不是只说“有 A/B 所以安全”。

### 21. 测试矩阵和验收标准

建议把测试分为五组：

1. **基础构建和枚举**
   - Debug/Release 构建通过，map 中所有节都位于前 124 KiB。
   - Linux 无 sudo 可 claim Vendor interface 0。
   - `lsusb -v` 检查 MISC/IAD、接口数和五个非 EP0 端点。
2. **正常升级**
   - A/B 独立包都能 START→ERASE→DATA→END。
   - `GET_INFO` 的版本、大小、状态和 generation 与预期一致。
   - `--jump` 更改默认槽，复位后进入目标 App。
3. **安全反例**
   - 改一个清单字节后重算 CRC，ECDSA 仍必须拒绝。
   - 改一个 App 字节，END 阶段 SHA-256 必须拒绝。
   - 使用错私钥、错 slot、错 MCU、错链接地址和非法向量均必须拒绝。
   - 低 `security_version` 在当前软件策略下必须拒绝。
4. **状态机和掉电**
   - ERASE 前、擦除中、每个 DATA 边界、END 前和 BCB 提交中切断电源。
   - 恢复后目标 slot 不启动半成品，另一 VALID slot 能回退启动。
   - 单独损坏 BCB0 或 BCB1，设备仍能从另一份恢复。
5. **通信异常**
   - 错 magic、错帧头 CRC、错 payload CRC、超长 payload、分片到达和多帧粘包。
   - 错 session、乱序 offset、越界长度和最后非对齐块。
   - 在 DATA 已写但响应丢失、USB 断开、残帧和会话超时处验证恢复策略。

### 22. 项目现状与后续优先级

已完成并接入常规 Bootloader 构建：

- AT32F403A 的 CMake/OpenOCD/VS Code 编译、下载和调试链路。
- TinyUSB AT32F403A FSDEV 端口与 CDC ACM + Vendor 复合设备。
- Vendor Bulk 自定义协议、PC 打包工具和升级工具。
- 4 KiB 签名头、A/B 独立链接地址、Flash 回读哈希和向量检查。
- Mbed TLS 4.2 TF-PSA-Crypto 的 SHA-256 + ECDSA P-256 公钥验签子集。
- sLib 公钥记录读取；制造阶段 provision 测试源码保留，但不链入常规目标。
- BCB0/BCB1 generation 乒乓提交、A/B 启动回退和 PA0 强制恢复。

后续建议按优先级实现：

1. 会话超时、残帧超时、USB 断开清理、`GET_STATUS` 和幂等重试。
2. 硬件看门狗和掉电/断线自动化故障注入测试。
3. 将防回滚下限放入不可逆安全存储，并定义密钥轮换/撤销策略。
4. 根据量产威胁模型配置 Bootloader 写保护、读保护和 SWD 策略。
5. 根据实际产品需求再增加 DFU 或更多 Vendor Specific 功能，不在基础 Bootloader 中预先堆叠未使用功能。
