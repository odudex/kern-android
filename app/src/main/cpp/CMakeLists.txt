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
set(WALLY_DIR ${KERN_DIR}/components/libwally-core)

if(NOT EXISTS "${KERN_DIR}/simulator/CMakeLists.txt")
    message(FATAL_ERROR "Kern submodule missing. Expected ${KERN_DIR}")
endif()
if(NOT EXISTS "${LVGL_DIR}/src/lvgl.h")
    message(FATAL_ERROR "LVGL submodule missing. Expected ${LVGL_DIR}")
endif()
if(NOT EXISTS "${MBEDTLS_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "Mbed TLS source missing. Expected ${MBEDTLS_DIR}")
endif()

# ---------------------------------------------------------------------------
# mbedtls (vendored). Only libmbedcrypto is consumed; libmbedtls / libmbedx509
# are EXCLUDE_FROM_ALL and unreferenced so they never build.
#
# MBEDTLS_CONFIG_FILE points mbedtls at a hand-trimmed config header enabling
# only the primitives Kern actually calls (AES/GCM/MD/HMAC/PBKDF2/SHA/Base64).
# Mbed TLS's own CMake propagates this define PUBLIC to every TU, so the
# consumer side sees the same #defines when including mbedtls headers.
# ---------------------------------------------------------------------------
set(MBEDTLS_CONFIG_FILE "${CMAKE_CURRENT_SOURCE_DIR}/mbedtls_kern_config.h"
    CACHE FILEPATH "Custom Kern mbedtls config" FORCE)
set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
add_subdirectory(${MBEDTLS_DIR} ${CMAKE_BINARY_DIR}/mbedtls-build EXCLUDE_FROM_ALL)

# ---------------------------------------------------------------------------
# LVGL (static)
# ---------------------------------------------------------------------------
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

# ---------------------------------------------------------------------------
# libwally-core + bundled secp256k1 (amalgamation, single TU)
#
# TODO: split secp256k1 out of combined.c into its own static library so it
# can be cached independently and unit-tested. Requires either a wrapper .c
# that re-includes combined.c minus the three secp256k1 #includes, or an
# upstream-aware split that compiles wally's individual .c files. Keeping
# the amalgamation for now preserves wally's sha256/sha512/ripemd160 macro
# renames that depend on single-TU compilation.
# ---------------------------------------------------------------------------
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

# ---------------------------------------------------------------------------
# kern_common: INTERFACE library carrying the includes, defines, and flags
# that every Kern-side static library and the JNI shim need to share.
# ---------------------------------------------------------------------------
add_library(kern_common INTERFACE)
target_include_directories(kern_common INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${KERN_DIR}/main
    ${KERN_DIR}/components
    ${KERN_DIR}/components/bbqr/src
    ${KERN_DIR}/components/k_quirc/include
    ${KERN_DIR}/components/k_quirc/src
    ${KERN_DIR}/components/cUR/src
    ${KERN_DIR}/components/video
    ${KERN_DIR}/simulator/platform
    ${KERN_DIR}/simulator/platform/esp_idf_stubs/include
    ${KERN_DIR}/simulator/platform/bsp_sim/include
    ${KERN_DIR}/simulator/platform/video_sim/include
    ${KERN_DIR}/simulator/platform/sd_card_sim/include
)
target_compile_definitions(kern_common INTERFACE
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
target_compile_options(kern_common INTERFACE
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

# ---------------------------------------------------------------------------
# kern_sim_stubs: ESP-IDF / BSP / SD-card / video simulator shims.
# These implement APIs that real Kern firmware would get from ESP-IDF.
# ---------------------------------------------------------------------------
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
add_library(kern_sim_stubs STATIC ${KERN_SIM_SOURCES})
target_link_libraries(kern_sim_stubs PUBLIC kern_common lvgl wally)

# ---------------------------------------------------------------------------
# kern_core: Kern firmware application code + first-party components.
# Excludes main.c (we drive lifecycle from JNI), test/, and core/storage.c
# (replaced by simulator's storage_stub.c).
# ---------------------------------------------------------------------------
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

add_library(kern_core STATIC
    ${KERN_MAIN_SOURCES}
    ${KERN_COMPONENT_SOURCES}
    ${KQUIRC_SOURCES}
    ${CUR_SOURCES}
)
target_link_libraries(kern_core PUBLIC kern_common lvgl wally)

# ---------------------------------------------------------------------------
# kern_android: the actual JNI shared library packaged in the APK.
# Thin Android shim — keep this target small so its rebuild stays fast.
# ---------------------------------------------------------------------------
add_library(kern_android SHARED
    android_bridge.cpp
    android_bsp.c
    android_lvgl_display.cpp
    android_posix_compat.c
)
target_link_libraries(kern_android PRIVATE
    kern_core
    kern_sim_stubs
    android
    log
    m
)
# Note: mbedcrypto is pulled in transitively via wally -> kern_core. The TLS
# and X.509 libraries (mbedtls, mbedx509) are intentionally not linked: a
# symbol audit showed Kern only uses AES/GCM/Base64/MD/HMAC/PKCS5/SHA, all of
# which live in mbedcrypto. EXCLUDE_FROM_ALL on the mbedtls add_subdirectory
# means the unused libs are not built at all.
