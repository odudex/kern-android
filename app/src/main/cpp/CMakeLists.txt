cmake_minimum_required(VERSION 3.22.1)
project(kern_android C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT DEFINED SIM_LCD_H_RES)
    set(SIM_LCD_H_RES 720)
endif()
if(NOT DEFINED SIM_LCD_V_RES)
    set(SIM_LCD_V_RES 1280)
endif()
if(NOT DEFINED SIM_BOARD)
    set(SIM_BOARD "wave_5")
endif()

set(ROOT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../..)
set(KERN_DIR ${ROOT_DIR}/third_party/Kern)
set(LVGL_DIR ${ROOT_DIR}/third_party/lvgl)
set(MBEDTLS_DIR ${ROOT_DIR}/third_party/mbedtls)

if(NOT EXISTS "${KERN_DIR}/simulator/CMakeLists.txt")
    message(FATAL_ERROR "Kern submodule missing. Expected ${KERN_DIR}")
endif()
if(NOT EXISTS "${LVGL_DIR}/src/lvgl.h")
    message(FATAL_ERROR "LVGL submodule missing. Expected ${LVGL_DIR}")
endif()
if(NOT EXISTS "${MBEDTLS_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "Mbed TLS source missing. Expected ${MBEDTLS_DIR}")
endif()

set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
add_subdirectory(${MBEDTLS_DIR} ${CMAKE_BINARY_DIR}/mbedtls-build EXCLUDE_FROM_ALL)

file(GLOB_RECURSE LVGL_SOURCES CONFIGURE_DEPENDS ${LVGL_DIR}/src/*.c)
add_library(lvgl STATIC ${LVGL_SOURCES})
target_include_directories(lvgl PUBLIC
    ${LVGL_DIR}
    ${LVGL_DIR}/src
)
target_include_directories(lvgl PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(lvgl PRIVATE -include lvgl_compat.h)
target_compile_definitions(lvgl PRIVATE
    LV_CONF_INCLUDE_SIMPLE
    lv_refr_now=kern_lv_refr_now_real
)

set(WALLY_DIR ${KERN_DIR}/components/libwally-core)
add_library(wally STATIC ${WALLY_DIR}/upstream/src/amalgamation/combined.c)
target_include_directories(wally PUBLIC
    ${WALLY_DIR}/upstream/include
)
target_include_directories(wally PRIVATE
    ${WALLY_DIR}
    ${WALLY_DIR}/upstream
    ${WALLY_DIR}/upstream/src
    ${WALLY_DIR}/upstream/src/ccan
    ${WALLY_DIR}/upstream/src/secp256k1
    ${WALLY_DIR}/upstream/src/secp256k1/src
    ${WALLY_DIR}/upstream/src/secp256k1/include
    ${WALLY_DIR}/upstream/src/secp256k1/src/modules/ecdh
    ${WALLY_DIR}/upstream/src/secp256k1/src/modules/ecdsa_adaptor
    ${WALLY_DIR}/upstream/src/secp256k1/src/modules/ecdsa_s2c
    ${WALLY_DIR}/upstream/src/secp256k1/src/modules/extrakeys
    ${WALLY_DIR}/upstream/src/secp256k1/src/modules/generator
    ${WALLY_DIR}/upstream/src/secp256k1/src/modules/musig
    ${WALLY_DIR}/upstream/src/secp256k1/src/modules/rangeproof
    ${WALLY_DIR}/upstream/src/secp256k1/src/modules/recovery
    ${WALLY_DIR}/upstream/src/secp256k1/src/modules/schnorrsig
    ${WALLY_DIR}/upstream/src/secp256k1/src/modules/surjection
    ${WALLY_DIR}/upstream/src/secp256k1/src/modules/whitelist
    ${MBEDTLS_DIR}/include
)
target_compile_definitions(wally PRIVATE
    BUILD_ELEMENTS=0
    BUILD_MINIMAL=1
    ECMULT_WINDOW_SIZE=8
    ENABLE_MODULE_ECDH=1
    ENABLE_MODULE_ECDSA_S2C=1
    ENABLE_MODULE_EXTRAKEYS=1
    ENABLE_MODULE_GENERATOR=1
    ENABLE_MODULE_RANGEPROOF=1
    ENABLE_MODULE_RECOVERY=1
    ENABLE_MODULE_SCHNORRSIG=1
    ENABLE_MODULE_SURJECTIONPROOF=1
    ENABLE_MODULE_WHITELIST=1
    HAVE_BUILTIN_POPCOUNT=1
    NDEBUG=1
)
target_compile_options(wally PRIVATE -w -fstack-protector-strong)
target_link_libraries(wally PUBLIC mbedcrypto)

file(GLOB_RECURSE KERN_MAIN_SOURCES CONFIGURE_DEPENDS ${KERN_DIR}/main/*.c)
list(FILTER KERN_MAIN_SOURCES EXCLUDE REGEX ".*/main\\.c$")
list(FILTER KERN_MAIN_SOURCES EXCLUDE REGEX ".*/test/.*")
list(FILTER KERN_MAIN_SOURCES EXCLUDE REGEX ".*/core/storage\\.c$")

file(GLOB KQUIRC_SOURCES CONFIGURE_DEPENDS ${KERN_DIR}/components/k_quirc/src/*.c)
file(GLOB_RECURSE CUR_SOURCES CONFIGURE_DEPENDS ${KERN_DIR}/components/cUR/src/*.c)

set(KERN_COMPONENT_SOURCES
    ${KERN_DIR}/components/bbqr/src/bbqr.c
    ${KERN_DIR}/components/bbqr/src/base32.c
    ${KERN_DIR}/components/bbqr/src/miniz.c
)

set(KERN_SIM_SOURCES
    ${KERN_DIR}/simulator/platform/esp_idf_stubs/stubs.c
    ${KERN_DIR}/simulator/platform/esp_idf_stubs/nvs_sim.c
    ${KERN_DIR}/simulator/platform/esp_idf_stubs/efuse_hmac_sim.c
    ${KERN_DIR}/simulator/platform/esp_idf_stubs/freertos_sim.c
    ${KERN_DIR}/simulator/platform/esp_idf_stubs/storage_stub.c
    ${KERN_DIR}/simulator/platform/sd_card_sim/sd_card_sim.c
    ${KERN_DIR}/simulator/platform/video_sim/video_sim.c
    ${KERN_DIR}/simulator/platform/video_sim/stb_image_impl.c
    ${KERN_DIR}/simulator/platform/bsp_sim/pmic_sim.c
)

add_library(kern_android SHARED
    android_bridge.cpp
    android_bsp.c
    android_lvgl_display.cpp
    android_posix_compat.c
    ${KERN_MAIN_SOURCES}
    ${KERN_COMPONENT_SOURCES}
    ${KERN_SIM_SOURCES}
    ${KQUIRC_SOURCES}
    ${CUR_SOURCES}
)

target_include_directories(kern_android PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${KERN_DIR}/simulator/platform/esp_idf_stubs/include
    ${KERN_DIR}/simulator/platform/bsp_sim/include
    ${KERN_DIR}/simulator/platform/video_sim/include
    ${KERN_DIR}/simulator/platform/sd_card_sim/include
    ${KERN_DIR}/simulator/platform
    ${KERN_DIR}/main
    ${KERN_DIR}/components
    ${KERN_DIR}/components/bbqr/src
    ${KERN_DIR}/components/k_quirc/include
    ${KERN_DIR}/components/k_quirc/src
    ${KERN_DIR}/components/cUR/src
    ${KERN_DIR}/components/video
    ${LVGL_DIR}
    ${LVGL_DIR}/src
    ${MBEDTLS_DIR}/include
)

target_compile_definitions(kern_android PRIVATE
    SIMULATOR=1
    LV_CONF_INCLUDE_SIMPLE
    SIM_LCD_H_RES=${SIM_LCD_H_RES}
    SIM_LCD_V_RES=${SIM_LCD_V_RES}
    K_QUIRC_ADAPTIVE_THRESHOLD
    K_QUIRC_BILINEAR_THRESHOLD
    $<$<STREQUAL:${SIM_BOARD},wave_4b>:CONFIG_KERN_BOARD_WAVE_4B=1>
    $<$<STREQUAL:${SIM_BOARD},wave_35>:CONFIG_KERN_BOARD_WAVE_35=1>
    $<$<STREQUAL:${SIM_BOARD},wave_5>:CONFIG_KERN_BOARD_WAVE_5=1>
    $<$<STREQUAL:${SIM_BOARD},wave_43>:CONFIG_KERN_BOARD_WAVE_43=1>
)
target_compile_options(kern_android PRIVATE
    -Wall
    -Wextra
    -Wno-unused-parameter
    -Wno-unused-function
    -Wno-sign-compare
    -Wno-missing-field-initializers
    -Wno-type-limits
    -include android_force_include.h
    -fstack-protector-strong
)

target_link_libraries(kern_android PRIVATE
    lvgl
    wally
    mbedtls
    mbedcrypto
    mbedx509
    android
    log
    m
)
