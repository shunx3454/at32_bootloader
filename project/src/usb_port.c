#include "at32f403a_407_conf.h"
#include "tusb.h"
#include "wk_system.h"

void usb_port_init(void)
{
  /* 将 USB 中断映射到专用向量，避免与 CAN1 共用中断入口。 */
  crm_usb_interrupt_remapping_set(CRM_USB_INT73_INT74);

  /*
   * CPU 运行在 240 MHz，USB PLL 分频器无法从该频率得到 48 MHz，因此改用
   * HICK 作为 USB 时钟，并由 ACC 根据 USB SOF 对 HICK 做自动校准。
   * 不仅打开 USBFS 外设时钟，同时使 USBFS 硬件自动接管 PA11/PA12。
   * GPIO 的模式、上下拉、输出类型等配置对 USB_DM/DP 不再起普通 GPIO 作用
   */
  crm_usb_clock_source_select(CRM_USB_CLOCK_SOURCE_HICK);
  crm_periph_clock_enable(CRM_ACC_PERIPH_CLOCK, TRUE);
  acc_write_c1(7980);
  acc_write_c2(8000);
  acc_write_c3(8020);
  acc_calibration_mode_enable(ACC_CAL_HICKTRIM, TRUE);

  crm_periph_clock_enable(CRM_USB_PERIPH_CLOCK, TRUE);

  /* TinyUSB 使用映射后的高/低优先级及唤醒中断，统一配置相同抢占优先级。 */
  uint32_t const priority = NVIC_EncodePriority(NVIC_GetPriorityGrouping(), 1, 0);
  NVIC_SetPriority(USBFS_MAPH_IRQn, priority);
  NVIC_SetPriority(USBFS_MAPL_IRQn, priority);
  NVIC_SetPriority(USBFSWakeUp_IRQn, priority);
}

uint32_t tusb_time_millis_api(void)
{
  /* TinyUSB 裸机端口使用项目的 1 ms SysTick 作为协议超时时基。 */
  return wk_timebase_get();
}

void USBFS_MAPH_IRQHandler(void)
{
  /* 三个 USBFS 中断入口统一交给 TinyUSB 的根端口 0 处理。 */
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
