#ifndef USB_BOOT_H
#define USB_BOOT_H

#include <stdbool.h>

/* 初始化 TinyUSB Vendor 设备及一次升级会话的内存状态。 */
bool usb_boot_init(void);

/* 裸机轮询任务：收发协议帧、推进下载状态机，并处理延迟复位。 */
void usb_boot_task(void);

#endif
