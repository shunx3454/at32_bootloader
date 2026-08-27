#ifndef USB_CDC_H_
#define USB_CDC_H_

#include <stdbool.h>

bool usb_cdc_init(void);
void usb_cdc_task(void);

#endif
