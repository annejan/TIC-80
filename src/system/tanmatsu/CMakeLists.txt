# ESP-IDF component wrapping TIC-80 for the Tanmatsu.
#
# TIC-80's own CMake is not used here: ESP-IDF drives the build, so the source
# lists come from cmake/tanmatsu.cmake and everything lands in one component.

get_filename_component(TIC80_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)

include(${TIC80_ROOT}/cmake/tanmatsu.cmake)

set(TIC80_GENERATED_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${TIC80_GENERATED_DIR})
tic80_tanmatsu_generate_headers(${TIC80_GENERATED_DIR})

idf_component_register(
    SRCS
        ${TIC80_TANMATSU_SRCS}
    INCLUDE_DIRS
        ${TIC80_TANMATSU_INCLUDE_DIRS}
        ${TIC80_GENERATED_DIR}
    LDFRAGMENTS
        "linker.lf"
    PRIV_REQUIRES
        badge-bsp
        esp_lcd
        esp_timer
        esp_driver_i2s
        esp_driver_gpio
        esp_driver_sdmmc
        esp_mm
        esp_driver_ppa
        sdmmc
        fatfs
        wear_levelling
        nvs_flash
        vfs
)

target_compile_definitions(${COMPONENT_LIB} PUBLIC ${TIC80_TANMATSU_DEFINITIONS})

# -DTIC80_PROFILE=ON logs where each frame goes, every couple of seconds.
option(TIC80_PROFILE "Log per-frame timings" OFF)
if(TIC80_PROFILE)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE TIC80_TANMATSU_PROFILE=1)
endif()

# TIC-80 and its vendored libraries are older and noisier than the ESP-IDF
# default warning set; none of these are actionable from this port.
target_compile_options(${COMPONENT_LIB} PRIVATE
    # TIC-80 predates C23's reading of "()" as "(void)": include/tic80.h
    # declares tic80_tick with unspecified arguments while tic.c defines it
    # with the real ones, which only agree under the older standard.
    -std=gnu17
    # Warnings in code this port does not own must not stop the build.
    -Wno-error
    -Wno-error=implicit-function-declaration
    -Wno-unused-variable
    -Wno-unused-but-set-variable
    -Wno-unused-const-variable
    -Wno-unused-function
    -Wno-sign-compare
    -Wno-type-limits
    -Wno-stringop-truncation
    -Wno-char-subscripts
    -Wno-missing-field-initializers
)

# ESP-IDF's newlib has no lstat, and symlinks do not exist on FATFS anyway.
set_source_files_properties(
    ${TIC80_VENDOR}/zip/src/zip.c
    PROPERTIES COMPILE_DEFINITIONS "lstat=stat"
)
