set(TINYUSB_ROOT "${CMAKE_SOURCE_DIR}/third_party/tinyusb")

# TinyUSB's helper adds the portable stack and class sources, but not the
# MCU-specific device controller driver.
include("${TINYUSB_ROOT}/src/CMakeLists.txt")
tinyusb_target_add(${CMAKE_PROJECT_NAME})

target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    ${TINYUSB_ROOT}/src/portable/st/stm32_fsdev/dcd_stm32_fsdev.c
    ${TINYUSB_ROOT}/src/portable/st/stm32_fsdev/fsdev_common.c
    ${CMAKE_SOURCE_DIR}/project/src/usb_port.c
    ${CMAKE_SOURCE_DIR}/project/src/usb_descriptors.c
)

target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}/project/src/usb_boot.c
    ${CMAKE_SOURCE_DIR}/project/src/boot_crc32.c
    ${CMAKE_SOURCE_DIR}/project/src/boot_flash.c
    ${CMAKE_SOURCE_DIR}/project/src/boot_security.c
    ${CMAKE_SOURCE_DIR}/project/src/boot_bcb.c
    ${CMAKE_SOURCE_DIR}/project/src/boot_manager.c
)

target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    CFG_TUSB_MCU=OPT_MCU_AT32F403A_407
)
