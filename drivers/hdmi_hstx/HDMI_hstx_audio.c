/*
 * frank-video -- HDMI audio (over HSTX) shim that re-uses the I2S audio
 * API surface the player and the rest of the codebase already calls
 * (i2s_audio_init / i2s_audio_stream_push / i2s_audio_set_frame_rate /
 *  i2s_audio_stream_free / i2s_audio_fill_silence / i2s_audio_shutdown).
 *
 * Instead of feeding samples to PIO + DMA pins, we encode them into HDMI
 * Data Island packets (HDMI 1.3a section 5.3) and push them onto the
 * shared queue consumed by the HSTX scanout DMA ISR. The video_output
 * driver paces emission so the receiver gets one packet every 4 lines,
 * which at our 60 Hz / 262-line mode lands close to the target sample
 * rate without needing any host-side timing.
 *
 * This file replaces drivers/i2s/i2s_audio.c when -DHDMI_HSTX=ON. The PIO
 * audio I2S driver is still available but unused on this build.
 */

#include "i2s_audio.h"
#include "hstx_packet.h"
#include "hstx_data_island_queue.h"
#include "video_output.h"

#include "pico/stdlib.h"

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/* Audio frame counter is part of the HDMI sample-frame sequencing the
 * receiver uses to deinterleave; bump it by 4 per emitted packet. */
static int g_frame_counter = 0;
static uint32_t g_sample_rate = 44100;
static bool g_inited = false;

/* =========================================================================
 * Public API.
 * =========================================================================*/

void i2s_audio_init(unsigned int data_pin, unsigned int clock_pin_base, uint32_t sample_rate) {
    (void)data_pin;
    (void)clock_pin_base;

    g_sample_rate = sample_rate ? sample_rate : 44100;

    /* Reset the data-island scheduler and reload the silence fallback so
     * a re-init mid-program (between two clips) doesn't carry stale state. */
    hstx_di_queue_init();

    /* Tell the HSTX driver the new audio rate so the ACR + AudioInfoFrame
     * packets it inserts in vblank match what we're feeding. */
    pico_hdmi_set_audio_sample_rate(g_sample_rate);

    g_frame_counter = 0;
    g_inited = true;
}

void i2s_audio_set_frame_rate(int frame_rate) {
    /* The PIO i2s driver uses this to resize the per-DMA chunk so the
     * consumer drains at the right pace. The HSTX queue paces itself
     * via hstx_di_queue_tick(), so this is a no-op on the HDMI path. */
    (void)frame_rate;
}

/* Stereo (interleaved) push: feed pl_mpeg's samples into HDMI data islands
 * 4 stereo frames at a time. Returns frames accepted so the producer can
 * see backpressure when the queue is full. */
static int __not_in_flash_func(hdmi_audio_push_internal)(const int16_t *interleaved, int frames) {
    if (!g_inited || frames <= 0) return 0;

    int written = 0;
    while (written + 4 <= frames) {
        audio_sample_t s[4];
        const int16_t *p = interleaved + written * 2;
        for (int i = 0; i < 4; i++) {
            s[i].left  = p[2 * i + 0];
            s[i].right = p[2 * i + 1];
        }
        hstx_packet_t packet;
        int new_fc = hstx_packet_set_audio_samples(&packet, s, 4, g_frame_counter);
        hstx_data_island_t island;
        hstx_encode_data_island(&island, &packet, false, true);
        if (!hstx_di_queue_push(&island)) break;  /* ring full */
        g_frame_counter = new_fc;
        written += 4;
    }

    /* Refresh the silence fallback so an eventual underrun emits packets
     * with an in-sequence frame counter (otherwise the receiver mutes). */
    hstx_di_queue_update_silence(g_frame_counter);
    return written;
}

