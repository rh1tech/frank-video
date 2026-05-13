/*
 * frank-video -- MPEG-1 video player.
 *
 * Adapted from rhea/apps/source/videoplayer/main.c. The original ran as a
 * FrankOS app and used the display_, pcm_ and keyboard_ APIs. This port
 * talks directly to the murmheretic-style HDMI driver, the murmnes I2S
 * driver, and our PS/2 keyboard wrapper.
 *
 * Renderer (1x and 2x upscale, RGB332 palette + 4-position dither) is kept
 * verbatim -- it is the slow path on RP2350 and any change carries risk.
 */

#include "player.h"
#include "board_config.h"
#include "HDMI.h"
#include "i2s_audio.h"
#include "psram_allocator.h"
#include "ps2kbd_wrapper.h"
#include "ff.h"
#ifdef HDMI_HSTX
#include "hstx_data_island_queue.h"
#endif

#include "pico/stdlib.h"

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#define PLM_NO_STDIO
/* 256 KB ring buffer is plenty for streaming; halving it from rhea's 512 KB
 * frees ~256 KB and lets us push more pl_mpeg state into SRAM. */
#define PLM_BUFFER_DEFAULT_SIZE (256 * 1024)

/* Route pl_mpeg allocations:
 *   - Big chunks (>=128 KB) -> PSRAM (only the stream ring buffer hits this)
 *   - Everything else      -> SRAM via the standard malloc, so the YCbCr
 *                             frame planes that pl_mpeg touches every
 *                             macroblock decode live in fast memory.
 *
 * Original rhea videoplayer routed everything to PSRAM and ran on a
 * different memory map; on this build the PSRAM-resident YCbCr planes
 * were the dominant decode cost (~300ms per frame). Splitting them gets
 * us back into realtime budget. */
static void *plm_alloc(size_t sz) {
    if (sz >= 128 * 1024) return psram_malloc(sz);
    return malloc(sz);
}

/* PSRAM is XIP-mapped at 0x11000000..0x11800000. Distinguish allocator
 * origin by address so we route free/realloc to the right backend. */
#define PLM_PSRAM_LO 0x11000000u
#define PLM_PSRAM_HI 0x12000000u

static void plm_free_(void *p) {
    if (!p) return;
    uintptr_t a = (uintptr_t)p;
    if (a >= PLM_PSRAM_LO && a < PLM_PSRAM_HI) {
        psram_free(p);  /* bump allocator: no-op, but cheap */
    } else {
        free(p);
    }
}

static void *plm_realloc_(void *p, size_t sz) {
    if (!p) return plm_alloc(sz);
    uintptr_t a = (uintptr_t)p;
    if (a >= PLM_PSRAM_LO && a < PLM_PSRAM_HI) {
        return psram_realloc(p, sz);
    }
    return realloc(p, sz);
}

#define PLM_MALLOC(sz)     plm_alloc(sz)
#define PLM_FREE(p)        plm_free_(p)
#define PLM_REALLOC(p, sz) plm_realloc_(p, sz)

#define PL_MPEG_IMPLEMENTATION
#include "pl_mpeg.h"

/* HID usage codes (USB HID Keyboard/Keypad page). */
#define HID_KEY_ESCAPE 0x29
#define HID_KEY_SPACE  0x2C

#define DISPLAY_W 320
#define DISPLAY_H 240
#define LINE_BUF_W 336

/* Framebuffers are owned by main.c. g_framebuffer is the front buffer
 * (currently scanned out by HDMI); g_fb_back is the back buffer where the
 * renderer draws. After a frame is rendered we swap them and tell the
 * HDMI driver about the new front. */
extern uint8_t *g_framebuffer;
extern uint8_t *g_fb_back;

/* Dither tables: 4 Bayer positions x 3 channels (R,G,B). */
#define DT_SZ   1024
#define DT_BIAS 256

typedef struct {
    bool      closing;
    bool      paused;
    plm_t    *plm;
    FIL      *fil;
    uint8_t  *y_tab;         /* 256-entry Y -> display lookup */
    uint8_t  *dt;            /* 12 x 1024 dither tables */
    uint32_t  time_debt;
    uint8_t   skip_count;
    bool      audio_inited;
    int       offset_x;
    int       offset_y;
    int       video_w;
    int       video_h;

    /* Profiling counters reset once per second by the main loop. */
    uint32_t  prof_frames_decoded;
    uint32_t  prof_frames_rendered;
    uint32_t  prof_frames_skipped;
    uint64_t  prof_render_us;
    uint64_t  prof_audio_cb_us;
    uint32_t  prof_audio_calls;
    uint32_t  prof_audio_dropped;  /* frames lost to ring overflow */
} player_state_t;

