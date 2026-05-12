/*
 * frank-video -- file browser.
 *
 * Scans /video on the SD card for *.mpg and lets the user pick one with the
 * arrow keys + Enter. Self-contained: brings its own 5x7 font and palette
 * so it doesn't pull in any rendering library.
 */

#include "file_browser.h"
#include "board_config.h"
#include "HDMI.h"
#include "ps2kbd_wrapper.h"
#include "ff.h"

#include "pico/stdlib.h"

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

extern uint8_t *g_framebuffer;

#define DISPLAY_W 320
#define DISPLAY_H 240

/* Palette indices used by the browser UI. The video player resets the
 * palette to its 6x6x6 cube, so we reload these whenever the browser runs. */
#define PAL_BG       0
#define PAL_FG       1
#define PAL_HILITE   2  /* selected-row background */
#define PAL_FG_DARK  3
#define PAL_ACCENT   4

/* HID usage codes. */
#define HID_KEY_ENTER  0x28
#define HID_KEY_ESCAPE 0x29
#define HID_KEY_RIGHT  0x4F
#define HID_KEY_LEFT   0x50
#define HID_KEY_DOWN   0x51
#define HID_KEY_UP     0x52
#define HID_KEY_PGUP   0x4B
#define HID_KEY_PGDN   0x4E
#define HID_KEY_HOME   0x4A
#define HID_KEY_END    0x4D

#define MAX_FILES 256
#define NAME_MAX  64

typedef struct {
    char name[NAME_MAX];
} entry_t;

static entry_t  g_entries[MAX_FILES];
static int      g_entry_count = 0;

/* ===== Drawing primitives ============================================== */

static void browser_palette_load(void) {
    graphics_set_palette(PAL_BG,      0x101820);
    graphics_set_palette(PAL_FG,      0xE6E8EE);
    graphics_set_palette(PAL_HILITE,  0x3060C0);
    graphics_set_palette(PAL_FG_DARK, 0x808896);
    graphics_set_palette(PAL_ACCENT,  0x60D0FF);
}

static inline void put_pixel(int x, int y, uint8_t c) {
    if (!g_framebuffer) return;
    if ((unsigned)x >= DISPLAY_W || (unsigned)y >= DISPLAY_H) return;
    g_framebuffer[y * DISPLAY_W + x] = c;
}

static void fill_rect(int x, int y, int w, int h, uint8_t c) {
    if (!g_framebuffer) return;
    if (w <= 0 || h <= 0) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > DISPLAY_W) w = DISPLAY_W - x;
    if (y + h > DISPLAY_H) h = DISPLAY_H - y;
    if (w <= 0 || h <= 0) return;
    for (int yy = y; yy < y + h; yy++)
        memset(&g_framebuffer[yy * DISPLAY_W + x], c, (size_t)w);
}

/* 5x7 font lifted from murmheretic/src/doomgeneric_rp2350.c. Columns are
 * MSB->LSB across 5 bits per row. Only the printable ASCII subset we need. */
