/*
 * FRANK NES - I2S Audio Driver
 * Chained double-buffer DMA via PIO0
 * Adapted from murmgenesis audio driver.
 * SPDX-License-Identifier: MIT
 */

#include "i2s_audio.h"
#include "audio_i2s.pio.h"

#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/dma.h"
#include "hardware/clocks.h"
#include "hardware/sync.h"
#include "hardware/irq.h"
#include "hardware/resets.h"
#include "hardware/pio.h"

#include <string.h>
#include <stdio.h>

/* HDMI HSTX driver uses DMA_IRQ_0; DispHSTX VGA driver uses DMA_IRQ_1.
 * Pick the other one so audio I2S never clashes. */
#if defined(VGA_HSTX)
#define AUDIO_DMA_IRQ   DMA_IRQ_0
#else
#define AUDIO_DMA_IRQ   DMA_IRQ_1
#endif
#define AUDIO_DMA_CH_A  10
#define AUDIO_DMA_CH_B  11

/* DMA buffer: 1120 stereo frames covers NTSC (735) + headroom */
#define DMA_BUFFER_SAMPLES 1120
#define DMA_BUFFER_COUNT   2
#define PREROLL_BUFFERS    2

static uint32_t __attribute__((aligned(4))) dma_bufs[DMA_BUFFER_COUNT][DMA_BUFFER_SAMPLES];

static volatile uint32_t bufs_free_mask = 0;
static volatile int preroll_count = 0;
static volatile bool i2s_running = false;

/* Software ring buffer for stream-mode push. Decouples producer's chunk
 * size (e.g. MPEG-1's 1152 frames) from the DMA chunk size, so we never
 * silence-pad mid-chunk. Sized to ~370 ms @ 44.1kHz (16384 stereo frames):
 * pl_mpeg delivers up to ~200ms of buffered audio in bursts whenever the
 * decode loop catches up after a slow frame, so we need at least double
 * that to absorb without dropping. */
#define RING_FRAMES 16384
#define RING_MASK   (RING_FRAMES - 1)
static uint32_t __attribute__((aligned(4))) ring_buf[RING_FRAMES];
static volatile uint32_t ring_w = 0;  /* written by producer    */
static volatile uint32_t ring_r = 0;  /* read by DMA IRQ refill */

static PIO i2s_pio;
static uint i2s_sm;
static uint i2s_offset;
static uint i2s_data_pin;
static uint i2s_clock_base;
static bool i2s_claimed = false;
static volatile uint32_t dma_xfer_count;
static uint32_t cached_sample_rate = 0;

/* Stream-mode is engaged once i2s_audio_stream_push is called; it switches
 * the IRQ to refill DMA buffers from the software ring instead of just
 * re-arming the static ping-pong. */
static volatile bool stream_mode = false;

/* Drain up to dma_xfer_count stereo frames from the ring into dst, padding
 * any tail with silence. Indexes are wrapped mod RING_FRAMES with one slot
 * reserved (effective capacity = RING_FRAMES - 1) so empty (w == r) is
 * distinct from full (((w + 1) & RING_MASK) == r). Called from DMA IRQ. */
static inline void ring_drain_to(uint32_t *dst) {
    uint32_t r = ring_r;
    uint32_t w = ring_w;
    uint32_t avail = (w - r) & RING_MASK;        /* 0 .. RING_FRAMES-1 */
    uint32_t n = dma_xfer_count;
    uint32_t take = avail < n ? avail : n;

    for (uint32_t i = 0; i < take; i++)
        dst[i] = ring_buf[(r + i) & RING_MASK];
    /* Underrun-pad with silence -- happens before the producer's first
     * push and whenever decode briefly falls behind realtime. */
    for (uint32_t i = take; i < n; i++) dst[i] = 0;

    ring_r = (r + take) & RING_MASK;
}

