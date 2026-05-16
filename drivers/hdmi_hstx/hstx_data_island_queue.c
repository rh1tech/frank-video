/*
 * FRANK NES - NES Emulator for RP2350
 * Originally from pico_hdmi by fliperama86 (https://github.com/fliperama86/pico_hdmi)
 * https://rh1.tech | https://github.com/rh1tech/frank-nes
 * SPDX-License-Identifier: Unlicense
 */

#include "hstx_data_island_queue.h"

#include <string.h>

#include "pico.h"
#include "hardware/sync.h"

#define DI_RING_BUFFER_SIZE 1024
static hstx_data_island_t di_ring_buffer[DI_RING_BUFFER_SIZE];
static volatile uint32_t di_ring_head = 0;
static volatile uint32_t di_ring_tail = 0;

// Single pre-encoded silent audio packet (fixed B-frame flags).
static hstx_data_island_t silence_packet;

// Audio timing state (default 48kHz, 525 lines for 480p).
//
// Scheme: each scanline tick adds sample_rate_hz_x60 (== sample_rate_hz)
// to the accumulator; threshold is (4 * v_total * 60). That keeps the
// math exact for any integer sample rate including 32 kHz, where the
// older "samples_per_frame = rate / 60" formula truncated 32000/60 to
// 533 and emitted at 31980 Hz (audio fell behind video by ~20 Hz over
// the whole stream — a 95-min film drifted by ~3.5 s).
static uint32_t audio_sample_accum = 0;
static uint32_t cached_v_total_lines = 525;
#define DEFAULT_SAMPLE_RATE_HZ 48000u
static uint32_t sample_rate_hz = DEFAULT_SAMPLE_RATE_HZ;

// Bound the accumulator so a temporary stall doesn't overflow. ~8 frames
// worth of slack lets the consumer catch up after a brief queue dry-up.
static uint32_t max_audio_accum = (8u * 525u * 60u);

/* Debug: track silence fallback hits. Check via hstx_di_queue_get_underrun_count() */
static volatile uint32_t audio_underrun_count = 0;

void hstx_di_queue_init(void)
{
    di_ring_head = 0;
    di_ring_tail = 0;
    audio_sample_accum = 0;
    audio_underrun_count = 0;
    // Build a single silent audio packet.
    hstx_packet_t packet;
    audio_sample_t samples[4] = {0};
    (void)hstx_packet_set_audio_samples(&packet, samples, 4, 0);
    hstx_encode_data_island(&silence_packet, &packet, false, true);
}

void hstx_di_queue_set_sample_rate(uint32_t sample_rate)
{
    sample_rate_hz = sample_rate ? sample_rate : DEFAULT_SAMPLE_RATE_HZ;
}

void hstx_di_queue_set_v_total(uint32_t v_total)
{
    cached_v_total_lines = v_total;
    max_audio_accum = 8u * v_total * 60u;
}

void hstx_di_queue_set_samples_per_line_fp(uint32_t value)
{
    // Legacy: value is samples-per-line in 16.16 fixed point, so the
    // implied per-second rate is value * v_total * 60 / 65536. Convert
    // straight to Hz.
    sample_rate_hz = (uint32_t)(((uint64_t)value * cached_v_total_lines * 60u) >> 16);
}

bool __not_in_flash("audio") hstx_di_queue_push(const hstx_data_island_t *island)
{
    uint32_t next_head = (di_ring_head + 1) % DI_RING_BUFFER_SIZE;
    if (next_head == di_ring_tail)
        return false;

    di_ring_buffer[di_ring_head] = *island;
    di_ring_head = next_head;
    return true;
}

uint32_t __not_in_flash("audio") hstx_di_queue_get_level(void)
{
    uint32_t head = di_ring_head;
    uint32_t tail = di_ring_tail;
    if (head >= tail)
        return head - tail;
    return DI_RING_BUFFER_SIZE + head - tail;
}

void __scratch_x("") hstx_di_queue_tick(void)
{
    /* One scanline tick: produced samples in 1/(60*v_total) seconds
     * = sample_rate / (60 * v_total). Keeping the denominator implicit
     * (in the threshold below) and adding sample_rate per tick gives
     * exact arithmetic for any integer sample rate. */
    audio_sample_accum += sample_rate_hz;
    if (audio_sample_accum > max_audio_accum) {
        audio_sample_accum = max_audio_accum;
    }
}

uint32_t hstx_di_queue_get_underrun_count(void) { return audio_underrun_count; }

const uint32_t *__scratch_x("") hstx_di_queue_get_audio_packet(void)
{
    /* Threshold: four samples (one packet) costs 4 / sample_rate seconds.
     * In the units used by the accumulator (sample_rate per scanline of
     * 1/(60*v_total) seconds) that is exactly 4 * 60 * v_total. */
    uint32_t threshold = 4u * cached_v_total_lines * 60u;
    if (audio_sample_accum >= threshold) {
        audio_sample_accum -= threshold;
        if (di_ring_tail != di_ring_head) {
            const uint32_t *words = di_ring_buffer[di_ring_tail].words;
            di_ring_tail = (di_ring_tail + 1) % DI_RING_BUFFER_SIZE;
            return words;
        }
        audio_underrun_count++;
        return silence_packet.words;
    }
    return NULL;
}

void __not_in_flash("audio") hstx_di_queue_update_silence(int frame_counter)
{
    hstx_packet_t packet;
    audio_sample_t samples[4] = {0};
    (void)hstx_packet_set_audio_samples(&packet, samples, 4, frame_counter);
    hstx_encode_data_island(&silence_packet, &packet, false, true);
}
