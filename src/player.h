/*
 * frank-video -- MPEG-1 video player.
 *
 * Streams a .mpg file from the SD card and renders it at 320x240 in 256
 * colours over HDMI, with audio over I2S. Adapted from rhea's videoplayer
 * app. Controls: ESC = exit, Space = pause.
 *
 * Returns when ESC is pressed or playback finishes.
 */

#ifndef PLAYER_H
#define PLAYER_H

#ifdef __cplusplus
extern "C" {
#endif

void player_play(const char *path);

#ifdef __cplusplus
}
#endif

#endif