static void i2s_dma_irq_handler(void) {
#if defined(VGA_HSTX)
    uint32_t ints = dma_hw->ints0;
#else
    uint32_t ints = dma_hw->ints1;
#endif
    uint32_t mask = (1u << AUDIO_DMA_CH_A) | (1u << AUDIO_DMA_CH_B);
    ints &= mask;
    if (!ints) return;

    if (ints & (1u << AUDIO_DMA_CH_A)) {
#if defined(VGA_HSTX)
        dma_hw->ints0 = (1u << AUDIO_DMA_CH_A);
#else
        dma_hw->ints1 = (1u << AUDIO_DMA_CH_A);
#endif
        if (stream_mode) ring_drain_to(dma_bufs[0]);
        dma_channel_set_read_addr(AUDIO_DMA_CH_A, dma_bufs[0], false);
        dma_channel_set_trans_count(AUDIO_DMA_CH_A, dma_xfer_count, false);
        bufs_free_mask |= 1u;
    }
    if (ints & (1u << AUDIO_DMA_CH_B)) {
#if defined(VGA_HSTX)
        dma_hw->ints0 = (1u << AUDIO_DMA_CH_B);
#else
        dma_hw->ints1 = (1u << AUDIO_DMA_CH_B);
#endif
        if (stream_mode) ring_drain_to(dma_bufs[1]);
        dma_channel_set_read_addr(AUDIO_DMA_CH_B, dma_bufs[1], false);
        dma_channel_set_trans_count(AUDIO_DMA_CH_B, dma_xfer_count, false);
        bufs_free_mask |= 2u;
    }
}

