#include <string.h>

#include "tusb.h"

enum {
  ITF_NUM_CDC,
  ITF_NUM_CDC_DATA,
  ITF_NUM_TOTAL
};

enum {
  STRID_LANGID,
  STRID_MANUFACTURER,
  STRID_PRODUCT,
  STRID_SERIAL,
  STRID_CDC
};

#define USB_VID               0xCAFE
#define USB_PID               0x4001
#define USB_BCD               0x0100

#define EPNUM_CDC_NOTIF       0x81
#define EPNUM_CDC_OUT         0x02
#define EPNUM_CDC_IN          0x82

#define CONFIG_TOTAL_LEN      (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN)

static tusb_desc_device_t const desc_device = {
  .bLength = sizeof(tusb_desc_device_t),
  .bDescriptorType = TUSB_DESC_DEVICE,
  .bcdUSB = 0x0200,
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
  TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, STRID_CDC, EPNUM_CDC_NOTIF, 8,
                     EPNUM_CDC_OUT, EPNUM_CDC_IN, 64)
};

static char const *string_desc_arr[] = {
  (char const[]){0x09, 0x04},
  "Artery",
  "AT32F403A TinyUSB CDC",
  NULL,
  "TinyUSB CDC"
};

static uint16_t desc_string[32];

uint8_t const *tud_descriptor_device_cb(void)
{
  return (uint8_t const *)&desc_device;
}

uint8_t const *tud_descriptor_configuration_cb(uint8_t index)
{
  (void)index;
  return desc_configuration;
}

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
  (void)langid;
  size_t count;

  if (index == STRID_LANGID) {
    memcpy(&desc_string[1], string_desc_arr[0], 2);
    count = 1;
  } else if (index == STRID_SERIAL) {
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
