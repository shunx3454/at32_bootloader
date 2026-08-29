#ifndef SLIB_PROVISION_H
#define SLIB_PROVISION_H

/*
 * 运行需要串口口令二次确认的 sLib 配置程序。
 * 只有启用 CMake 选项 AT32_SLIB_PROVISION 时才会链接该入口；此操作会改变
 * Flash 硬件保护状态，量产 Bootloader 构建不应调用它。
 */
void slib_provision_test_run(void);

#endif