static const uint8_t *glyph_5x7(char ch) {
    static const uint8_t glyph_space[7]  = {0,0,0,0,0,0,0};
    static const uint8_t glyph_bang[7]   = {0x04,0x04,0x04,0x04,0x04,0x00,0x04};
    static const uint8_t glyph_dot[7]    = {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C};
    static const uint8_t glyph_comma[7]  = {0x00,0x00,0x00,0x00,0x0C,0x0C,0x08};
    static const uint8_t glyph_colon[7]  = {0x00,0x0C,0x0C,0x00,0x0C,0x0C,0x00};
    static const uint8_t glyph_hyphen[7] = {0x00,0x00,0x00,0x1F,0x00,0x00,0x00};
    static const uint8_t glyph_under[7]  = {0x00,0x00,0x00,0x00,0x00,0x00,0x1F};
    static const uint8_t glyph_lparen[7] = {0x04,0x08,0x08,0x08,0x08,0x08,0x04};
    static const uint8_t glyph_rparen[7] = {0x04,0x02,0x02,0x02,0x02,0x02,0x04};
    static const uint8_t glyph_slash[7]  = {0x01,0x02,0x04,0x08,0x10,0x00,0x00};
    static const uint8_t glyph_quote[7]  = {0x0A,0x0A,0x00,0x00,0x00,0x00,0x00};
    static const uint8_t glyph_apos[7]   = {0x04,0x04,0x00,0x00,0x00,0x00,0x00};

    static const uint8_t glyph_0[7] = {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E};
    static const uint8_t glyph_1[7] = {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E};
    static const uint8_t glyph_2[7] = {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F};
    static const uint8_t glyph_3[7] = {0x1E,0x01,0x01,0x0E,0x01,0x01,0x1E};
    static const uint8_t glyph_4[7] = {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02};
    static const uint8_t glyph_5[7] = {0x1F,0x10,0x10,0x1E,0x01,0x01,0x1E};
    static const uint8_t glyph_6[7] = {0x0E,0x10,0x10,0x1E,0x11,0x11,0x0E};
    static const uint8_t glyph_7[7] = {0x1F,0x01,0x02,0x04,0x08,0x08,0x08};
    static const uint8_t glyph_8[7] = {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E};
    static const uint8_t glyph_9[7] = {0x0E,0x11,0x11,0x0F,0x01,0x01,0x0E};

    static const uint8_t glyph_A[7] = {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11};
    static const uint8_t glyph_B[7] = {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E};
    static const uint8_t glyph_C[7] = {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E};
    static const uint8_t glyph_D[7] = {0x1E,0x11,0x11,0x11,0x11,0x11,0x1E};
    static const uint8_t glyph_E[7] = {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F};
    static const uint8_t glyph_F[7] = {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10};
    static const uint8_t glyph_G[7] = {0x0E,0x11,0x10,0x17,0x11,0x11,0x0E};
    static const uint8_t glyph_H[7] = {0x11,0x11,0x11,0x1F,0x11,0x11,0x11};
    static const uint8_t glyph_I[7] = {0x1F,0x04,0x04,0x04,0x04,0x04,0x1F};
    static const uint8_t glyph_J[7] = {0x07,0x02,0x02,0x02,0x12,0x12,0x0C};
    static const uint8_t glyph_K[7] = {0x11,0x12,0x14,0x18,0x14,0x12,0x11};
    static const uint8_t glyph_L[7] = {0x10,0x10,0x10,0x10,0x10,0x10,0x1F};
    static const uint8_t glyph_M[7] = {0x11,0x1B,0x15,0x15,0x11,0x11,0x11};
    static const uint8_t glyph_N[7] = {0x11,0x19,0x15,0x13,0x11,0x11,0x11};
    static const uint8_t glyph_O[7] = {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E};
    static const uint8_t glyph_P[7] = {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10};
    static const uint8_t glyph_Q[7] = {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D};
    static const uint8_t glyph_R[7] = {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11};
    static const uint8_t glyph_S[7] = {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E};
    static const uint8_t glyph_T[7] = {0x1F,0x04,0x04,0x04,0x04,0x04,0x04};
    static const uint8_t glyph_U[7] = {0x11,0x11,0x11,0x11,0x11,0x11,0x0E};
    static const uint8_t glyph_V[7] = {0x11,0x11,0x11,0x11,0x0A,0x0A,0x04};
    static const uint8_t glyph_W[7] = {0x11,0x11,0x11,0x15,0x15,0x15,0x0A};
    static const uint8_t glyph_X[7] = {0x11,0x0A,0x04,0x04,0x04,0x0A,0x11};
    static const uint8_t glyph_Y[7] = {0x11,0x0A,0x04,0x04,0x04,0x04,0x04};
    static const uint8_t glyph_Z[7] = {0x1F,0x02,0x04,0x08,0x10,0x10,0x1F};

    static const uint8_t glyph_arrow_r[7] = {0x00,0x04,0x06,0x1F,0x06,0x04,0x00};
    static const uint8_t glyph_arrow_u[7] = {0x04,0x0E,0x15,0x04,0x04,0x04,0x00};
    static const uint8_t glyph_arrow_d[7] = {0x00,0x04,0x04,0x04,0x15,0x0E,0x04};

    int c = (unsigned char)ch;
    if (c >= 'a' && c <= 'z') c -= 32; /* font is upper-case only */
    switch (c) {
        case ' ': return glyph_space;
        case '!': return glyph_bang;
        case '"': return glyph_quote;
        case '\'': return glyph_apos;
        case '.': return glyph_dot;
        case ',': return glyph_comma;
        case ':': return glyph_colon;
        case '-': return glyph_hyphen;
        case '_': return glyph_under;
        case '(': return glyph_lparen;
        case ')': return glyph_rparen;
        case '/': return glyph_slash;

        case '0': return glyph_0; case '1': return glyph_1;
        case '2': return glyph_2; case '3': return glyph_3;
        case '4': return glyph_4; case '5': return glyph_5;
        case '6': return glyph_6; case '7': return glyph_7;
        case '8': return glyph_8; case '9': return glyph_9;

        case 'A': return glyph_A; case 'B': return glyph_B;
        case 'C': return glyph_C; case 'D': return glyph_D;
        case 'E': return glyph_E; case 'F': return glyph_F;
        case 'G': return glyph_G; case 'H': return glyph_H;
        case 'I': return glyph_I; case 'J': return glyph_J;
        case 'K': return glyph_K; case 'L': return glyph_L;
        case 'M': return glyph_M; case 'N': return glyph_N;
        case 'O': return glyph_O; case 'P': return glyph_P;
        case 'Q': return glyph_Q; case 'R': return glyph_R;
        case 'S': return glyph_S; case 'T': return glyph_T;
        case 'U': return glyph_U; case 'V': return glyph_V;
        case 'W': return glyph_W; case 'X': return glyph_X;
        case 'Y': return glyph_Y; case 'Z': return glyph_Z;

        case 0x10: return glyph_arrow_r; /* private codes used internally */
        case 0x11: return glyph_arrow_u;
        case 0x12: return glyph_arrow_d;

        default: return glyph_space;
    }
}

