#ifndef USB_CDC_H_
#define USB_CDC_H_

#include <stdbool.h>

/* 独立 CDC 回显示例接口；复合 Bootloader 仅枚举 CDC，不调用这两个函数。 */
bool usb_cdc_init(void);
void usb_cdc_task(void);

#endif