void i2s_audio_init(uint data_pin, uint clock_pin_base, uint32_t sample_rate) {
    printf("I2S: init data=%u clk=%u rate=%lu\n", data_pin, clock_pin_base, (unsigned long)sample_rate);

#if defined(VIDEO_COMPOSITE) || defined(HDMI_PIO)
    /* Composite TV uses PIO0; I2S must use PIO1 */
    i2s_pio = pio1;
#else
    i2s_pio = pio0;
#endif
    cached_sample_rate = sample_rate;
    /* Default to NTSC pacing (60 fps). Call i2s_audio_set_frame_rate(50)
     * after init if the active ROM runs at Dendy timing. Using the wrong
     * divisor makes the PIO drain slower than the emulator produces,
     * throttling frame rate and injecting per-frame silence pads — audio
     * ends up slowed and buzzy. */
    dma_xfer_count = sample_rate / 60;
    if (dma_xfer_count > DMA_BUFFER_SAMPLES) dma_xfer_count = DMA_BUFFER_SAMPLES;

    /* Configure GPIO for PIO */
#if defined(VIDEO_COMPOSITE) || defined(HDMI_PIO)
    gpio_set_function(data_pin, GPIO_FUNC_PIO1);
    gpio_set_function(clock_pin_base, GPIO_FUNC_PIO1);
    gpio_set_function(clock_pin_base + 1, GPIO_FUNC_PIO1);
#else
    gpio_set_function(data_pin, GPIO_FUNC_PIO0);
    gpio_set_function(clock_pin_base, GPIO_FUNC_PIO0);
    gpio_set_function(clock_pin_base + 1, GPIO_FUNC_PIO0);
#endif
    gpio_set_drive_strength(data_pin, GPIO_DRIVE_STRENGTH_12MA);
    gpio_set_drive_strength(clock_pin_base, GPIO_DRIVE_STRENGTH_12MA);
    gpio_set_drive_strength(clock_pin_base + 1, GPIO_DRIVE_STRENGTH_12MA);

    /* Claim a PIO0 state machine */
    i2s_sm = pio_claim_unused_sm(i2s_pio, true);
    i2s_claimed = true;
    i2s_data_pin = data_pin;
    i2s_clock_base = clock_pin_base;
    printf("I2S: PIO0 SM%u\n", i2s_sm);

    /* Load PIO program */
    i2s_offset = pio_add_program(i2s_pio, &audio_i2s_program);
    audio_i2s_program_init(i2s_pio, i2s_sm, i2s_offset, data_pin, clock_pin_base);
    pio_sm_clear_fifos(i2s_pio, i2s_sm);

    /* Clock divider: sys_clk * 4 / sample_rate (PIO outputs 1 bit per 2 clocks,
     * 32 bits per stereo frame = 64 PIO clocks per frame) */
    uint32_t sys_clk = clock_get_hz(clk_sys);
    uint32_t divider = sys_clk * 4 / sample_rate;
    pio_sm_set_clkdiv_int_frac(i2s_pio, i2s_sm, divider >> 8u, divider & 0xffu);

    /* Init DMA buffers with silence */
    memset(dma_bufs, 0, sizeof(dma_bufs));

    /* Abort and claim fixed DMA channels */
    dma_channel_abort(AUDIO_DMA_CH_A);
    dma_channel_abort(AUDIO_DMA_CH_B);
    while (dma_channel_is_busy(AUDIO_DMA_CH_A) || dma_channel_is_busy(AUDIO_DMA_CH_B))
        tight_loop_contents();
    dma_channel_unclaim(AUDIO_DMA_CH_A);
    dma_channel_unclaim(AUDIO_DMA_CH_B);
    dma_channel_claim(AUDIO_DMA_CH_A);
    dma_channel_claim(AUDIO_DMA_CH_B);

    /* Configure ping-pong DMA chain */
    dma_channel_config cfg_a = dma_channel_get_default_config(AUDIO_DMA_CH_A);
    channel_config_set_read_increment(&cfg_a, true);
    channel_config_set_write_increment(&cfg_a, false);
    channel_config_set_transfer_data_size(&cfg_a, DMA_SIZE_32);
    channel_config_set_dreq(&cfg_a, pio_get_dreq(i2s_pio, i2s_sm, true));
    channel_config_set_chain_to(&cfg_a, AUDIO_DMA_CH_B);

    dma_channel_config cfg_b = dma_channel_get_default_config(AUDIO_DMA_CH_B);
    channel_config_set_read_increment(&cfg_b, true);
    channel_config_set_write_increment(&cfg_b, false);
    channel_config_set_transfer_data_size(&cfg_b, DMA_SIZE_32);
    channel_config_set_dreq(&cfg_b, pio_get_dreq(i2s_pio, i2s_sm, true));
    channel_config_set_chain_to(&cfg_b, AUDIO_DMA_CH_A);

    dma_channel_configure(AUDIO_DMA_CH_A, &cfg_a, &i2s_pio->txf[i2s_sm],
                          dma_bufs[0], dma_xfer_count, false);
    dma_channel_configure(AUDIO_DMA_CH_B, &cfg_b, &i2s_pio->txf[i2s_sm],
                          dma_bufs[1], dma_xfer_count, false);

    /* DMA IRQ handler — route DMA channels to whichever of IRQ0/IRQ1
     * isn't claimed by the video driver (IRQ_0 for HDMI HSTX, IRQ_1 for
     * DispHSTX VGA). AUDIO_DMA_IRQ is selected above. */
#if defined(VGA_HSTX)
    dma_hw->ints0 = (1u << AUDIO_DMA_CH_A) | (1u << AUDIO_DMA_CH_B);
#else
    dma_hw->ints1 = (1u << AUDIO_DMA_CH_A) | (1u << AUDIO_DMA_CH_B);
#endif
    irq_set_exclusive_handler(AUDIO_DMA_IRQ, i2s_dma_irq_handler);
    irq_set_priority(AUDIO_DMA_IRQ, 0x80);
    irq_set_enabled(AUDIO_DMA_IRQ, true);
#if defined(VGA_HSTX)
    dma_channel_set_irq0_enabled(AUDIO_DMA_CH_A, true);
    dma_channel_set_irq0_enabled(AUDIO_DMA_CH_B, true);
#else
    dma_channel_set_irq1_enabled(AUDIO_DMA_CH_A, true);
    dma_channel_set_irq1_enabled(AUDIO_DMA_CH_B, true);
#endif

    /* Enable PIO state machine */
    pio_sm_set_enabled(i2s_pio, i2s_sm, true);

    preroll_count = 0;
    bufs_free_mask = (1u << DMA_BUFFER_COUNT) - 1u;
    i2s_running = false;

    /* Reset streaming-mode state. Without this the second time we init
     * the driver (e.g. after returning to the file browser and starting
     * a new clip), stream_mode would still be true from the previous
     * playback and i2s_audio_stream_push would skip the DMA start path,
     * leaving DMA disarmed -> silent playback. */
    stream_mode = false;
    ring_w = ring_r = 0;

    printf("I2S: ready (double-buffer DMA, %lu frames/buf)\n", (unsigned long)dma_xfer_count);
}