static player_state_t G;

/* ===== Framebuffer ====================================================== */

/* The renderer always draws into the back buffer; on_video() flips after
 * each successful render. fb_get() is what the on_video() path uses, so it
 * returns the back buffer pointer. */
static uint8_t *fb_get(void) { return g_fb_back; }

/* ===== RGB332 palette =================================================== */

/* sRGB-to-linear gamma applied to 6 levels per channel.
 *
 * The dither maths runs in linear "level" space (0..5 per channel), but
 * the panel and the human eye expect sRGB-encoded output. If we just
 * write level*51 directly, the perceived steps are uneven: the gap
 * between level 0 and level 1 looks ~5x larger than between level 4 and
 * level 5, so dark gradients band visibly even with dithering.
 *
 * Pre-baking the sRGB transfer curve into the palette makes the visible
 * steps approximately equal-luminance, so dither noise averages to the
 * intended midtone instead of crushing into the nearest dark/light
 * level. Values come from sRGB encoding of (i / 5)^(1/2.2) -- close
 * enough to the real piecewise sRGB curve at 6 levels. */
static const uint8_t kSRGB6[6] = {
    /* linear 0/5, 1/5, 2/5, 3/5, 4/5, 5/5 -> 8-bit sRGB */
    0x00, 0x67, 0x90, 0xB1, 0xCE, 0xEA
};

/* 6x6x6 gamma-correct colour cube + 40-step grayscale ramp (216-255). */
static void setup_palette(void) {
    for (int r = 0; r < 6; r++)
        for (int g = 0; g < 6; g++)
            for (int b = 0; b < 6; b++) {
                int idx = r * 36 + g * 6 + b;
                uint32_t rgb = ((uint32_t)kSRGB6[r] << 16)
                             | ((uint32_t)kSRGB6[g] << 8)
                             |  (uint32_t)kSRGB6[b];
                graphics_set_palette((uint8_t)idx, rgb);
            }
    /* Greyscale ramp uses the same sRGB curve so the dark steps don't
     * crush either. The dither path doesn't index this region but it's
     * what the file browser uses, and it keeps a smooth fade-to-black. */
    for (int i = 0; i < 40; i++) {
        /* sRGB-encode (i / 39) at gamma 2.2 */
        float lin = (float)i / 39.0f;
        float enc = (lin <= 0.0f) ? 0.0f
                  : (lin >= 1.0f) ? 1.0f
                  : powf(lin, 1.0f / 2.2f);
        uint8_t v = (uint8_t)(enc * 255.0f + 0.5f);
        uint32_t rgb = ((uint32_t)v << 16) | ((uint32_t)v << 8) | v;
        graphics_set_palette((uint8_t)(216 + i), rgb);
    }
}

/* ===== YCbCr -> RGB332 dither tables ===================================== */

/* Classic 4x4 Bayer ordered-dither matrix, 0..15. Compared to the 2x2
 * matrix the previous renderer used (4 thresholds), this gives 16
 * thresholds spread evenly between palette steps -- enough to break
 * the visible banding on slow gradients (sky, skin) without resorting
 * to error diffusion (which would serialise the inner loop). */
static const uint8_t kBayer4x4[16] = {
     0,  8,  2, 10,
    12,  4, 14,  6,
     3, 11,  1,  9,
    15,  7, 13,  5
};

