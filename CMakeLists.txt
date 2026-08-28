cmake_minimum_required(VERSION 3.22)

#
# User is free to modify the file as much as necessary
#

# Setup compiler settings
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

# Building the sLib provisioner is deliberately opt-in because enabling sLib
# consumes a configuration slot and changes the MCU's flash protection state.
option(AT32_SLIB_PROVISION "Build the AT32 sLib provisioning test" OFF)
set(AT32_SLIB_PASSWORD "" CACHE STRING
    "32-bit sLib password (required only when AT32_SLIB_PROVISION=ON)")


# Define the build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug")
endif()

# Set the project name
set(CMAKE_PROJECT_NAME at32_usb_dfu)

# Include toolchain file
include("cmake/gcc-arm-none-eabi.cmake")

# Enable compile command to ease indexing with e.g. clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

# Core project settings
project(${CMAKE_PROJECT_NAME})
message("Build type: " ${CMAKE_BUILD_TYPE})

# Enable CMake support for ASM and C languages
enable_language(C ASM)

# Create an executable object type
add_executable(${CMAKE_PROJECT_NAME})

# Add at32_workbench generated sources
add_subdirectory(cmake/at32_workbench)

# Add TinyUSB device stack and the AT32F403A FSDEV port.
include("cmake/tinyusb.cmake")

# Add the embedded Mbed TLS 4.2 crypto backend (TF-PSA-Crypto).
include("cmake/mbedtls.cmake")

# Link directories setup
target_link_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined library search paths
)

# Add sources to executable
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user sources here
)

if(AT32_SLIB_PROVISION)
    if(NOT AT32_SLIB_PASSWORD MATCHES "^(0[xX][0-9A-Fa-f]+|[0-9]+)$")
        message(FATAL_ERROR
            "AT32_SLIB_PASSWORD must be a 32-bit hexadecimal or decimal value")
    endif()

    math(EXPR AT32_SLIB_PASSWORD_VALUE "${AT32_SLIB_PASSWORD}")
    if(AT32_SLIB_PASSWORD_VALUE EQUAL 0 OR
       AT32_SLIB_PASSWORD_VALUE GREATER 4294967294)
        message(FATAL_ERROR
            "AT32_SLIB_PASSWORD cannot be 0x00000000 or 0xFFFFFFFF")
    endif()

    target_sources(${CMAKE_PROJECT_NAME} PRIVATE
        ${CMAKE_SOURCE_DIR}/project/src/slib_provision.c
    )
    target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
        AT32_SLIB_PROVISION_ENABLED=1
        AT32_SLIB_PROVISION_PASSWORD=${AT32_SLIB_PASSWORD_VALUE}UL
    )
    target_link_options(${CMAKE_PROJECT_NAME} PRIVATE
        "LINKER:--defsym=__slib_provision_build__=1"
    )
endif()

# Add include paths
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined include paths
)

# Add project symbols (macros)
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined symbols
)

# Add linked libraries
target_link_libraries(${CMAKE_PROJECT_NAME}
    at32_workbench

    # Add user defined libraries
)