static void draw_char(int x, int y, char ch, uint8_t color) {
    const uint8_t *rows = glyph_5x7(ch);
    for (int row = 0; row < 7; row++) {
        uint8_t bits = rows[row];
        for (int col = 0; col < 5; col++) {
            if (bits & (1u << (4 - col))) put_pixel(x + col, y + row, color);
        }
    }
}

static void draw_text(int x, int y, const char *s, uint8_t color) {
    for (; *s; s++) {
        draw_char(x, y, *s, color);
        x += 6;
    }
}

static int text_width(const char *s) {
    int n = 0;
    while (*s++) n++;
    return n * 6;
}

/* ===== Directory scan ================================================== */

static int ascii_tolower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

static bool ends_with_mpg(const char *name) {
    size_t n = strlen(name);
    if (n < 4) return false;
    return  name[n-4] == '.'
        && ascii_tolower((unsigned char)name[n-3]) == 'm'
        && ascii_tolower((unsigned char)name[n-2]) == 'p'
        && ascii_tolower((unsigned char)name[n-1]) == 'g';
}

/* Insertion-sort the entries by name (case-insensitive) so the list stays
 * stable as the SD is reshuffled. The list is small enough that an O(n²)
 * sort is fine. */
static void sort_entries(void) {
    for (int i = 1; i < g_entry_count; i++) {
        entry_t key = g_entries[i];
        int j = i - 1;
        while (j >= 0) {
            const char *a = g_entries[j].name;
            const char *b = key.name;
            int cmp = 0;
            while (*a && *b) {
                int ca = ascii_tolower((unsigned char)*a++);
                int cb = ascii_tolower((unsigned char)*b++);
                if (ca != cb) { cmp = ca - cb; break; }
            }
            if (cmp == 0) cmp = ascii_tolower((unsigned char)*a)
                              - ascii_tolower((unsigned char)*b);
            if (cmp <= 0) break;
            g_entries[j + 1] = g_entries[j];
            j--;
        }
        g_entries[j + 1] = key;
    }
}

static void scan_video_dir(void) {
    g_entry_count = 0;

    DIR dir;
    FILINFO fno;
    /* Try /video first; if missing, fall back to root. Some users may drop
     * .mpg files at the SD root rather than in a subfolder. */
    const char *path = "/video";
    FRESULT fr = f_opendir(&dir, path);
    if (fr != FR_OK) {
        path = "/";
        fr = f_opendir(&dir, path);
        if (fr != FR_OK) return;
    }

    while (g_entry_count < MAX_FILES) {
        if (f_readdir(&dir, &fno) != FR_OK || fno.fname[0] == 0) break;
        if (fno.fattrib & AM_DIR) continue;
        if (!ends_with_mpg(fno.fname)) continue;

        snprintf(g_entries[g_entry_count].name, NAME_MAX, "%s", fno.fname);
        g_entry_count++;
    }
    f_closedir(&dir);

    sort_entries();
}

/* ===== UI rendering ===================================================== */

static int g_selected = 0;
static int g_scroll = 0;

#define LINE_H        10
#define LIST_X        16
#define LIST_Y        56
#define LIST_W       (DISPLAY_W - 32)
#define LIST_H       (DISPLAY_H - LIST_Y - 32)
#define MAX_VISIBLE  (LIST_H / LINE_H)

static void draw_header(void) {
    fill_rect(0, 0, DISPLAY_W, 36, PAL_BG);
    fill_rect(0, 36, DISPLAY_W, 2, PAL_ACCENT);

    const char *title = "FRANK-VIDEO  MPEG-1 Player";
    draw_text((DISPLAY_W - text_width(title)) / 2, 12, title, PAL_FG);
#ifndef FRANK_VERSION
#define FRANK_VERSION "?"
#endif
    char ver[32];
    snprintf(ver, sizeof(ver), "v%s", FRANK_VERSION);
    draw_text(DISPLAY_W - text_width(ver) - 6, 24, ver, PAL_FG_DARK);
}

