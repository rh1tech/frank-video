/*
 * frank-video -- entry point.
 *
 * Boots the RP2350 to 504 MHz with PSRAM @ 166 MHz / Flash @ 66 MHz, brings
 * up HDMI (320x240x256), I2S audio, SD card and PS/2 keyboard, then loops:
 *     file browser  ->  MPEG-1 player  ->  file browser  ->  ...
 *
 * The 4-second startup wait gives a host time to enumerate the USB CDC
 * device so early printf() lines aren't lost.
 */

#include "board_config.h"

#include "pico/stdlib.h"
#include "pico/stdio.h"
#include "hardware/clocks.h"
#include "hardware/vreg.h"
#include "hardware/structs/qmi.h"
#include "hardware/watchdog.h"

#include "HDMI.h"
#include "psram_init.h"
#include "psram_allocator.h"
#include "sdcard.h"
#include "ff.h"
#include "ps2kbd_wrapper.h"

#include "file_browser.h"
#include "player.h"

#include <stdio.h>
#include <string.h>

#define DISPLAY_W 320
#define DISPLAY_H 240

/* Two 320x240x8bpp framebuffers in SRAM for double buffering. The HDMI
 * driver scans whichever is current via graphics_set_buffer(); the player
 * renders into the other one, then swaps. The browser uses only one buffer
 * (g_framebuffer) since it is not animation-sensitive. */
uint8_t *g_framebuffer = NULL;
uint8_t *g_fb_back     = NULL;

/* Mounted FatFs object. Kept alive for the lifetime of the program -- the
 * SD stays mounted across browser <-> player transitions. */
static FATFS fs;

/* Flash timing must be tightened before increasing the system clock or the
 * QMI controller will return garbage on the next code-fetch. The math here
 * is the same as murmheretic's -- divisor + RX delay derived from the new
 * system clock and the maximum safe flash frequency. */
static void __no_inline_not_in_flash_func(set_flash_timings)(int cpu_mhz) {
    const int clock_hz = cpu_mhz * 1000000;
    const int max_flash_freq = FLASH_MAX_FREQ_MHZ * 1000000;

    int divisor = (clock_hz + max_flash_freq - (max_flash_freq >> 4) - 1)
                  / max_flash_freq;
    if (divisor == 1 && clock_hz >= 166000000) divisor = 2;

    int rxdelay = divisor;
    if (clock_hz / divisor > 100000000 && clock_hz >= 166000000) rxdelay += 1;

    qmi_hw->m[0].timing = 0x60007000
        | rxdelay << QMI_M0_TIMING_RXDELAY_LSB
        | divisor << QMI_M0_TIMING_CLKDIV_LSB;
}

static void overclock_and_init_clocks(void) {
#if CPU_CLOCK_MHZ > 252
    vreg_disable_voltage_limit();
    vreg_set_voltage(CPU_VOLTAGE);
    set_flash_timings(CPU_CLOCK_MHZ);
    sleep_ms(100);  /* let voltage and timings stabilise */
#endif

    if (!set_sys_clock_khz(CPU_CLOCK_MHZ * 1000, false)) {
        /* Fallback to a known-good clock so we still come up if the
         * requested overclock can't be exactly synthesised. */
        set_sys_clock_khz(252 * 1000, true);
    }
}

static void wait_for_usb_serial(uint32_t ms) {
    /* Two reasons to wait here:
     *   1. Give the host's USB stack time to enumerate the CDC device so
     *      the first printf() lines actually reach the user.
     *   2. Provide a uniform 4 s "press BOOTSEL now" window if the user
     *      wants to recover the device after an OC misadventure. */
    const uint32_t deadline = to_ms_since_boot(get_absolute_time()) + ms;
    while (to_ms_since_boot(get_absolute_time()) < deadline) {
        sleep_ms(50);
    }
}

static bool mount_sd(void) {
    FRESULT fr = f_mount(&fs, "", 1);
    if (fr != FR_OK) {
        printf("SD: mount failed (%d)\n", (int)fr);
        return false;
    }
    f_chdir("/");
    return true;
}

static void show_sd_error_screen(void) {
    if (!g_framebuffer) return;
    /* Minimal red-on-black error -- we may never have run the browser yet,
     * so its palette isn't loaded. Index 1 (red) is enough. */
    graphics_set_palette(0, 0x000000);
    graphics_set_palette(1, 0xFF4040);
    memset(g_framebuffer, 0, DISPLAY_W * DISPLAY_H);
    /* Three diagonal stripes -- crude but visible. */
    for (int y = 100; y < 140; y++) {
        for (int x = 60; x < 260; x += 1) {
            if (((x + y) & 0x07) < 4) g_framebuffer[y * DISPLAY_W + x] = 1;
        }
    }
}

