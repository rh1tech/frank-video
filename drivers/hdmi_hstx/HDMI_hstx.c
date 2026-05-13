/*
 * frank-video -- HDMI shim layer that maps the existing 8bpp 320x240
 * paletted graphics API onto the HSTX (RP2350 native) HDMI driver.
 *
 * The renderer / file_browser / player code was written against the PIO
 * HDMI driver (drivers/HDMI.c) which uses an 8bpp indexed framebuffer and
 * a 256-entry RGB888 palette. HSTX needs RGB565 pixels delivered through
 * a per-scanline callback. We bridge by:
 *   - keeping a 256-entry RGB565 palette that mirrors the RGB888 set the
 *     player feeds us via graphics_set_palette();
 *   - in the scanline callback, looking up each indexed pixel and emitting
 *     it 4x horizontally to fill the 1280-wide active region (320 logical
 *     pixels, 4x replication = pixel-perfect 320x240 letterbox at 60Hz);
 *   - exposing a vsync counter + staged-buffer-swap API identical to the
 *     PIO driver's so the player's tear-free swap path keeps working.
 *
 * Audio over HDMI is handled by HDMI_hstx_audio.c via the Data Island
 * queue exposed by hstx_data_island_queue.h.
 */

#include "HDMI.h"
#include "video_output.h"
#include "hstx_data_island_queue.h"

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/clocks.h"

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/* =========================================================================
 * Globals expected by the legacy HDMI API (mirrored from drivers/HDMI.c).
 * =========================================================================*/

int graphics_buffer_width  = 320;
int graphics_buffer_height = 240;
int graphics_buffer_shift_x = 0;
int graphics_buffer_shift_y = 0;
enum graphics_mode_t hdmi_graphics_mode = GRAPHICSMODE_DEFAULT;

static uint8_t *graphics_buffer = NULL;

/* Tear-free swap: producer stages here, the per-frame vsync callback
 * (running on Core 1 inside the HSTX DMA ISR's vsync slot) promotes it
 * to graphics_buffer at the start of vblank. */
static uint8_t * volatile graphics_buffer_pending = NULL;
static volatile uint32_t  graphics_vsync_count = 0;

/* RGB888 palette keeps the API identical to the PIO driver -- callers
 * write graphics_set_palette(idx, 0xRRGGBB). We pre-encode each entry to
 * RGB565 so the scanline hot path is a single 16-bit indexed load.
 *
 * Stored in scratch_y (SRAM bank 5) so the per-scanline palette LUT
 * loads on Core 1 don't compete on the same SRAM bank as the framebuffer
 * (bank 0). On RP2350, scratch_y is a 4 KB bank reserved for Core 1. */
static uint16_t __scratch_y("hdmi") palette565[256];

/* Active-line buffer: the HSTX driver runs in 640x480 @ 60Hz mode (VIC=1)
 * for max compatibility with TVs / capture cards. We feed 320 source
 * pixels with 2x horizontal replication = 640 dst pixels = 320 u32 words.
 * Vertical doubling is handled by emitting the same source row for two
 * consecutive active scanlines. */
#define ACTIVE_PIXELS_PER_LINE 640
#define ACTIVE_WORDS_PER_LINE  (ACTIVE_PIXELS_PER_LINE / 2)

/* =========================================================================
 * Public API -- match drivers/HDMI.c signatures exactly.
 * =========================================================================*/

void graphics_set_buffer(uint8_t *buffer) {
    graphics_buffer = buffer;
}

void graphics_set_buffer_at_vsync(uint8_t *buffer) {
    graphics_buffer_pending = buffer;
}

void graphics_wait_vsync(void) {
    uint32_t start = graphics_vsync_count;
    while (graphics_vsync_count == start) tight_loop_contents();
}

uint8_t* graphics_get_buffer(void)   { return graphics_buffer; }
uint32_t graphics_get_width(void)    { return (uint32_t)graphics_buffer_width; }
uint32_t graphics_get_height(void)   { return (uint32_t)graphics_buffer_height; }

void graphics_set_res(int w, int h) {
    graphics_buffer_width  = w;
    graphics_buffer_height = h;
}

void graphics_set_shift(int x, int y) {
    graphics_buffer_shift_x = x;
    graphics_buffer_shift_y = y;
}

static struct video_mode_t hstx_video_mode = {
    .h_total  = MODE_V_TOTAL_LINES,
    .h_width  = MODE_V_ACTIVE_LINES,
    .freq     = 60,
    .vgaPxClk = 25200000
};

struct video_mode_t graphics_get_video_mode(int mode) {
    (void)mode;
    return hstx_video_mode;
}

