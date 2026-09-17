tud_vendor_* 是 TinyUSB Vendor Class 的应用层接口。它们操作的是 TinyUSB 的软件 FIFO，不是直接同步读写 USB 寄存器。

  先明确方向：

  主机 → MCU：USB OUT端点 0x01 → Vendor RX FIFO
  MCU → 主机：USB IN 端点 0x81 ← Vendor TX FIFO

  本项目配置为：

  #define CFG_TUD_VENDOR_RX_BUFSIZE 512
  #define CFG_TUD_VENDOR_TX_BUFSIZE 512
  #define CFG_TUD_VENDOR_RX_EPSIZE   64
  #define CFG_TUD_VENDOR_TX_EPSIZE   64

  也就是：

  - USB 单包最多 64 字节。
  - RX/TX 软件 FIFO 各 512 字节。
  - 一次 4096 字节升级块会被拆成很多个 64 字节 USB 包。

  ### 常用函数

   函数                            作用
  ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━  ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
   tud_vendor_mounted()            Vendor 接口是否已经被主机配置并打开
  ──────────────────────────────  ────────────────────────────────────────────────
   tud_vendor_available()          RX FIFO 中当前可读取的字节数
  ──────────────────────────────  ────────────────────────────────────────────────
   tud_vendor_read()               从 RX FIFO 取走数据
  ──────────────────────────────  ────────────────────────────────────────────────
   tud_vendor_peek()               查看 RX FIFO 的下一个字节，但不取走
  ──────────────────────────────  ────────────────────────────────────────────────
   tud_vendor_read_flush()         丢弃 RX FIFO 中所有未读数据
  ──────────────────────────────  ────────────────────────────────────────────────
   tud_vendor_write()              把数据复制到 TX FIFO
  ──────────────────────────────  ────────────────────────────────────────────────
   tud_vendor_write_available()    查询 TX FIFO 还能接收多少字节
  ──────────────────────────────  ────────────────────────────────────────────────
   tud_vendor_write_flush()        强制把 TX FIFO 中不足一个 USB 包的数据提交发送
  ──────────────────────────────  ────────────────────────────────────────────────
   tud_vendor_write_clear()        丢弃 TX FIFO 中尚未发送的数据
  ──────────────────────────────  ────────────────────────────────────────────────
   tud_vendor_write_str()          写入以 \0 结尾的字符串

  定义位于 third_party/tinyusb/src/class/vendor/vendor_device.h:83。

  ### tud_vendor_available()

  uint32_t count = tud_vendor_available();

  返回主机已经通过 OUT 0x01 发到 MCU、并且当前存放在 RX FIFO 中的字节数。

  它返回的不是：

  - RX FIFO 总容量；
  - 主机本次准备发送的总长度；
  - 一个完整协议帧的长度。

  例如主机发送了一个 4096 字节块，MCU某一时刻可能只看到：

  available() = 64

  下一次调用 tud_task() 后可能变成更多。

  本项目中：

  if (tud_vendor_available() != 0u) {
      uint32_t count = tud_vendor_read(...);
  }

  采用流式接收，然后由 parse_rx_stream() 判断是否已经收到完整协议帧。

  ### tud_vendor_read()

  uint32_t count = tud_vendor_read(buffer, buffer_size);

  从 RX FIFO 最多读取 buffer_size 字节，返回实际读取数量。

  例如：

  uint8_t buffer[128];

  uint32_t count = tud_vendor_read(buffer, sizeof(buffer));

  如果 FIFO 中只有 37 字节，就返回 37。

  读取成功后，这些字节会从 FIFO 中删除。随着 FIFO 腾出空间，TinyUSB 会继续安排 OUT 端点接收。

  本项目使用：

  uint32_t const count =
      tud_vendor_read(rx_frame + rx_length,
                      sizeof(rx_frame) - rx_length);

  rx_length += count;
  parse_rx_stream();

  也就是把收到的数据累积到协议帧缓存。

  ### tud_vendor_write()

  uint32_t written = tud_vendor_write(data, length);

  把数据复制到 Vendor TX FIFO，返回实际接受的字节数。

  重要的是：返回成功只表示数据进入 TinyUSB FIFO，不表示主机已经收到。

  例如 TX FIFO 只剩 100 字节空间：

  written = tud_vendor_write(data, 200);

  可能只返回：

  written = 100

  调用者必须保存剩余偏移，下一轮继续写。本项目正是这样处理：

  uint32_t const written =
      tud_vendor_write(tx_frame + tx_offset,
                       tx_length - tx_offset);

  tx_offset += written;

  这里：

  - tx_length：完整响应长度；
  - tx_offset：已经被 TinyUSB TX FIFO 接受的长度；
  - tx_length - tx_offset：尚未进入 FIFO 的长度。

  ### tud_vendor_write_flush()

  tud_vendor_write_flush();

  强制将 TX FIFO 中的数据提交给 IN 端点。

  正常情况下，当 FIFO 中积累到一个完整端点包，例如 64 字节时，TinyUSB可以自动启动传输。但协议响应最后可能只剩：

  17 字节

  如果不调用 flush()，这17字节可能继续留在 FIFO，等待后续数据凑包。

  因此本项目每次写入后都调用：

  (void)tud_vendor_write_flush();

  这表示：“即使不足64字节，也立即安排发送。”

  它仍然是异步的。函数返回时，数据可能只是进入端点发送队列，还没有真正到达主机。

  ### tud_vendor_write_available()

  uint32_t free_size = tud_vendor_write_available();

  返回 TX FIFO 当前剩余空间。

  可以先判断：

  if (tud_vendor_write_available() >= response_length) {
      tud_vendor_write(response, response_length);
      tud_vendor_write_flush();
  }

  本项目没有预先检查，而是根据 tud_vendor_write() 的实际返回值推进 tx_offset，这种写法同样正确，而且支持响应大于当前 FIFO 空间。

  ### tud_vendor_read_flush()

  这个名字容易误解。它不是“把接收数据处理掉”，而是直接清空未读 RX FIFO：

  tud_vendor_read_flush();

  适合：

  - 协议严重失步；
  - 放弃当前会话；
  - 主机重新连接；
  - 需要丢弃所有旧数据。

  本项目的解析器采用逐字节寻找魔数的方式重新同步，因此正常情况下不需要清空整个 FIFO。

  ### tud_vendor_mounted() 与 tud_mounted()

  本项目当前使用：

  tud_mounted()

  含义是整个 USB Device 已收到主机的 SET_CONFIGURATION。

  而：

  tud_vendor_mounted()

  特指 Vendor 类实例的端点已经打开。

  在当前固定包含 Vendor 接口的复合描述符中，两者效果基本一致。语义上写成：

  if (tx_length != 0u && tud_vendor_mounted()) {

  会更明确。

  ### _n 版本

  所有函数都有多实例版本：

  tud_vendor_n_read(index, buffer, size);
  tud_vendor_n_write(index, buffer, size);

  当前配置：

  #define CFG_TUD_VENDOR 1

  所以普通版本实际上是实例0的内联封装：

  tud_vendor_read(buffer, size);
  // 等价于
  tud_vendor_n_read(0, buffer, size);

  这里的 index=0 是“第0个 Vendor 类实例”，不一定等于 USB 的 bInterfaceNumber=0，本项目中两者恰好都是0。

  ### 和 tud_task() 的关系

  这些函数依赖持续调用：

  tud_task();

  总体流程是：

  USB中断收到数据
      ↓
  tud_task() 处理传输完成事件
      ↓
  数据进入 Vendor RX FIFO
      ↓
  tud_vendor_available/read() 取出

  tud_vendor_write() 写入 TX FIFO
      ↓
  tud_vendor_write_flush() 安排 IN 传输
      ↓
  USB中断发送数据
      ↓
  tud_task() 处理传输完成事件

  因此 tud_vendor_read/write 是非阻塞、FIFO式、异步接口，不能理解成串口那种调用一次就等待传输完成。