static void init_tables(void) {
    G.y_tab = (uint8_t *)malloc(256);
    for (int i = 0; i < 256; i++) {
        int v = ((i - 16) * 76309) >> 16;
        if (v < 0) v = 0; else if (v > 255) v = 255;
        G.y_tab[i] = (uint8_t)v;
    }

    /* 16 Bayer positions x {R, G, B} = 48 dither tables of 1024 bytes
     * each = 48 KB SRAM. Each table maps a (signed-biased) Y+chroma
     * delta to a pre-quantised palette component, so the inner render
     * loop is three loads + two adds per pixel regardless of the
     * Bayer-matrix size. The threshold for cell p is
     *     (kBayer4x4[p] + 0.5) / 16  *  step
     * where step = 255 / 5 = 51 (the gap between adjacent palette
     * levels per channel). */
    G.dt = (uint8_t *)malloc(48 * DT_SZ);
    for (int p = 0; p < 16; p++) {
        int th = (kBayer4x4[p] * 2 + 1) * 51 / 32;  /* (b+0.5)/16 * 51 */
        uint8_t *dr = G.dt + (p * 3 + 0) * DT_SZ;
        uint8_t *dg = G.dt + (p * 3 + 1) * DT_SZ;
        uint8_t *db = G.dt + (p * 3 + 2) * DT_SZ;
        for (int i = 0; i < DT_SZ; i++) {
            int v = i - DT_BIAS;
            if (v < 0) v = 0; if (v > 255) v = 255;

            int q = (v + th) * 5 / 255;
            if (q > 5) q = 5;
            dr[i] = (uint8_t)(q * 36);
            dg[i] = (uint8_t)(q * 6);
            db[i] = (uint8_t)(q);
        }
    }
}

/* ===== Renderers (kept identical to rhea's videoplayer) ================= */

/* Look up the (R,G,B) dither tables for Bayer position p (0..15). */
#define DT_R(p) (G.dt + ((p) * 3 + 0) * DT_SZ + DT_BIAS)
#define DT_G(p) (G.dt + ((p) * 3 + 1) * DT_SZ + DT_BIAS)
#define DT_B(p) (G.dt + ((p) * 3 + 2) * DT_SZ + DT_BIAS)

static void __no_inline_not_in_flash_func(render_1x_rows)(uint8_t *fb, plm_frame_t *frame,
                           int row_start, int row_end) {
    int cols = (int)frame->width >> 1;
    int yw = (int)frame->y.width;
    int cw = (int)frame->cb.width;
    int ox = G.offset_x;
    int oy = G.offset_y;
    const uint8_t *ytab = G.y_tab;

    if (cols > 160) cols = 160;

    uint8_t yl0[LINE_BUF_W], yl1[LINE_BUF_W];
    uint8_t cbl[LINE_BUF_W/2], crl[LINE_BUF_W/2];

    for (int row = row_start; row < row_end; row++) {
        memcpy(yl0, frame->y.data  + row*2*yw,      cols*2);
        memcpy(yl1, frame->y.data  + row*2*yw + yw, cols*2);
        memcpy(cbl, frame->cb.data + row*cw,        cols);
        memcpy(crl, frame->cr.data + row*cw,        cols);

        int dest_y_top = (oy + row*2);
        int dest_y_bot = dest_y_top + 1;
        int by_top = dest_y_top & 3;
        int by_bot = dest_y_bot & 3;

        int di = dest_y_top * DISPLAY_W + ox;
        int yi = 0;

        for (int col = 0; col < cols; col++) {
            int dest_x_l = (ox + col*2);
            int dest_x_r = dest_x_l + 1;
            int bx_l = dest_x_l & 3;
            int bx_r = dest_x_r & 3;

            /* Pick the four Bayer positions for this 2x2 dest block. */
            int p_tl = by_top * 4 + bx_l;
            int p_tr = by_top * 4 + bx_r;
            int p_bl = by_bot * 4 + bx_l;
            int p_br = by_bot * 4 + bx_r;

            const uint8_t *rTL = DT_R(p_tl), *gTL = DT_G(p_tl), *bTL = DT_B(p_tl);
            const uint8_t *rTR = DT_R(p_tr), *gTR = DT_G(p_tr), *bTR = DT_B(p_tr);
            const uint8_t *rBL = DT_R(p_bl), *gBL = DT_G(p_bl), *bBL = DT_B(p_bl);
            const uint8_t *rBR = DT_R(p_br), *gBR = DT_G(p_br), *bBR = DT_B(p_br);

            int cr = crl[col] - 128;
            int cb = cbl[col] - 128;
            int rd = (cr * 104597) >> 16;
            int gd = (cb * 25674 + cr * 53278) >> 16;
            int bd = (cb * 132201) >> 16;
            int yy;

            yy = ytab[yl0[yi]];
            fb[di]     = rTL[yy+rd] + gTL[yy-gd] + bTL[yy+bd];
            yy = ytab[yl0[yi+1]];
            fb[di+1]   = rTR[yy+rd] + gTR[yy-gd] + bTR[yy+bd];
            yy = ytab[yl1[yi]];
            fb[di+DISPLAY_W]   = rBL[yy+rd] + gBL[yy-gd] + bBL[yy+bd];
            yy = ytab[yl1[yi+1]];
            fb[di+DISPLAY_W+1] = rBR[yy+rd] + gBR[yy-gd] + bBR[yy+bd];

            yi += 2;
            di += 2;
        }
    }
}