/* The legacy driver reserves indices 240..243 for HDMI sync-control
 * codes; HSTX has no such reservation, so this is a no-op here.
 * Keep the symbol so file_browser / player don't need ifdefs. */
void graphics_restore_sync_colors(void) {
    /* nothing to do on HSTX */
}

static inline uint16_t rgb888_to_rgb565(uint32_t color888) {
    uint8_t r = (color888 >> 16) & 0xFF;
    uint8_t g = (color888 >>  8) & 0xFF;
    uint8_t b = (color888      ) & 0xFF;
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

void graphics_set_palette(uint8_t i, uint32_t color888) {
    palette565[i] = rgb888_to_rgb565(color888);
}

void graphics_set_bgcolor(uint32_t color888) {
    /* The PIO driver folds the background colour into palette index 255.
     * Mirror that here so callers that rely on the convention keep working. */
    graphics_set_palette(255, color888);
}

/* =========================================================================
 * HSTX scanline callback. Runs in DMA ISR context on Core 1; must be in
 * SRAM and avoid flash accesses. Each call must fill 640 u32 words with
 * two packed RGB565 pixels per word.
 * =========================================================================*/

static void __not_in_flash_func(hstx_scanline_cb)(uint32_t v_scanline,
                                                  uint32_t active_line,
                                                  uint32_t *line_buf) {
    (void)v_scanline;

    const uint8_t *fb = graphics_buffer;

    /* 640x480 active output. We line-double the 320x240 source: each
     * source row covers two consecutive output rows. */
    int src_y = (int)active_line >> 1;

    if (!fb || src_y >= graphics_buffer_height) {
        for (int i = 0; i < ACTIVE_WORDS_PER_LINE; i++) line_buf[i] = 0;
        return;
    }

    const uint8_t *src = fb + src_y * graphics_buffer_width;
    uint32_t *dst = line_buf;

    /* 320 source px -> 640 dst px = each source pixel emitted 2x.
     * Two dst pixels (= one u32 with both halves equal) per source pixel. */
    int w = graphics_buffer_width;
    if (w > 320) w = 320;

    for (int x = 0; x < w; x++) {
        uint16_t c = palette565[src[x]];
        uint32_t pair = (uint32_t)c | ((uint32_t)c << 16);
        *dst++ = pair;
    }

    /* Zero any tail (shouldn't happen with w=320 but cheap to guard). */
    int tail_words = ACTIVE_WORDS_PER_LINE - (int)(dst - line_buf);
    for (int i = 0; i < tail_words; i++) dst[i] = 0;
}

static void __not_in_flash_func(hstx_vsync_cb)(void) {
    /* Apply staged buffer at the boundary of vblank, identical semantics
     * to the PIO driver. */
    uint8_t *staged = graphics_buffer_pending;
    if (staged && staged != graphics_buffer) {
        graphics_buffer = staged;
    }
    graphics_vsync_count++;
}

/* =========================================================================
 * Initialisation. Mirrors the murmnes m2 path: clk_hstx is derived from
 * pll_sys (252 MHz / 2 = 126 MHz pixel clock domain), HDMI Data Island
 * queue is brought up before video_output_init() so audio packets can
 * be enqueued from the moment scanout starts.
 * =========================================================================*/

void graphics_init(g_out g_out) {
    (void)g_out;

    /* Bring up the Data Island queue before scanout so the DMA ISR has a
     * valid silence packet to fall back on. video_output_init() will set
     * a default 48 kHz audio rate; the player overrides it in
     * i2s_audio_init() (= HDMI_hstx_audio.c) once it knows the clip's
     * sample rate. */
    hstx_di_queue_init();

    video_output_set_vsync_callback(hstx_vsync_cb);
    video_output_set_scanline_callback(hstx_scanline_cb);
    /* video_output_init() reconfigures clk_hstx using MODE_HSTX_CLK_DIV
     * (set in CMake for the active CPU clock) so 504 MHz sys yields a
     * 126 MHz hstx domain. */
    video_output_init(graphics_buffer_width, graphics_buffer_height);

    /* HSTX scanout / DMA ISR live on Core 1, exactly like murmnes' m2 path. */
    multicore_launch_core1(video_output_core1_run);

    /* Give Core 1 a moment to bring up HSTX before the caller starts
     * pushing pixels. */
    sleep_ms(50);
}

/* =========================================================================
 * Stubs for compatibility with the old API. The video player and
 * file_browser don't actually use these on the HSTX path.
 * =========================================================================*/

void startVIDEO(uint8_t vol) { (void)vol; }
void set_palette(uint8_t n)  { (void)n; }
