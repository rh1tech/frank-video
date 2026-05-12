/*
 * frank-video -- file browser.
 *
 * Scans the SD card's /video directory for *.mpg files and lets the user
 * pick one with arrow keys + Enter. Adapted in spirit from murmnes's
 * rom_selector but stripped down: no covers, no CRCs, no settings.
 */

#ifndef FILE_BROWSER_H
#define FILE_BROWSER_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Path buffer length used when returning a selection. */
#define FB_PATH_MAX 256

/* Initialize the browser's framebuffer + palette. Must be called once,
 * after the HDMI driver is up. */
void file_browser_init(void);

/* Run the browser UI until the user picks a file or hits ESC.
 *
 * On success returns true and fills out_path with a /video-relative path
 * usable with f_open (e.g. "/video/clip.mpg").
 * Returns false if there are no files or the user pressed ESC. */
bool file_browser_show(char *out_path, int out_path_len);

#ifdef __cplusplus
}
#endif

#endif