/* Claim a free DMA buffer, copy samples (mono→stereo), pad with silence, start DMA if preroll done */
static void i2s_write_buf(const int16_t *stereo_buf, uint32_t frame_count) {
    if (frame_count > dma_xfer_count) frame_count = dma_xfer_count;
    if (frame_count == 0) frame_count = 1;

    uint8_t idx = 0;
    while (true) {
        uint32_t irq_state = save_and_disable_interrupts();
        uint32_t free = bufs_free_mask;
        if (!i2s_running) {
            idx = (uint8_t)preroll_count;
            if (idx < DMA_BUFFER_COUNT && (free & (1u << idx))) {
                bufs_free_mask &= ~(1u << idx);
                restore_interrupts(irq_state);
                break;
            }
        } else if (free) {
            idx = (free & 1u) ? 0 : 1;
            bufs_free_mask &= ~(1u << idx);
            restore_interrupts(irq_state);
            break;
        }
        restore_interrupts(irq_state);
        tight_loop_contents();
    }

    /* Copy stereo packed frames */
    memcpy(dma_bufs[idx], stereo_buf, frame_count * sizeof(uint32_t));

    /* Pad remainder with silence */
    if (frame_count < dma_xfer_count)
        memset(&dma_bufs[idx][frame_count], 0, (dma_xfer_count - frame_count) * sizeof(uint32_t));

    __dmb();

    if (!i2s_running) {
        preroll_count++;
        if (preroll_count >= PREROLL_BUFFERS) {
            dma_channel_start(AUDIO_DMA_CH_A);
            i2s_running = true;
        }
    }
}

void i2s_audio_push_samples(const int16_t *buf, int count) {
    /* Convert mono to packed stereo (L in upper 16, R in lower 16) and write
     * in chunks of dma_xfer_count.
     * Attenuate by 2 bits (÷4) to avoid clipping on external DACs — NES audio
     * is very hot and I2S goes straight to the amplifier with no level control. */
    static uint32_t __attribute__((aligned(4))) pack_buf[DMA_BUFFER_SAMPLES];
    int pos = 0;

    while (pos < count) {
        int chunk = count - pos;
        if (chunk > (int)dma_xfer_count) chunk = (int)dma_xfer_count;

        for (int i = 0; i < chunk; i++) {
            int16_t attenuated = buf[pos + i] >> 3;
            uint16_t s = (uint16_t)attenuated;
            pack_buf[i] = ((uint32_t)s << 16) | s;
        }

        i2s_write_buf((const int16_t *)pack_buf, (uint32_t)chunk);
        pos += chunk;
    }
}

void i2s_audio_push_stereo(const int16_t *buf, int frames) {
    /* MPEG-1 audio is interleaved stereo. Pack to (L<<16)|R words and push
     * in chunks of dma_xfer_count, with the same attenuation as the mono
     * path to keep headroom on external DACs. */
    static uint32_t __attribute__((aligned(4))) pack_buf[DMA_BUFFER_SAMPLES];
    int pos = 0;
    while (pos < frames) {
        int chunk = frames - pos;
        if (chunk > (int)dma_xfer_count) chunk = (int)dma_xfer_count;

        const int16_t *p = buf + pos * 2;
        for (int i = 0; i < chunk; i++) {
            uint16_t l = (uint16_t)(p[i * 2 + 0] >> 3);
            uint16_t r = (uint16_t)(p[i * 2 + 1] >> 3);
            pack_buf[i] = ((uint32_t)l << 16) | r;
        }
        i2s_write_buf((const int16_t *)pack_buf, (uint32_t)chunk);
        pos += chunk;
    }
}

/* Non-blocking single-buffer try-write. Returns 1 on enqueue, 0 if both
 * DMA buffers are still busy (caller should drop or retry later). */