static void draw_footer(void) {
    int y = DISPLAY_H - 22;
    fill_rect(0, y - 2, DISPLAY_W, 24, PAL_BG);
    fill_rect(0, y - 2, DISPLAY_W, 1, PAL_ACCENT);

    /* private-code arrows used for hint icons */
    char up_dn[] = { 0x11, 0x12, 0 };
    char hint1[64];
    snprintf(hint1, sizeof(hint1), "%s NAVIGATE   ENTER PLAY   ESC EXIT", up_dn);
    draw_text(8, y + 4, hint1, PAL_FG_DARK);
}

static void draw_list(void) {
    fill_rect(0, 38, DISPLAY_W, DISPLAY_H - 38 - 24, PAL_BG);

    if (g_entry_count == 0) {
        const char *m1 = "NO MPG FILES FOUND";
        const char *m2 = "PUT *.MPG IN /VIDEO ON THE SD CARD";
        draw_text((DISPLAY_W - text_width(m1)) / 2, 100, m1, PAL_FG);
        draw_text((DISPLAY_W - text_width(m2)) / 2, 116, m2, PAL_FG_DARK);
        return;
    }

    int end = g_scroll + MAX_VISIBLE;
    if (end > g_entry_count) end = g_entry_count;

    for (int i = g_scroll; i < end; i++) {
        int y = LIST_Y + (i - g_scroll) * LINE_H;
        if (i == g_selected) {
            fill_rect(LIST_X - 2, y - 1, LIST_W + 4, LINE_H, PAL_HILITE);
            draw_text(LIST_X + 6, y + 1, g_entries[i].name, PAL_FG);
        } else {
            draw_text(LIST_X + 6, y + 1, g_entries[i].name, PAL_FG);
        }
    }

    /* Scroll indicators. */
    if (g_scroll > 0) {
        char up[] = { 0x11, 0 };
        draw_text(DISPLAY_W - 12, LIST_Y, up, PAL_ACCENT);
    }
    if (end < g_entry_count) {
        char dn[] = { 0x12, 0 };
        draw_text(DISPLAY_W - 12, LIST_Y + (MAX_VISIBLE - 1) * LINE_H, dn, PAL_ACCENT);
    }
}

static void clamp_scroll(void) {
    if (g_selected < g_scroll) g_scroll = g_selected;
    if (g_selected >= g_scroll + MAX_VISIBLE)
        g_scroll = g_selected - MAX_VISIBLE + 1;
    if (g_scroll < 0) g_scroll = 0;
}

/* ===== Public entry ===================================================== */

void file_browser_init(void) {
    /* Nothing to allocate yet -- the list buffer is static. The first call
     * to file_browser_show() loads the palette and runs a scan. */
}

bool file_browser_show(char *out_path, int out_path_len) {
    if (!out_path || out_path_len < 8) return false;

    if (g_framebuffer) memset(g_framebuffer, 0, DISPLAY_W * DISPLAY_H);
    browser_palette_load();

    scan_video_dir();
    g_selected = 0;
    g_scroll = 0;

    draw_header();
    draw_footer();
    draw_list();

    while (true) {
        ps2kbd_tick();

        int pressed;
        uint8_t code;
        bool dirty = false;

        while (ps2kbd_get_event(&pressed, &code)) {
            if (!pressed) continue;

            if (code == HID_KEY_ESCAPE) return false;

            if (g_entry_count == 0) continue;

            if (code == HID_KEY_ENTER) {
                /* Rebuild with the matching directory prefix used in scan. */
                DIR dir;
                if (f_opendir(&dir, "/video") == FR_OK) {
                    f_closedir(&dir);
                    snprintf(out_path, out_path_len, "/video/%s",
                             g_entries[g_selected].name);
                } else {
                    snprintf(out_path, out_path_len, "/%s",
                             g_entries[g_selected].name);
                }
                return true;
            }

            int prev = g_selected;
            switch (code) {
                case HID_KEY_UP:
                    g_selected = (g_selected - 1 + g_entry_count) % g_entry_count;
                    break;
                case HID_KEY_DOWN:
                    g_selected = (g_selected + 1) % g_entry_count;
                    break;
                case HID_KEY_PGUP:
                    g_selected -= MAX_VISIBLE;
                    if (g_selected < 0) g_selected = 0;
                    break;
                case HID_KEY_PGDN:
                    g_selected += MAX_VISIBLE;
                    if (g_selected >= g_entry_count) g_selected = g_entry_count - 1;
                    break;
                case HID_KEY_HOME: g_selected = 0; break;
                case HID_KEY_END:  g_selected = g_entry_count - 1; break;
                default: break;
            }
            if (g_selected != prev) {
                clamp_scroll();
                dirty = true;
            }
        }

        if (dirty) draw_list();
        sleep_ms(16);
    }
}
