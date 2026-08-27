#include "at32f403a_407_conf.h"
#include "tusb.h"
#include "wk_system.h"

void usb_port_init(void)
{
  // Use the dedicated USB IRQ vectors instead of the USB/CAN1 shared vectors.
  crm_usb_interrupt_remapping_set(CRM_USB_INT73_INT74);

  // The CPU runs at 240 MHz, which cannot be divided to 48 MHz by the USB PLL
  // divider. Use the calibrated 48 MHz HICK clock as the USB source instead.
  crm_usb_clock_source_select(CRM_USB_CLOCK_SOURCE_HICK);
  crm_periph_clock_enable(CRM_ACC_PERIPH_CLOCK, TRUE);
  acc_write_c1(7980);
  acc_write_c2(8000);
  acc_write_c3(8020);
  acc_calibration_mode_enable(ACC_CAL_HICKTRIM, TRUE);

  crm_periph_clock_enable(CRM_USB_PERIPH_CLOCK, TRUE);

  uint32_t const priority = NVIC_EncodePriority(NVIC_GetPriorityGrouping(), 1, 0);
  NVIC_SetPriority(USBFS_MAPH_IRQn, priority);
  NVIC_SetPriority(USBFS_MAPL_IRQn, priority);
  NVIC_SetPriority(USBFSWakeUp_IRQn, priority);
}

uint32_t tusb_time_millis_api(void)
{
  return wk_timebase_get();
}

void USBFS_MAPH_IRQHandler(void)
{
  tud_int_handler(0);
}

void USBFS_MAPL_IRQHandler(void)
{
  tud_int_handler(0);
}

void USBFSWakeUp_IRQHandler(void)
{
  tud_int_handler(0);
}
