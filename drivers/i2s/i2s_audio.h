/*
 * FRANK NES - I2S Audio Driver
 * Chained double-buffer DMA via PIO
 * Adapted from murmgenesis audio driver.
 * SPDX-License-Identifier: MIT
 */

#ifndef I2S_AUDIO_H
#define I2S_AUDIO_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize I2S audio output on PIO0.
 * @param data_pin     GPIO for I2S serial data
 * @param clock_pin_base GPIO for LRCLK (clock_pin_base+1 = BCLK)
 * @param sample_rate  Audio sample rate in Hz (e.g. 44100)
 */
void i2s_audio_init(unsigned int data_pin, unsigned int clock_pin_base, uint32_t sample_rate);

/**
 * Push mono 16-bit samples to I2S output (duplicated to stereo).
 * Blocks briefly if both DMA buffers are in-flight.
 * @param buf   Mono 16-bit PCM samples
 * @param count Number of samples
 */
void i2s_audio_push_samples(const int16_t *buf, int count);

/**
 * Push interleaved stereo 16-bit samples (L,R,L,R,...) to I2S.
 * @param buf    Interleaved stereo PCM, two int16_t per frame
 * @param frames Number of stereo frames
 */
void i2s_audio_push_stereo(const int16_t *buf, int frames);

/**
 * Non-blocking variant of i2s_audio_push_stereo: drops the chunk if no DMA
 * buffer is currently free, returning 0; returns 1 on success. Use this on
 * latency-sensitive paths where blocking the producer is worse than a
 * brief audio glitch (e.g. video decode loops).
 */
int i2s_audio_try_push_stereo(const int16_t *buf, int frames);

/**
 * Stream-mode push: enqueue stereo frames into a software ring buffer that
 * an IRQ-driven consumer drains into the DMA chunks. Decouples producer
 * chunk size from DMA chunk size, so MPEG-1 audio (1152 frames per call)
 * never gets silence-padded inside a chunk.
 *
 * Returns the number of frames actually accepted. If the ring is full the
 * remainder is dropped (producer outpaced realtime).
 */
int i2s_audio_stream_push(const int16_t *interleaved, int frames);

/**
 * How many free frames the ring buffer currently has. Use to decide if
 * a producer should yield rather than drop.
 */
int i2s_audio_stream_free(void);

/**
 * Push silence frames to I2S (keeps DMA running without pops).
 * @param count Number of silence samples
 */
void i2s_audio_fill_silence(int count);

/**
 * Resize the per-DMA-chunk transfer count to match the emulation frame rate.
 * Called whenever region switches between NTSC (60 fps) and Dendy (50 fps);
 * keeps consumer throughput aligned with producer throughput so the emulator
 * is not back-pressured.
 * @param frame_rate Emulation frame rate in Hz (60 for NTSC, 50 for Dendy)
 */
void i2s_audio_set_frame_rate(int frame_rate);

/**
 * Shut down I2S audio (stop DMA, disable PIO SM).
 */
void i2s_audio_shutdown(void);

/**
 * Number of stereo audio frames the consumer has had to substitute with
 * silence since the last call (clears on read). The producer can use
 * this to drop an equivalent number of input frames so audio stays
 * aligned with wall clock when the queue underruns.
 *
 * On the legacy I2S backend (drivers/i2s/i2s_audio.c) the DMA layer
 * silence-pads short bursts internally and reports 0; only the HSTX
 * backend reports real underrun frames.
 */
uint32_t i2s_audio_consume_underrun_frames(void);

#ifdef __cplusplus
}
#endif

#endif /* I2S_AUDIO_H */