static int i2s_try_write_buf(const int16_t *stereo_buf, uint32_t frame_count) {
    if (frame_count > dma_xfer_count) frame_count = dma_xfer_count;
    if (frame_count == 0) return 1;

    uint32_t irq_state = save_and_disable_interrupts();
    uint32_t free = bufs_free_mask;
    uint8_t  idx;

    if (!i2s_running) {
        idx = (uint8_t)preroll_count;
        if (idx >= DMA_BUFFER_COUNT || !(free & (1u << idx))) {
            restore_interrupts(irq_state);
            return 0;
        }
    } else {
        if (!free) { restore_interrupts(irq_state); return 0; }
        idx = (free & 1u) ? 0 : 1;
    }
    bufs_free_mask &= ~(1u << idx);
    restore_interrupts(irq_state);

    memcpy(dma_bufs[idx], stereo_buf, frame_count * sizeof(uint32_t));
    if (frame_count < dma_xfer_count)
        memset(&dma_bufs[idx][frame_count], 0,
               (dma_xfer_count - frame_count) * sizeof(uint32_t));

    __dmb();

    if (!i2s_running) {
        preroll_count++;
        if (preroll_count >= PREROLL_BUFFERS) {
            dma_channel_start(AUDIO_DMA_CH_A);
            i2s_running = true;
        }
    }
    return 1;
}

int i2s_audio_try_push_stereo(const int16_t *buf, int frames) {
    /* Drop-on-overrun stereo push. Same packing/attenuation as the
     * blocking variant; returns 0 if any chunk had to be dropped so the
     * caller can keep accounting if it cares. */
    static uint32_t __attribute__((aligned(4))) pack_buf[DMA_BUFFER_SAMPLES];
    int pos = 0;
    int ok = 1;
    while (pos < frames) {
        int chunk = frames - pos;
        if (chunk > (int)dma_xfer_count) chunk = (int)dma_xfer_count;

        const int16_t *p = buf + pos * 2;
        for (int i = 0; i < chunk; i++) {
            uint16_t l = (uint16_t)(p[i * 2 + 0] >> 3);
            uint16_t r = (uint16_t)(p[i * 2 + 1] >> 3);
            pack_buf[i] = ((uint32_t)l << 16) | r;
        }
        if (!i2s_try_write_buf((const int16_t *)pack_buf, (uint32_t)chunk)) {
            ok = 0;
            /* Skip the remainder of this audio frame instead of pretending
             * we delivered it -- audio glitches are preferable to spending
             * a whole frame's worth of time blocked here. */
            break;
        }
        pos += chunk;
    }
    return ok;
}

/* Stream-mode push: pack stereo frames and append into the ring buffer;
 * the DMA IRQ refills DMA chunks from the ring, so producer chunk size is
 * decoupled from DMA chunk size and there is no silence padding. */
int i2s_audio_stream_push(const int16_t *interleaved, int frames) {
    if (frames <= 0) return 0;

    /* On first call, switch to stream mode and prime DMA with two zeroed
     * chunks before kicking it off, so the very first DMA buffer doesn't
     * race against an empty ring. */
    if (!stream_mode) {
        memset(dma_bufs, 0, sizeof(dma_bufs));
        ring_r = ring_w = 0;
        stream_mode = true;
        __dmb();
        if (!i2s_running) {
            bufs_free_mask &= ~(1u | 2u);
            __dmb();
            dma_channel_start(AUDIO_DMA_CH_A);
            i2s_running = true;
        }
    }

    int written = 0;
    for (int i = 0; i < frames; i++) {
        uint32_t w = ring_w;
        uint32_t r = ring_r;
        uint32_t next = (w + 1) & RING_MASK;
        if (next == r) break;  /* ring full -- drop remainder */

        uint16_t l = (uint16_t)(interleaved[i * 2 + 0] >> 3);
        uint16_t rt = (uint16_t)(interleaved[i * 2 + 1] >> 3);
        ring_buf[w] = ((uint32_t)l << 16) | rt;
        ring_w = next;
        written++;
    }
    __dmb();
    return written;
}

