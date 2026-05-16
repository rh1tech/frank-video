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

# Inject version.txt as a compile-time string. version.txt is "MAJOR MINOR"
# (space separated); minor is zero-padded to two digits, e.g. "1 0" -> "1.00",
# "1 12" -> "1.12". Matches the frank-fodder release versioning.
file(READ "${CMAKE_CURRENT_LIST_DIR}/version.txt" FRANK_VERSION_RAW)
string(STRIP "${FRANK_VERSION_RAW}" FRANK_VERSION_RAW)
string(REGEX REPLACE "[ \t]+" ";" FRANK_VERSION_PARTS "${FRANK_VERSION_RAW}")
list(GET FRANK_VERSION_PARTS 0 FRANK_VERSION_MAJOR)
list(GET FRANK_VERSION_PARTS 1 FRANK_VERSION_MINOR)
if(FRANK_VERSION_MINOR LESS 10)
    set(FRANK_VERSION "${FRANK_VERSION_MAJOR}.0${FRANK_VERSION_MINOR}")
else()
    set(FRANK_VERSION "${FRANK_VERSION_MAJOR}.${FRANK_VERSION_MINOR}")
endif()

include(pico_sdk_import.cmake)

project(frank-video C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

# --- USB HID host (optional, swaps native USB from CDC->Host) --------------
#
# USB_HID_ENABLED=ON brings up TinyUSB Host on the native USB controller so
# a USB keyboard works alongside the PS/2 one. The native controller can't
# be both Host and Device, so CDC stdio is disabled in that build (debug
# output goes to UART instead). Default OFF keeps the USB serial console.
option(USB_HID_ENABLED "Enable USB HID host (disables USB CDC stdio)" OFF)

# --- FatFS + SD + PS/2 + (optional) USB HID driver subdirs -----------------
add_subdirectory(src/fatfs)
add_subdirectory(drivers/sdcard)
add_subdirectory(drivers/ps2kbd)
add_subdirectory(drivers/usbhid)

# --- Video output selection ------------------------------------------------
#
# HDMI_HSTX=ON   -> RP2350 native HSTX driver (DMA-fed scanline + HDMI audio
#                   over Data Islands). Requires GPIO 12-19 for the TMDS
#                   lanes, so M1 (HDMI base 6) is not supported.
# default        -> legacy PIO HDMI driver (drivers/HDMI.c) + I2S audio.
option(HDMI_HSTX "Use RP2350 HSTX HDMI driver with HDMI audio" OFF)

if(HDMI_HSTX AND NOT BOARD_VARIANT STREQUAL "M2")
    message(FATAL_ERROR
        "HDMI_HSTX requires BOARD_VARIANT=M2 (HSTX uses GPIO 12-19); "
        "BOARD_VARIANT=${BOARD_VARIANT} has its HDMI base on a non-HSTX pin set.")
endif()

if(HDMI_HSTX)
    # HSTX needs an integer divisor from clk_sys to its 126 MHz pixel domain.
    # 504 / 4 = 126, 252 / 2 = 126. CMake passes the active CPU clock in
    # CPU_SPEED so we pick MODE_HSTX_CLK_DIV accordingly.
    if(CPU_SPEED EQUAL 504)
        set(HSTX_CLK_DIV 4)
    elseif(CPU_SPEED EQUAL 378)
        set(HSTX_CLK_DIV 3)
    elseif(CPU_SPEED EQUAL 252)
        set(HSTX_CLK_DIV 2)
    elseif(CPU_SPEED EQUAL 126)
        set(HSTX_CLK_DIV 1)
    else()
        message(FATAL_ERROR
            "HDMI_HSTX needs CPU_SPEED such that CPU_SPEED/N == 126 MHz "
            "(126/252/378/504); got ${CPU_SPEED}.")
    endif()
    message(STATUS "HDMI_HSTX: clk_sys=${CPU_SPEED} MHz, MODE_HSTX_CLK_DIV=${HSTX_CLK_DIV}")
endif()

# Drivers static lib: HDMI (PIO or HSTX) + PSRAM.
if(HDMI_HSTX)
    add_library(drivers STATIC
        drivers/hdmi_hstx/HDMI_hstx.c
        drivers/hdmi_hstx/HDMI_hstx_audio.c
        drivers/hdmi_hstx/video_output.c
        drivers/hdmi_hstx/hstx_packet.c
        drivers/hdmi_hstx/hstx_data_island_queue.c
        drivers/psram_init.c
        drivers/psram_allocator.c
    )
    target_include_directories(drivers PUBLIC
        drivers
        drivers/hdmi_hstx
        drivers/i2s    # for i2s_audio.h header (signature compat shim)
    )
    # 640x480 @ 60Hz (HDMI VIC=1) is the most compatible mode -- TVs and
    # capture cards accept it without any quirky-mode handshaking. The
    # 320x240 framebuffer is line-doubled inside the scanline callback so
    # each source pixel fills a 2x2 dst block.
    target_compile_definitions(drivers PUBLIC
        HDMI_HSTX=1
        MODE_HSTX_CLK_DIV=${HSTX_CLK_DIV}
    )
    # M2 PCB layout has CLK- on GPIO12, CLK+ on GPIO13 etc (N on even, P on
    # odd). The HSTX driver's default branch maps that exact polarity, so
    # HSTX_INVERT_DIFFPAIRS stays *unset* -- enabling it would invert every
    # lane and break the signal. (murmnes' M2 build also leaves it unset.)
    option(HDMI_HSTX_INVERT_DIFFPAIRS "Swap P/N polarity on every HSTX diff pair" OFF)
    if(HDMI_HSTX_INVERT_DIFFPAIRS)
        target_compile_definitions(drivers PUBLIC HSTX_INVERT_DIFFPAIRS=1)
    endif()
    target_compile_definitions(drivers PRIVATE
        BOARD_${BOARD_VARIANT}
        PSRAM_MAX_FREQ_MHZ=${PSRAM_SPEED}
    )
    target_compile_options(drivers PRIVATE -Ofast)
    target_link_libraries(drivers PUBLIC
        pico_stdlib pico_multicore
        hardware_dma hardware_pio hardware_spi hardware_clocks
        hardware_irq hardware_sync hardware_gpio
    )
else()
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
endif()

# --- Main executable --------------------------------------------------------
if(HDMI_HSTX)
    # HDMI audio path uses HDMI_hstx_audio.c (compiled into drivers); no
    # I2S source file needed in the executable.
    add_executable(frank-video
        src/main.c
        src/file_browser.c
        src/player.c
    )
else()
    add_executable(frank-video
        src/main.c
        src/file_browser.c
        src/player.c
        drivers/i2s/i2s_audio.c
    )

    # Generate the I2S PIO header against the executable so the
    # audio_i2s.pio.h include resolves at compile time.
    pico_generate_pio_header(frank-video
        ${CMAKE_CURRENT_LIST_DIR}/drivers/i2s/audio_i2s.pio
    )
endif()

target_include_directories(frank-video PRIVATE
    src
    drivers
    drivers/i2s
    drivers/sdcard
    drivers/ps2kbd
)
if(HDMI_HSTX)
    target_include_directories(frank-video PRIVATE drivers/hdmi_hstx)
endif()

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
set(AUTOPLAY_MAX_SECS "0" CACHE STRING "When >0, force the player to stop after N seconds (profiling)")
if(AUTOPLAY_FIRST)
    target_compile_definitions(frank-video PRIVATE AUTOPLAY_FIRST=1)
    if(NOT AUTOPLAY_TARGET STREQUAL "")
        target_compile_definitions(frank-video PRIVATE AUTOPLAY_TARGET="${AUTOPLAY_TARGET}")
    endif()
    if(NOT AUTOPLAY_MAX_SECS STREQUAL "0")
        target_compile_definitions(frank-video PRIVATE AUTOPLAY_MAX_SECS=${AUTOPLAY_MAX_SECS})
    endif()
endif()

set(AUTOPLAY_ALTERNATE "" CACHE STRING "When AUTOPLAY_FIRST is ON, alternate with this filename each loop")
if(AUTOPLAY_FIRST AND NOT AUTOPLAY_ALTERNATE STREQUAL "")
    target_compile_definitions(frank-video PRIVATE AUTOPLAY_ALTERNATE="${AUTOPLAY_ALTERNATE}")
endif()

# Profiling-only: -DNO_AUDIO=ON disables audio decode/output to isolate
# the video-decode cost from audio synthesis cost.
option(NO_AUDIO "Disable audio (profiling only)" OFF)
if(NO_AUDIO)
    target_compile_definitions(frank-video PRIVATE NO_AUDIO=1)
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
    usbhid
    fatfs
    drivers
)

# USB stdio: device-mode CDC by default (4-second wait in main.c). When
# USB_HID_ENABLED is on, the native USB is owned by TinyUSB Host instead,
# so route stdio over UART. main.c also skips its USB-wait loop in that
# build via the same USB_HID_ENABLED define propagated by the usbhid lib.
if(USB_HID_ENABLED)
    pico_enable_stdio_usb(frank-video 0)
    pico_enable_stdio_uart(frank-video 1)
else()
    pico_enable_stdio_usb(frank-video 1)
    pico_enable_stdio_uart(frank-video 0)
endif()

pico_add_extra_outputs(frank-video)