int main(void) {
    /* 1. Voltage + flash timing + system clock. */
    overclock_and_init_clocks();

    /* 2. USB CDC stdio with a 4 s grace window for the host. */
    stdio_init_all();
    wait_for_usb_serial(4000);

    printf("\nfrank-video " FRANK_VERSION "\n");
    printf("clk_sys = %lu MHz\n", clock_get_hz(clk_sys) / 1000000);
    printf("PSRAM target = %d MHz, Flash target = %d MHz\n",
           PSRAM_MAX_FREQ_MHZ, FLASH_MAX_FREQ_MHZ);

    /* 3. PSRAM. The pin depends on the chip package -- get_psram_pin()
     * detects the variant at runtime. */
    uint psram_pin = get_psram_pin();
    psram_init(psram_pin);
    psram_set_sram_mode(0);
    printf("PSRAM: CS=%u\n", psram_pin);

    /* 4. Two SRAM framebuffers for double buffering. Both live in SRAM so
     * HDMI scanout DMA and CPU render writes don't compete for PSRAM
     * bandwidth (PSRAM stores cost ~5x SRAM stores at this clock; the
     * single-buffer SRAM move alone dropped 320x240 render from 15ms to
     * 7.5ms). With two buffers, render writes never overlap with scanout
     * reads, so we also stop tearing. The player swaps buffers after each
     * render; the browser draws into whichever is current and sticks. */
    static uint8_t framebuffer_a[DISPLAY_W * DISPLAY_H] __attribute__((aligned(4)));
    static uint8_t framebuffer_b[DISPLAY_W * DISPLAY_H] __attribute__((aligned(4)));
    g_framebuffer = framebuffer_a;  /* front: HDMI scans this */
    g_fb_back     = framebuffer_b;  /* back: renderer writes this */
    memset(framebuffer_a, 0, sizeof(framebuffer_a));
    memset(framebuffer_b, 0, sizeof(framebuffer_b));

    /* 5. HDMI: 320x240, 8bpp paletted. */
    graphics_init(g_out_HDMI);
    graphics_set_res(DISPLAY_W, DISPLAY_H);
    graphics_set_buffer(g_framebuffer);

    /* 6. SD card. If it fails the user just sees a static error screen --
     * we don't reboot, so they can yank/reseat the card and reset. */
    if (!mount_sd()) {
        show_sd_error_screen();
        while (1) sleep_ms(1000);
    }
    printf("SD: mounted\n");

    /* 7. PS/2 keyboard. */
    ps2kbd_init();
    printf("PS/2 keyboard: ready\n");

    /* 8. Browser <-> player loop. */
    file_browser_init();
    char path[FB_PATH_MAX];

#ifdef AUTOPLAY_FIRST
    /* Headless profiling mode: skip the browser. Prefer AUTOPLAY_TARGET
     * (set via -DAUTOPLAY_TARGET=KYZYA.MPG), otherwise fall back to the
     * first .mpg encountered. */
#ifndef AUTOPLAY_TARGET
#define AUTOPLAY_TARGET ""
#endif
    {
        DIR dir;
        FILINFO fno;
        const char *root = "/video";
        FRESULT fr = f_opendir(&dir, root);
        if (fr != FR_OK) { root = "/"; fr = f_opendir(&dir, root); }

        char fallback[FB_PATH_MAX] = {0};
        char target[FB_PATH_MAX] = {0};
        const char *want = AUTOPLAY_TARGET;
        bool have_target = (want[0] != 0);

        printf("AUTOPLAY: scanning %s for *.mpg (want='%s')\n",
               root, have_target ? want : "<first>");
        if (fr == FR_OK) {
            while (f_readdir(&dir, &fno) == FR_OK && fno.fname[0]) {
                if (fno.fattrib & AM_DIR) continue;
                size_t n = strlen(fno.fname);
                if (n < 4) continue;
                const char *e = &fno.fname[n - 4];
                if (!(e[0] == '.'
                      && (e[1] == 'm' || e[1] == 'M')
                      && (e[2] == 'p' || e[2] == 'P')
                      && (e[3] == 'g' || e[3] == 'G'))) continue;

                printf("AUTOPLAY: found %s/%s\n", root, fno.fname);

                if (have_target) {
                    /* Case-insensitive compare against AUTOPLAY_TARGET. */
                    int match = 1;
                    const char *a = fno.fname, *b = want;
                    while (*a && *b) {
                        char ca = *a, cb = *b;
                        if (ca >= 'a' && ca <= 'z') ca -= 32;
                        if (cb >= 'a' && cb <= 'z') cb -= 32;
                        if (ca != cb) { match = 0; break; }
                        a++; b++;
                    }
                    if (match && *a == 0 && *b == 0) {
                        snprintf(target, sizeof(target), "%s/%s", root, fno.fname);
                    }
                }
                if (fallback[0] == 0) {
                    snprintf(fallback, sizeof(fallback), "%s/%s", root, fno.fname);
                }
            }
            f_closedir(&dir);
        }

        const char *chosen = target[0] ? target : fallback;
        if (!chosen[0]) {
            printf("AUTOPLAY: no .mpg found\n");
            while (1) sleep_ms(1000);
        }
        snprintf(path, sizeof(path), "%s", chosen);
        printf("AUTOPLAY: %s\n", path);

#ifdef AUTOPLAY_ALTERNATE
        /* Profiling-only: alternate between two clips on each loop so we
         * exercise the full close-then-open path with different sample
         * rates each time. Set AUTOPLAY_ALTERNATE=NAME.MPG to alternate
         * with that file (in /video/). */
        char other_path[FB_PATH_MAX];
        snprintf(other_path, sizeof(other_path), "/video/%s", AUTOPLAY_ALTERNATE);
        printf("AUTOPLAY: alternating with %s\n", other_path);
        unsigned int loop_idx = 0;
        for (;;) {
            const char *p = (loop_idx & 1u) ? other_path : path;
            printf("AUTOPLAY: cycle %u -> %s\n", loop_idx, p);
            player_play(p);
            printf("AUTOPLAY: looped\n");
            loop_idx++;
        }
#else
        for (;;) {
            player_play(path);
            printf("AUTOPLAY: looped\n");
        }
#endif
    }
#endif

    while (true) {
        if (!file_browser_show(path, sizeof(path))) {
            /* ESC pressed in the browser -- nothing else to do at the top
             * level, so re-enter the browser. There's no power-down on this
             * board, so falling out would just leave a black screen. */
            continue;
        }
        printf("playing: %s\n", path);
        player_play(path);
        printf("playback ended\n");
    }
}