void i2s_audio_push_samples(const int16_t *buf, int count) {
    /* Mono -> stereo duplication. */
    if (count <= 0) return;
    /* Pack 64-frame bursts on the stack to keep heap pressure off the
     * audio ISR path; loop until everything is queued or the queue refuses
     * to accept further frames. */
    int16_t st[128];
    int frames_left = count;
    const int16_t *src = buf;
    while (frames_left > 0) {
        int n = frames_left < 64 ? frames_left : 64;
        for (int i = 0; i < n; i++) {
            st[2 * i + 0] = src[i];
            st[2 * i + 1] = src[i];
        }
        int written = hdmi_audio_push_internal(st, n);
        if (written <= 0) break;
        src += written;
        frames_left -= written;
    }
}

void i2s_audio_push_stereo(const int16_t *buf, int frames) {
    int frames_left = frames;
    const int16_t *src = buf;
    while (frames_left > 0) {
        int written = hdmi_audio_push_internal(src, frames_left);
        if (written <= 0) break;
        src += written * 2;
        frames_left -= written;
    }
}

int i2s_audio_try_push_stereo(const int16_t *buf, int frames) {
    int written = hdmi_audio_push_internal(buf, frames);
    return (written == frames) ? 1 : 0;
}

int i2s_audio_stream_push(const int16_t *interleaved, int frames) {
    /* The producer is pl_mpeg with a 1152-frame burst. Round down to
     * a multiple of 4 since each HDMI data island carries exactly 4
     * sample frames; the leftover (0..3 frames) is dropped to keep
     * frame_counter sequencing intact. The chance of any dropped frame
     * surviving the ear at 44.1 kHz over a 1152-frame burst is nil. */
    if (frames <= 0) return 0;
    int aligned = frames & ~3;
    if (aligned == 0) return 0;
    return hdmi_audio_push_internal(interleaved, aligned);
}

int i2s_audio_stream_free(void) {
    /* HSTX DI queue holds DI_RING_BUFFER_SIZE packets (must match the
     * value in hstx_data_island_queue.c). Each packet = 4 stereo frames.
     * Report free space in stereo frames so the producer-pacing rule in
     * player.c (`>= 1200 free`) keeps working unchanged. */
    const int capacity_packets = 1024;
    int level = (int)hstx_di_queue_get_level();
    int free_packets = capacity_packets - level - 1;  /* keep one slot empty */
    if (free_packets < 0) free_packets = 0;
    return free_packets * 4;
}

void i2s_audio_fill_silence(int count) {
    /* Push N silent stereo frames so the data-island stream doesn't go
     * dry in the receiver's view. Aligned to 4-frame packets. */
    if (count <= 0) return;
    audio_sample_t s[4] = {0};
    int packets = (count + 3) / 4;
    for (int i = 0; i < packets; i++) {
        hstx_packet_t packet;
        g_frame_counter = hstx_packet_set_audio_samples(&packet, s, 4, g_frame_counter);
        hstx_data_island_t island;
        hstx_encode_data_island(&island, &packet, false, true);
        if (!hstx_di_queue_push(&island)) break;
    }
    hstx_di_queue_update_silence(g_frame_counter);
}

void i2s_audio_shutdown(void) {
    /* Nothing to physically tear down -- the HSTX scanout owns the HDMI
     * pins and stays running for the lifetime of the program. Zero the
     * silence packet's frame counter so the next i2s_audio_init() starts
     * cleanly. */
    g_inited = false;
    hstx_di_queue_update_silence(0);
}

/* Track how many silence packets the HSTX consumer has emitted between
 * calls so the producer can drop the equivalent number of stereo frames
 * and stay aligned with wall clock. Each silence packet is 4 frames. */
static uint32_t g_last_underrun_count = 0;

uint32_t i2s_audio_consume_underrun_frames(void) {
    uint32_t cur = hstx_di_queue_get_underrun_count();
    uint32_t delta = cur - g_last_underrun_count;
    g_last_underrun_count = cur;
    return delta * 4;
}