static void __not_in_flash_func(render_2x)(uint8_t *fb, plm_frame_t *frame) {
    int cols = (int)frame->width >> 1;
    int rows = (int)frame->height >> 1;
    int yw = (int)frame->y.width;
    int cw = (int)frame->cb.width;
    int ox = G.offset_x;
    int oy = G.offset_y;
    const uint8_t *ytab = G.y_tab;

    if (cols > 80) cols = 80;
    if (rows > 60) rows = 60;

    /* For 2x upscale, each source pixel fills a 2x2 dest block of one
     * colour, so all 4 dest pixels in that block share one Bayer
     * threshold. The 4 source-pixel positions per chroma block (TL, TR,
     * BL, BR) get 4 different threshold positions chosen from the 4x4
     * Bayer matrix to maximise per-source-pixel variation -- the
     * Bayer cells {(0,0),(2,1),(1,3),(3,2)} sit at near-equal value
     * gaps, so this gives an even threshold distribution. */
    static const uint8_t kSrc4[4] = {
        /* (0,0)=0, (2,1)=14, (1,3)=11, (3,2)=13 */
        0 * 4 + 0,
        1 * 4 + 2,
        3 * 4 + 1,
        2 * 4 + 3,
    };
    const uint8_t *rp[4], *gp[4], *bp[4];
    for (int i = 0; i < 4; i++) {
        rp[i] = DT_R(kSrc4[i]);
        gp[i] = DT_G(kSrc4[i]);
        bp[i] = DT_B(kSrc4[i]);
    }

    uint8_t yl0[LINE_BUF_W], yl1[LINE_BUF_W];
    uint8_t cbl[LINE_BUF_W/2], crl[LINE_BUF_W/2];

    for (int row = 0; row < rows; row++) {
        memcpy(yl0, frame->y.data  + row*2*yw,      cols*2);
        memcpy(yl1, frame->y.data  + row*2*yw + yw, cols*2);
        memcpy(cbl, frame->cb.data + row*cw,        cols);
        memcpy(crl, frame->cr.data + row*cw,        cols);

        int di0 = (oy + row*4    ) * DISPLAY_W + ox;
        int di1 = (oy + row*4 + 1) * DISPLAY_W + ox;
        int di2 = (oy + row*4 + 2) * DISPLAY_W + ox;
        int di3 = (oy + row*4 + 3) * DISPLAY_W + ox;
        int yi = 0;

        for (int col = 0; col < cols; col++) {
            int cr = crl[col] - 128;
            int cb = cbl[col] - 128;
            int rd = (cr * 104597) >> 16;
            int gd = (cb * 25674 + cr * 53278) >> 16;
            int bd = (cb * 132201) >> 16;
            int yy;
            uint8_t px;

            yy = ytab[yl0[yi]];
            px = rp[0][yy+rd] + gp[0][yy-gd] + bp[0][yy+bd];
            fb[di0]   = px; fb[di0+1] = px;
            fb[di1]   = px; fb[di1+1] = px;

            yy = ytab[yl0[yi+1]];
            px = rp[1][yy+rd] + gp[1][yy-gd] + bp[1][yy+bd];
            fb[di0+2] = px; fb[di0+3] = px;
            fb[di1+2] = px; fb[di1+3] = px;

            yy = ytab[yl1[yi]];
            px = rp[2][yy+rd] + gp[2][yy-gd] + bp[2][yy+bd];
            fb[di2]   = px; fb[di2+1] = px;
            fb[di3]   = px; fb[di3+1] = px;

            yy = ytab[yl1[yi+1]];
            px = rp[3][yy+rd] + gp[3][yy-gd] + bp[3][yy+bd];
            fb[di2+2] = px; fb[di2+3] = px;
            fb[di3+2] = px; fb[di3+3] = px;

            yi += 2;
            di0 += 4; di1 += 4; di2 += 4; di3 += 4;
        }
    }
}

