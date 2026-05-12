#ifndef BOARD_CONFIG_H
#define BOARD_CONFIG_H

#include "hardware/structs/sysinfo.h"
#include "hardware/vreg.h"

/*
 * frank-video -- RP2350 board configuration.
 *
 * Pin layouts and PSRAM auto-detection are inherited from murmheretic.
 *
 * BOARD_M1 / BOARD_M2 select GPIO layout:
 *   M1: HDMI base 6, SD on SPI0 pins 2/3/4/5, PS/2 on 0/1, I2S on 26/27/28
 *   M2: HDMI base 12, SD on SPI0 pins 6/7/4/5, PS/2 on 2/3, I2S on 9/10/11
 *
 * PSRAM CS pin is detected at runtime based on chip package.
 */

#if !defined(BOARD_M1) && !defined(BOARD_M2)
#define BOARD_M1
#endif

/* CPU/PSRAM/Flash speed defaults -- overridden via CMake. */
#ifndef CPU_CLOCK_MHZ
#define CPU_CLOCK_MHZ 504
#endif

#ifndef CPU_VOLTAGE
#define CPU_VOLTAGE VREG_VOLTAGE_1_65
#endif

#ifndef PSRAM_MAX_FREQ_MHZ
#define PSRAM_MAX_FREQ_MHZ 166
#endif

#ifndef FLASH_MAX_FREQ_MHZ
#define FLASH_MAX_FREQ_MHZ 66
#endif

/* PSRAM CS pin auto-detection (RP2350A vs RP2350B). */
#ifdef BOARD_M1
#define PSRAM_PIN_RP2350A 19
#else
#define PSRAM_PIN_RP2350A 8
#endif
#define PSRAM_PIN_RP2350B 47

static inline uint get_psram_pin(void) {
    uint32_t package_sel = *((io_ro_32 *)(SYSINFO_BASE + SYSINFO_PACKAGE_SEL_OFFSET));
    return (package_sel & 1) ? PSRAM_PIN_RP2350A : PSRAM_PIN_RP2350B;
}

/* M1 layout */
#ifdef BOARD_M1
#define HDMI_PIN_CLKN 6
#define HDMI_PIN_CLKP 7
#define HDMI_PIN_D0N  8
#define HDMI_PIN_D0P  9
#define HDMI_PIN_D1N  10
#define HDMI_PIN_D1P  11
#define HDMI_PIN_D2N  12
#define HDMI_PIN_D2P  13
#define HDMI_BASE_PIN HDMI_PIN_CLKN

#define SDCARD_PIN_CLK    2
#define SDCARD_PIN_CMD    3
#define SDCARD_PIN_D0     4
#define SDCARD_PIN_D3     5

#define PS2_PIN_CLK  0
#define PS2_PIN_DATA 1

#define I2S_DATA_PIN       26
#define I2S_CLOCK_PIN_BASE 27
#endif /* BOARD_M1 */

/* M2 layout */
#ifdef BOARD_M2
#define HDMI_PIN_CLKN 12
#define HDMI_PIN_CLKP 13
#define HDMI_PIN_D0N  14
#define HDMI_PIN_D0P  15
#define HDMI_PIN_D1N  16
#define HDMI_PIN_D1P  17
#define HDMI_PIN_D2N  18
#define HDMI_PIN_D2P  19
#define HDMI_BASE_PIN HDMI_PIN_CLKN

#define SDCARD_PIN_CLK    6
#define SDCARD_PIN_CMD    7
#define SDCARD_PIN_D0     4
#define SDCARD_PIN_D3     5

#define PS2_PIN_CLK  2
#define PS2_PIN_DATA 3

#define I2S_DATA_PIN       9
#define I2S_CLOCK_PIN_BASE 10
#endif /* BOARD_M2 */

#endif /* BOARD_CONFIG_H */
