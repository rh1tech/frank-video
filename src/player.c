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

#include "pico/stdlib.h"

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

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

/* 6x6x6 colour cube + 40-step grayscale ramp (216-255). */
static void setup_palette(void) {
    for (int r = 0; r < 6; r++)
        for (int g = 0; g < 6; g++)
            for (int b = 0; b < 6; b++) {
                int idx = r * 36 + g * 6 + b;
                uint32_t rgb = ((uint32_t)(r * 51) << 16)
                             | ((uint32_t)(g * 51) << 8)
                             |  (uint32_t)(b * 51);
                graphics_set_palette((uint8_t)idx, rgb);
            }
    for (int i = 0; i < 40; i++) {
        uint8_t v = (uint8_t)(i * 255 / 39);
        uint32_t rgb = ((uint32_t)v << 16) | ((uint32_t)v << 8) | v;
        graphics_set_palette((uint8_t)(216 + i), rgb);
    }
}

/* ===== YCbCr -> RGB332 dither tables ===================================== */

static void init_tables(void) {
    G.y_tab = (uint8_t *)malloc(256);
    for (int i = 0; i < 256; i++) {
        int v = ((i - 16) * 76309) >> 16;
        if (v < 0) v = 0; else if (v > 255) v = 255;
        G.y_tab[i] = (uint8_t)v;
    }

    /* 2x2 Bayer thresholds = step x {0, 0.5, 0.75, 0.25} = {0, 25, 38, 13} */
    int th[4] = {0, 25, 38, 13};

    G.dt = (uint8_t *)malloc(12 * DT_SZ);
    for (int p = 0; p < 4; p++) {
        uint8_t *dr = G.dt + (p * 3 + 0) * DT_SZ;
        uint8_t *dg = G.dt + (p * 3 + 1) * DT_SZ;
        uint8_t *db = G.dt + (p * 3 + 2) * DT_SZ;
        for (int i = 0; i < DT_SZ; i++) {
            int v = i - DT_BIAS;
            if (v < 0) v = 0; if (v > 255) v = 255;

            int rv = (v + th[p]) * 5 / 255; if (rv > 5) rv = 5;
            int gv = (v + th[p]) * 5 / 255; if (gv > 5) gv = 5;
            int bv = (v + th[p]) * 5 / 255; if (bv > 5) bv = 5;
            dr[i] = (uint8_t)(rv * 36);
            dg[i] = (uint8_t)(gv * 6);
            db[i] = (uint8_t)(bv);
        }
    }
}

/* ===== Renderers (kept identical to rhea's videoplayer) ================= */

static void __no_inline_not_in_flash_func(render_1x_rows)(uint8_t *fb, plm_frame_t *frame,
                           int row_start, int row_end) {
    int cols = (int)frame->width >> 1;
    int yw = (int)frame->y.width;
    int cw = (int)frame->cb.width;
    int ox = G.offset_x;
    int oy = G.offset_y;
    const uint8_t *ytab = G.y_tab;

    if (cols > 160) cols = 160;

    const uint8_t *r0 = G.dt +  0*DT_SZ + DT_BIAS;
    const uint8_t *g0 = G.dt +  1*DT_SZ + DT_BIAS;
    const uint8_t *b0 = G.dt +  2*DT_SZ + DT_BIAS;
    const uint8_t *r1 = G.dt +  3*DT_SZ + DT_BIAS;
    const uint8_t *g1 = G.dt +  4*DT_SZ + DT_BIAS;
    const uint8_t *b1 = G.dt +  5*DT_SZ + DT_BIAS;
    const uint8_t *r2 = G.dt +  6*DT_SZ + DT_BIAS;
    const uint8_t *g2 = G.dt +  7*DT_SZ + DT_BIAS;
    const uint8_t *b2 = G.dt +  8*DT_SZ + DT_BIAS;
    const uint8_t *r3 = G.dt +  9*DT_SZ + DT_BIAS;
    const uint8_t *g3 = G.dt + 10*DT_SZ + DT_BIAS;
    const uint8_t *b3 = G.dt + 11*DT_SZ + DT_BIAS;

    uint8_t yl0[LINE_BUF_W], yl1[LINE_BUF_W];
    uint8_t cbl[LINE_BUF_W/2], crl[LINE_BUF_W/2];

    for (int row = row_start; row < row_end; row++) {
        memcpy(yl0, frame->y.data  + row*2*yw,      cols*2);
        memcpy(yl1, frame->y.data  + row*2*yw + yw, cols*2);
        memcpy(cbl, frame->cb.data + row*cw,        cols);
        memcpy(crl, frame->cr.data + row*cw,        cols);

        int di = (oy + row*2) * DISPLAY_W + ox;
        int yi = 0;

        for (int col = 0; col < cols; col++) {
            int cr = crl[col] - 128;
            int cb = cbl[col] - 128;
            int rd = (cr * 104597) >> 16;
            int gd = (cb * 25674 + cr * 53278) >> 16;
            int bd = (cb * 132201) >> 16;
            int yy;

            yy = ytab[yl0[yi]];
            fb[di]     = r0[yy+rd] + g0[yy-gd] + b0[yy+bd];
            yy = ytab[yl0[yi+1]];
            fb[di+1]   = r1[yy+rd] + g1[yy-gd] + b1[yy+bd];
            yy = ytab[yl1[yi]];
            fb[di+DISPLAY_W]   = r2[yy+rd] + g2[yy-gd] + b2[yy+bd];
            yy = ytab[yl1[yi+1]];
            fb[di+DISPLAY_W+1] = r3[yy+rd] + g3[yy-gd] + b3[yy+bd];

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

    const uint8_t *rp[4], *gp[4], *bp[4];
    for (int p = 0; p < 4; p++) {
        rp[p] = G.dt + (p*3+0)*DT_SZ + DT_BIAS;
        gp[p] = G.dt + (p*3+1)*DT_SZ + DT_BIAS;
        bp[p] = G.dt + (p*3+2)*DT_SZ + DT_BIAS;
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
            int max_audio = 4;
            if (G.time_debt > 30) max_audio = 1;
            float audio_target_s = plm_video_get_time(G.plm->video_decoder)
                                 + 0.05f;
            for (int i = 0; i < max_audio; i++) {
                if (plm_audio_get_time(G.plm->audio_decoder) >= audio_target_s) break;
                if (i2s_audio_stream_free() < 1500) break;
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
            printf("[prof] win=%lums dec=%lu rnd=%lu skp=%lu "
                   "render_avg=%luus audio_avg=%luus(%lu) "
                   "decode_avg=%luus cpu=%lu%% debt=%lums adrop=%lu\n",
                   (unsigned long)window_ms,
                   (unsigned long)dec, (unsigned long)rnd, (unsigned long)skp,
                   (unsigned long)avg_render_us,
                   (unsigned long)avg_audio_us, (unsigned long)a_n,
                   (unsigned long)avg_dec_us,
                   (unsigned long)cpu_pct,
                   (unsigned long)debt_now,
                   (unsigned long)a_drop);
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
    /* Clear both framebuffers and put HDMI back on the canonical front
     * pointer so the file browser repaints into the buffer it expects. */
    if (g_framebuffer) memset(g_framebuffer, 0, DISPLAY_W * DISPLAY_H);
    if (g_fb_back)     memset(g_fb_back,     0, DISPLAY_W * DISPLAY_H);
    graphics_set_buffer(g_framebuffer);
}
