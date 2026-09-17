#include "usb_cdc.h"
#include "at32f403a_407_conf.h"
#include "tusb.h"

void usb_port_init(void);

bool usb_cdc_init(void) {
    /* CDC 演示与 Vendor Bootloader 共用同一个 AT32 FSDEV 底层端口初始化。 */
    usb_port_init();

    tusb_rhport_init_t const dev_init = {.role = TUSB_ROLE_DEVICE, .speed = TUSB_SPEED_FULL};

    return tusb_init(0, &dev_init);
}

void usb_cdc_task(void) {
    static bool was_connected;
    static char const ready_message[] = "TinyUSB CDC echo ready\r\n";
    uint8_t buffer[64];

    /* 裸机模式持续轮询 TinyUSB，并在首次连接时提示，然后原样回显收到的数据。 */
    tud_task();

    bool const connected = tud_cdc_connected();
    if (connected && !was_connected) {
        tud_cdc_write(ready_message, sizeof(ready_message) - 1u);
        tud_cdc_write_flush();
    }
    was_connected = connected;

    if (!connected || !tud_cdc_available()) {
        return;
    }

    uint32_t const count = tud_cdc_read(buffer, sizeof(buffer));
    if (count > 0u) {
        tud_cdc_write(buffer, count);
        tud_cdc_write_flush();
    }
}