int i2s_audio_stream_free(void) {
    uint32_t w = ring_w;
    uint32_t r = ring_r;
    uint32_t used = (w - r) & RING_MASK;
    return (int)(RING_FRAMES - 1 - used);
}

void i2s_audio_fill_silence(int count) {
    static uint32_t __attribute__((aligned(4))) silence[DMA_BUFFER_SAMPLES];
    memset(silence, 0, dma_xfer_count * sizeof(uint32_t));

    int remaining = count;
    while (remaining > 0) {
        int chunk = remaining;
        if (chunk > (int)dma_xfer_count) chunk = (int)dma_xfer_count;
        i2s_write_buf((const int16_t *)silence, (uint32_t)chunk);
        remaining -= chunk;
    }
}

void i2s_audio_set_frame_rate(int frame_rate) {
    if (frame_rate <= 0) return;
    if (cached_sample_rate == 0) return; /* init hasn't run yet */
    uint32_t n = cached_sample_rate / (uint32_t)frame_rate;
    if (n < 1) n = 1;
    if (n > DMA_BUFFER_SAMPLES) n = DMA_BUFFER_SAMPLES;
    /* Single-word write is atomic on Cortex-M33; the DMA IRQ handler
     * reads dma_xfer_count to program the next chain-to transfer, so the
     * new value takes effect within one buffer. */
    dma_xfer_count = n;
}

void i2s_audio_shutdown(void) {
    if (!i2s_claimed) return;
    i2s_running = false;
    pio_sm_set_enabled(i2s_pio, i2s_sm, false);
    irq_set_enabled(AUDIO_DMA_IRQ, false);

#if defined(VGA_HSTX)
    dma_channel_set_irq0_enabled(AUDIO_DMA_CH_A, false);
    dma_channel_abort(AUDIO_DMA_CH_A);
    dma_hw->ints0 = (1u << AUDIO_DMA_CH_A);
    dma_channel_unclaim(AUDIO_DMA_CH_A);

    dma_channel_set_irq0_enabled(AUDIO_DMA_CH_B, false);
    dma_channel_abort(AUDIO_DMA_CH_B);
    dma_hw->ints0 = (1u << AUDIO_DMA_CH_B);
    dma_channel_unclaim(AUDIO_DMA_CH_B);
#else
    dma_channel_set_irq1_enabled(AUDIO_DMA_CH_A, false);
    dma_channel_abort(AUDIO_DMA_CH_A);
    dma_hw->ints1 = (1u << AUDIO_DMA_CH_A);
    dma_channel_unclaim(AUDIO_DMA_CH_A);

    dma_channel_set_irq1_enabled(AUDIO_DMA_CH_B, false);
    dma_channel_abort(AUDIO_DMA_CH_B);
    dma_hw->ints1 = (1u << AUDIO_DMA_CH_B);
    dma_channel_unclaim(AUDIO_DMA_CH_B);
#endif

    bufs_free_mask = (1u << DMA_BUFFER_COUNT) - 1u;
    preroll_count = 0;

    /* Streaming-mode state must reset alongside the DMA state -- otherwise
     * the next i2s_audio_init() leaves stream_mode=true from this run,
     * i2s_audio_stream_push() skips the DMA-start path, and audio is
     * silent on the next playback. (init also resets these for belt and
     * suspenders, but doing it here keeps the driver in a consistent
     * post-shutdown state regardless of the caller.) */
    stream_mode = false;
    ring_w = ring_r = 0;

    /* Release PIO program + SM so the driver can re-init on different pins
     * (e.g. switching between onboard I2S and an external DAC at runtime). */
    pio_remove_program(i2s_pio, &audio_i2s_program, i2s_offset);
    pio_sm_unclaim(i2s_pio, i2s_sm);
    /* Hand GPIOs back to SIO so the new init's gpio_set_function call can
     * cleanly reassign them (to either PIO or I2C). */
    gpio_set_function(i2s_data_pin,      GPIO_FUNC_SIO);
    gpio_set_function(i2s_clock_base,    GPIO_FUNC_SIO);
    gpio_set_function(i2s_clock_base + 1, GPIO_FUNC_SIO);
    i2s_claimed = false;
}
