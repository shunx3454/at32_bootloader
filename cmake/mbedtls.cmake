set(MBEDTLS_ROOT "${CMAKE_SOURCE_DIR}/third_party/mbedtls")
set(TF_PSA_CRYPTO_ROOT "${MBEDTLS_ROOT}/tf-psa-crypto")

# Only build the static cryptographic library. TLS, X.509, host programs and
# upstream test suites are intentionally outside the initial embedded port.
set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(GEN_FILES OFF CACHE BOOL "" FORCE)
set(DISABLE_PACKAGE_CONFIG_AND_INSTALL ON CACHE BOOL "" FORCE)
set(USE_STATIC_TF_PSA_CRYPTO_LIBRARY ON CACHE BOOL "" FORCE)
set(USE_SHARED_TF_PSA_CRYPTO_LIBRARY OFF CACHE BOOL "" FORCE)
set(TF_PSA_CRYPTO_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
set(INSTALL_TF_PSA_CRYPTO_HEADERS OFF CACHE BOOL "" FORCE)

set(TF_PSA_CRYPTO_CONFIG_FILE
    "${CMAKE_SOURCE_DIR}/project/inc/mbedtls_crypto_config.h"
    CACHE FILEPATH "" FORCE
)

add_subdirectory(
    "${TF_PSA_CRYPTO_ROOT}"
    "${CMAKE_BINARY_DIR}/third_party/tf-psa-crypto"
)

# The upstream build compiles guarded source files for disabled public-key
# modules; with this symmetric-only profile some of their types are zero-sized.
# GCC accepts that extension, so suppress the resulting upstream-only warning.
foreach(mbedtls_target builtin everest p256-m pqcp extras platform utilities tfpsacrypto)
    if(TARGET ${mbedtls_target})
        target_compile_options(${mbedtls_target} PRIVATE -Wno-pedantic)
    endif()
endforeach()

target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}/project/src/crypto_platform.c
)

target_link_libraries(${CMAKE_PROJECT_NAME} tfpsacrypto)