/* ===== pl_mpeg callbacks =============================================== */

static void __not_in_flash_func(on_video)(plm_t *mpeg, plm_frame_t *frame, void *user) {
    (void)mpeg; (void)user;

    G.prof_frames_decoded++;

    /* Adaptive frame skip: when behind the wall clock, drop a render but
     * keep at least every third frame so the picture doesn't stall. */
    G.skip_count++;
    if (G.time_debt > 20 && G.skip_count < 3) {
        G.prof_frames_skipped++;
        return;
    }
    G.skip_count = 0;

    uint8_t *fb = fb_get();
    if (!fb) return;

    uint64_t t0 = time_us_64();
    /* Single-core render. We tried offloading this to core1 to overlap
     * with decode but it destabilised HDMI: core1's 76 KB/frame of SRAM
     * writes stalled the HDMI scanline DMA IRQ on core0, producing a
     * jittery picture. The marginal CPU savings on hard content (~10%)
     * weren't worth the regression on what already worked, so we render
     * inline and rely on the vsync swap below to keep things tear-free. */
    if ((int)frame->width <= 160 && (int)frame->height <= 120) {
        render_2x(fb, frame);
    } else {
        int rows = (int)frame->height >> 1;
        if (rows > 120) rows = 120;
        render_1x_rows(fb, frame, 0, rows);
    }
    uint8_t *new_front = g_fb_back;
    g_fb_back = g_framebuffer;
    g_framebuffer = new_front;
    graphics_set_buffer_at_vsync(new_front);
    G.prof_render_us += (time_us_64() - t0);
    G.prof_frames_rendered++;
}

static void __not_in_flash_func(on_audio)(plm_t *mpeg, plm_samples_t *samples, void *user) {
    (void)mpeg; (void)user;
    /* MPEG audio comes as float32 interleaved stereo in [-1,1]. Convert to
     * int16 stereo at 25% (matches rhea's headroom choice) and push to I2S. */
    int n = (int)samples->count;
    if (n <= 0) return;

    uint64_t t0 = time_us_64();
    static int16_t pcm[1152 * 2];
    if (n > 1152) n = 1152;
    for (int i = 0; i < n * 2; i++) {
        int v = (int)(samples->interleaved[i] * 8192.0f);
        if (v >  32767) v =  32767;
        if (v < -32767) v = -32767;
        pcm[i] = (int16_t)v;
    }
    /* Stream-mode push: appends into a software ring drained by the DMA
     * IRQ. Decouples this 1152-frame producer call from the much smaller
     * DMA chunk size so no chunk gets silence-padded mid-burst. */
    int written = i2s_audio_stream_push(pcm, n);
    if (written < n) G.prof_audio_dropped += (uint32_t)(n - written);
    G.prof_audio_cb_us += (time_us_64() - t0);
    G.prof_audio_calls++;
}

/* ===== File I/O callbacks ============================================== */

static void __not_in_flash_func(plm_load_cb)(plm_buffer_t *buf, void *user) {
    /* Refill the pl_mpeg ring buffer from FatFS. SD reads sit on the decode
     * critical path -- when pl_mpeg can't find a packet header in the ring
     * it calls back here synchronously. Each f_read of N bytes costs a
     * fixed FatFS overhead (cluster lookup, sector address calc) plus the
     * actual SPI transfer; bigger blocks amortise the overhead. 64 KB is
     * the sweet spot: large enough to hide FatFS overhead, small enough
     * not to stall a long time at low buffer levels. */
    FIL *fil = (FIL *)user;
    if (buf->discard_read_bytes)
        plm_buffer_discard_read_bytes(buf);
    size_t bytes_available = buf->capacity - buf->length;
    if (bytes_available > 65536) bytes_available = 65536;
    UINT br = 0;
    f_read(fil, buf->bytes + buf->length, (UINT)bytes_available, &br);
    buf->length += br;
    if (br == 0) buf->has_ended = TRUE;
}

static void plm_seek_cb(plm_buffer_t *buf, size_t offset, void *user) {
    (void)buf;
    f_lseek((FIL *)user, (FSIZE_t)offset);
}

static size_t plm_tell_cb(plm_buffer_t *buf, void *user) {
    (void)buf;
    return (size_t)f_tell((FIL *)user);
}

