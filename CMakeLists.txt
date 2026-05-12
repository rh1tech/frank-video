cmake_minimum_required(VERSION 3.13)

# Target board: Pico 2 / RP2350
set(PICO_BOARD pico2 CACHE STRING "Pico board type")

# Board variant — selects pin layout (see src/board_config.h).
set(BOARD_VARIANT "M2" CACHE STRING "Board variant: M1 or M2")

# CPU clock target. 504 MHz is the design point for this build.
set(CPU_SPEED "504" CACHE STRING "CPU clock in MHz: 252, 378, 504")

# PSRAM target frequency (max safe).
set(PSRAM_SPEED "166" CACHE STRING "PSRAM max frequency in MHz")

# Flash target frequency (max safe).
set(FLASH_SPEED "66" CACHE STRING "Flash max frequency in MHz")

# Map clock target to voltage rail.
if(CPU_SPEED GREATER_EQUAL 504)
    set(CPU_VOLTAGE "VREG_VOLTAGE_1_65")
elseif(CPU_SPEED GREATER_EQUAL 378)
    set(CPU_VOLTAGE "VREG_VOLTAGE_1_60")
else()
    set(CPU_VOLTAGE "VREG_VOLTAGE_1_50")
endif()

message(STATUS "frank-video: Board=${BOARD_VARIANT} CPU=${CPU_SPEED} MHz "
        "PSRAM=${PSRAM_SPEED} MHz Flash=${FLASH_SPEED} MHz Voltage=${CPU_VOLTAGE}")

# Inject version.txt as a compile-time string.
file(READ "${CMAKE_CURRENT_LIST_DIR}/version.txt" FRANK_VERSION_RAW)
string(STRIP "${FRANK_VERSION_RAW}" FRANK_VERSION_RAW)
string(REGEX REPLACE "[ \t]+" "." FRANK_VERSION "${FRANK_VERSION_RAW}")

include(pico_sdk_import.cmake)

project(frank-video C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

# --- FatFS + SD + PS/2 driver subdirs --------------------------------------
add_subdirectory(src/fatfs)
add_subdirectory(drivers/sdcard)
add_subdirectory(drivers/ps2kbd)

# Drivers static lib: HDMI + PSRAM (no PIO header generation needed here).
add_library(drivers STATIC
    drivers/HDMI.c
    drivers/psram_init.c
    drivers/psram_allocator.c
)
target_include_directories(drivers PUBLIC drivers)
target_compile_definitions(drivers PRIVATE
    BOARD_${BOARD_VARIANT}
    PSRAM_MAX_FREQ_MHZ=${PSRAM_SPEED}
)
target_compile_options(drivers PRIVATE -Ofast)
target_link_libraries(drivers PUBLIC
    pico_stdlib hardware_dma hardware_pio hardware_spi hardware_clocks
    hardware_irq hardware_sync
)

# --- Main executable --------------------------------------------------------
add_executable(frank-video
    src/main.c
    src/file_browser.c
    src/player.c
    drivers/i2s/i2s_audio.c
)

# Generate the I2S PIO header against the executable so the audio_i2s.pio.h
# include resolves at compile time.
pico_generate_pio_header(frank-video
    ${CMAKE_CURRENT_LIST_DIR}/drivers/i2s/audio_i2s.pio
)

target_include_directories(frank-video PRIVATE
    src
    drivers
    drivers/i2s
    drivers/sdcard
    drivers/ps2kbd
)

target_compile_options(frank-video PRIVATE -Ofast)

target_compile_definitions(frank-video PRIVATE
    BOARD_${BOARD_VARIANT}
    CPU_CLOCK_MHZ=${CPU_SPEED}
    CPU_VOLTAGE=${CPU_VOLTAGE}
    PSRAM_MAX_FREQ_MHZ=${PSRAM_SPEED}
    FLASH_MAX_FREQ_MHZ=${FLASH_SPEED}
    FRANK_VERSION="${FRANK_VERSION}"
    PICO_MALLOC_PANIC=0
)

# Headless profiling: -DAUTOPLAY_FIRST=ON skips the file browser and loops
# the first .mpg found on the SD card. Useful when iterating on perf without
# a keyboard plugged in. Pair with -DAUTOPLAY_TARGET=NAME.MPG to pin a
# specific clip; the matcher is case-insensitive.
option(AUTOPLAY_FIRST "Skip file browser and auto-play the first MPG" OFF)
set(AUTOPLAY_TARGET "" CACHE STRING "When AUTOPLAY_FIRST is ON, prefer this filename")
if(AUTOPLAY_FIRST)
    target_compile_definitions(frank-video PRIVATE AUTOPLAY_FIRST=1)
    if(NOT AUTOPLAY_TARGET STREQUAL "")
        target_compile_definitions(frank-video PRIVATE AUTOPLAY_TARGET="${AUTOPLAY_TARGET}")
    endif()
endif()

# SD card pin macros based on board variant.
if(BOARD_VARIANT STREQUAL "M1")
    target_compile_definitions(frank-video PRIVATE
        SDCARD_PIN_SPI0_CS=5
        SDCARD_PIN_SPI0_SCK=2
        SDCARD_PIN_SPI0_MOSI=3
        SDCARD_PIN_SPI0_MISO=4
    )
elseif(BOARD_VARIANT STREQUAL "M2")
    target_compile_definitions(frank-video PRIVATE
        SDCARD_PIN_SPI0_CS=5
        SDCARD_PIN_SPI0_SCK=6
        SDCARD_PIN_SPI0_MOSI=7
        SDCARD_PIN_SPI0_MISO=4
    )
endif()

target_link_options(frank-video PRIVATE -Wl,-Map=frank-video.map -Wl,--gc-sections)

target_link_libraries(frank-video
    pico_stdlib
    pico_multicore
    hardware_clocks
    hardware_dma
    hardware_pio
    hardware_pwm
    hardware_irq
    hardware_interp
    hardware_exception
    hardware_watchdog
    sdcard
    ps2kbd
    fatfs
    drivers
)

# USB CDC stdio with 4-second connect delay handled in src/main.c.
pico_enable_stdio_usb(frank-video 1)
pico_enable_stdio_uart(frank-video 0)

pico_add_extra_outputs(frank-video)