/* ===== Input ============================================================ */

static void process_input(void) {
    ps2kbd_tick();
    int pressed;
    uint8_t code;
    while (ps2kbd_get_event(&pressed, &code)) {
        if (!pressed) continue;
        if (code == HID_KEY_ESCAPE) { G.closing = true; return; }
        if (code == HID_KEY_SPACE)  { G.paused = !G.paused; }
    }
}

/* ===== Public entry ===================================================== */

void player_play(const char *path) {
    if (!path || !path[0]) return;

    memset(&G, 0, sizeof(G));

    /* Framebuffers are allocated and registered in main.c. Clear both so
     * neither shows stale browser pixels through the first few page flips,
     * and (re)load the player's RGB332 palette. */
    if (g_framebuffer) memset(g_framebuffer, 0, DISPLAY_W * DISPLAY_H);
    if (g_fb_back)     memset(g_fb_back,     0, DISPLAY_W * DISPLAY_H);
    setup_palette();

    /* The PSRAM allocator is a bump allocator -- psram_free() is a no-op.
     * Each playback allocates a 256 KB stream ring (and a few smaller
     * pl_mpeg structs) in PSRAM that would otherwise leak across clips,
     * so after ~28 plays we exhaust the 7 MB perm pool and the device
     * hangs (HDMI signal disappears, screen goes black, no sound).
     *
     * Mark the current PSRAM offset before any allocation; the cleanup
     * path at the end of player_play() rolls the bump pointer back to
     * here, freeing every PSRAM allocation made during this playback. */
    psram_mark_session();

    init_tables();

    /* core1 is intentionally left dormant: an earlier multicore render
     * pipeline destabilised HDMI scanout. Single-core decode + render is
     * fast enough on most content thanks to the SRAM framebuffer and the
     * RAM-resident pl_mpeg hot paths. */

    G.fil = (FIL *)malloc(sizeof(FIL));
    if (!G.fil || f_open(G.fil, path, FA_READ) != FR_OK) {
        printf("player: cannot open %s\n", path ? path : "(null)");
        if (G.fil) free(G.fil);
        free(G.y_tab); free(G.dt);
        psram_restore_session();
        return;
    }

    FSIZE_t file_size = f_size(G.fil);
    plm_buffer_t *buf = plm_buffer_create_with_callbacks(
        plm_load_cb, plm_seek_cb, plm_tell_cb,
        (size_t)file_size, G.fil);
    G.plm = plm_create_with_buffer(buf, TRUE);
    if (!G.plm) {
        printf("player: not a valid MPEG-1 file\n");
        f_close(G.fil); free(G.fil);
        free(G.y_tab); free(G.dt);
        psram_restore_session();
        return;
    }

    G.video_w = plm_get_width(G.plm);
    G.video_h = plm_get_height(G.plm);
    int samplerate = plm_get_samplerate(G.plm);
    printf("player: %dx%d @ %d fps, audio %d Hz\n",
           G.video_w, G.video_h,
           (int)plm_get_framerate(G.plm), samplerate);

    /* Centre on 320x240; account for 2x upscale of small clips. */
    int disp_w = G.video_w, disp_h = G.video_h;
    if (G.video_w <= 160 && G.video_h <= 120) { disp_w *= 2; disp_h *= 2; }
    G.offset_x = (DISPLAY_W - disp_w) / 2;
    G.offset_y = (DISPLAY_H - disp_h) / 2;
    if (G.offset_x < 0) G.offset_x = 0;
    if (G.offset_y < 0) G.offset_y = 0;

    if (samplerate > 0) {
        i2s_audio_init(I2S_DATA_PIN, I2S_CLOCK_PIN_BASE, (uint32_t)samplerate);
        /* Pick a small DMA chunk for low latency. With the streaming push
         * API the DMA chunk size is independent of the producer's call
         * size, so we just want a chunk small enough to refill quickly
         * (low underrun risk) but large enough that IRQ overhead stays
         * negligible. samplerate/172 ~= 256 frames @ 44.1k = 5.8 ms. */
        i2s_audio_set_frame_rate(172);
        plm_set_audio_enabled(G.plm, TRUE);
        plm_set_audio_decode_callback(G.plm, on_audio, NULL);
        /* audio_lead_time controls how far ahead pl_mpeg decodes audio.
         * Higher = bigger bursts of audio work in plm_decode() when video
         * is behind. With our 16k-frame I2S ring already absorbing 370ms,
         * we don't need pl_mpeg to look ahead at all -- 50ms is plenty to
         * keep audio in sync. Halving from 200ms cuts the worst-case
         * audio decode burst per plm_decode() call. */
        plm_set_audio_lead_time(G.plm, 0.05f);
        G.audio_inited = true;
    } else {
        plm_set_audio_enabled(G.plm, FALSE);
    }
    plm_set_video_decode_callback(G.plm, on_video, NULL);

    /* Main loop -- time-driven decode. We compare pl_mpeg's video PTS to
     * wall-clock time elapsed since playback start to decide whether to
     * pull the next frame, so playback proceeds at exactly source rate. */
    uint32_t playback_start_ms = to_ms_since_boot(get_absolute_time());
    uint32_t last_tick = playback_start_ms;
    uint32_t prof_last_ms = last_tick;
    uint64_t prof_decode_us = 0;
    uint32_t prof_decode_calls = 0;
    while (!G.closing) {
        process_input();
        if (G.closing) break;

        if (G.paused) {
            sleep_ms(50);
            last_tick = to_ms_since_boot(get_absolute_time());
            continue;
        }

        uint32_t now = to_ms_since_boot(get_absolute_time());
        last_tick = now;

#ifdef AUTOPLAY_MAX_SECS
        /* Profiling-only: force the player to exit after AUTOPLAY_MAX_SECS
         * of playback so the autoplay loop exercises the close-then-open
         * path repeatedly. Lets us reproduce restart-audio bugs without a
         * keyboard. */
        if ((now - playback_start_ms) > (AUTOPLAY_MAX_SECS * 1000u)) {
            G.closing = true;
            break;
        }
#endif

        /* Custom decode loop, paced by wall clock. pl_mpeg's plm_decode()
         * interleaves video and audio decode in a tight do-while; on hard
         * B-frame scenes that bursts many audio frames back-to-back since
         * audio time advances much faster than the stalled video. We do
         * it ourselves so audio can never monopolise CPU during a slow
         * video decode.
         *
         * Pacing rule: keep video PTS within ~10 ms ahead of wall clock,
         * keep audio PTS within ~50 ms ahead of video PTS. Each iteration
         * caps video at 4 decodes and audio at 4 (or 1 when debt>30 ms),
         * so a single iteration's audio decode can never bury video. */
        uint64_t t0 = time_us_64();

        float wall_pos_s = (float)(now - playback_start_ms) * 0.001f;
        float video_target_s = wall_pos_s + 0.010f;  /* 10 ms ahead */

        for (int i = 0; i < 4; i++) {
            if (plm_video_get_time(G.plm->video_decoder) >= video_target_s) break;
            plm_frame_t *vf = plm_decode_video(G.plm);
            if (!vf) break;
            on_video(G.plm, vf, NULL);
        }

        if (G.audio_inited) {
            /* Audio pacing: pull until the ring is mostly full, capped
             * per main-loop iter so a long audio burst can't starve
             * video decode. The original audio_target_s / video PTS
             * gate caused permanent audio lag on Sintel because pl_mpeg
             * never got asked to decode while the video was catching up;
             * gating purely on ring-fullness keeps audio pinned to the
             * I2S consumption rate (44.1 kHz) regardless of video state.
             *
             * Heavy scenes that push CPU above 100% real-time briefly
             * underrun the ring (DMA silence-pads, audible gap) instead
             * of letting audio drift behind video. */
            int max_audio = 5;
            if (G.time_debt > 30) max_audio = 1;
            for (int i = 0; i < max_audio; i++) {
                if (i2s_audio_stream_free() < 1200) break;
                plm_samples_t *as = plm_decode_audio(G.plm);
                if (!as) break;
                on_audio(G.plm, as, NULL);
            }
        }

        if (plm_has_ended(G.plm)) {
            prof_decode_us += (time_us_64() - t0);
            prof_decode_calls++;
            break;
        }

        /* Update time_debt for the on_video skip heuristic. If video PTS
         * is behind wall clock, we are accumulating debt; on_video uses
         * this to drop renders rather than back up further. */
        float vt = plm_video_get_time(G.plm->video_decoder);
        if (vt < wall_pos_s) {
            G.time_debt = (uint32_t)((wall_pos_s - vt) * 1000.0f);
        } else {
            G.time_debt = 0;
        }

        prof_decode_us += (time_us_64() - t0);
        prof_decode_calls++;

        /* Idle wait when caught up: the next decode is pointless until
         * wall clock advances past video PTS, so nap. Without this we
         * would burn CPU spinning while waiting for the next frame slot
         * and waste battery. */
        if (vt > wall_pos_s + 0.005f) {
            uint32_t lead_ms = (uint32_t)((vt - wall_pos_s) * 1000.0f);
            if (lead_ms > 30) lead_ms = 30;
            sleep_ms(lead_ms);
        }

        /* Once-per-second profile dump over USB CDC. Numbers reflect the
         * previous wall-clock second so they correspond directly to what
         * the user just saw. */
        if (now - prof_last_ms >= 1000) {
            uint32_t window_ms = now - prof_last_ms;
            uint32_t dec = G.prof_frames_decoded;
            uint32_t rnd = G.prof_frames_rendered;
            uint32_t skp = G.prof_frames_skipped;
            uint64_t r_us = G.prof_render_us;
            uint64_t a_us = G.prof_audio_cb_us;
            uint32_t a_n  = G.prof_audio_calls;

            uint32_t avg_render_us = rnd ? (uint32_t)(r_us / rnd) : 0;
            uint32_t avg_audio_us  = a_n ? (uint32_t)(a_us / a_n) : 0;
            uint32_t avg_dec_us    = prof_decode_calls
                                   ? (uint32_t)(prof_decode_us / prof_decode_calls)
                                   : 0;
            uint32_t cpu_pct = (uint32_t)((prof_decode_us / 10) / window_ms);
            uint32_t debt_now = G.time_debt;

            uint32_t a_drop = G.prof_audio_dropped;
#ifdef HDMI_HSTX
            uint32_t audio_underruns = hstx_di_queue_get_underrun_count();
#else
            uint32_t audio_underruns = 0;
#endif
            printf("[prof] win=%lums dec=%lu rnd=%lu skp=%lu "
                   "render_avg=%luus audio_avg=%luus(%lu) "
                   "decode_avg=%luus cpu=%lu%% debt=%lums adrop=%lu aundr=%lu\n",
                   (unsigned long)window_ms,
                   (unsigned long)dec, (unsigned long)rnd, (unsigned long)skp,
                   (unsigned long)avg_render_us,
                   (unsigned long)avg_audio_us, (unsigned long)a_n,
                   (unsigned long)avg_dec_us,
                   (unsigned long)cpu_pct,
                   (unsigned long)debt_now,
                   (unsigned long)a_drop,
                   (unsigned long)audio_underruns);
            G.prof_audio_dropped = 0;

            G.prof_frames_decoded = 0;
            G.prof_frames_rendered = 0;
            G.prof_frames_skipped = 0;
            G.prof_render_us = 0;
            G.prof_audio_cb_us = 0;
            G.prof_audio_calls = 0;
            prof_decode_us = 0;
            prof_decode_calls = 0;
            prof_last_ms = now;
        }

        if (plm_has_ended(G.plm)) break;
    }

    /* Cleanup. */
    if (G.audio_inited) i2s_audio_shutdown();
    plm_destroy(G.plm);
    f_close(G.fil); free(G.fil);
    free(G.y_tab); G.y_tab = NULL;
    free(G.dt);    G.dt = NULL;

    /* Roll the PSRAM bump pointer back to where it was before this
     * playback. This reclaims the stream ring and any pl_mpeg internals
     * that landed in PSRAM, so successive plays don't exhaust the perm
     * pool. plm_destroy() above only un-registers the C-level pointers;
     * the underlying PSRAM bytes can't be freed individually with this
     * allocator, so the session-mark mechanism is what actually returns
     * the memory. */
    psram_restore_session();

    /* Clear both framebuffers and put HDMI back on the canonical front
     * pointer so the file browser repaints into the buffer it expects. */
    if (g_framebuffer) memset(g_framebuffer, 0, DISPLAY_W * DISPLAY_H);
    if (g_fb_back)     memset(g_fb_back,     0, DISPLAY_W * DISPLAY_H);
    graphics_set_buffer(g_framebuffer);
}
