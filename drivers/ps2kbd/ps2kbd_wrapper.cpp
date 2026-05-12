/*
 * frank-video -- PS/2 keyboard wrapper
 *
 * Adapted from murmnes / murmheretic ps2kbd_wrapper.cpp.
 * Exposes raw HID usage codes (no game-specific remapping).
 */

#include "../../src/board_config.h"
#include "ps2kbd_wrapper.h"
#include "ps2kbd_mrmltr.h"

/* Lock-free ring buffer for key events (size must be power of two). */
#define EVENT_QUEUE_SIZE 32

struct KeyEvent {
    uint8_t pressed;
    uint8_t hid_code;
};

static KeyEvent event_queue[EVENT_QUEUE_SIZE];
static volatile uint8_t queue_head = 0;
static volatile uint8_t queue_tail = 0;

static inline bool queue_empty(void) { return queue_head == queue_tail; }

static inline bool queue_full(void) {
    return ((queue_head + 1) & (EVENT_QUEUE_SIZE - 1)) == queue_tail;
}

static void queue_push(uint8_t pressed, uint8_t hid_code) {
    if (!queue_full()) {
        event_queue[queue_head].pressed = pressed;
        event_queue[queue_head].hid_code = hid_code;
        queue_head = (queue_head + 1) & (EVENT_QUEUE_SIZE - 1);
    }
}

static bool queue_pop(uint8_t *pressed, uint8_t *hid_code) {
    if (queue_empty()) return false;
    *pressed = event_queue[queue_tail].pressed;
    *hid_code = event_queue[queue_tail].hid_code;
    queue_tail = (queue_tail + 1) & (EVENT_QUEUE_SIZE - 1);
    return true;
}

static void key_handler(hid_keyboard_report_t *curr, hid_keyboard_report_t *prev) {
    /* New key presses */
    for (int i = 0; i < 6; i++) {
        if (curr->keycode[i] != 0) {
            bool found = false;
            for (int j = 0; j < 6; j++) {
                if (prev->keycode[j] == curr->keycode[i]) { found = true; break; }
            }
            if (!found) queue_push(1, curr->keycode[i]);
        }
    }
    /* Key releases */
    for (int i = 0; i < 6; i++) {
        if (prev->keycode[i] != 0) {
            bool found = false;
            for (int j = 0; j < 6; j++) {
                if (curr->keycode[j] == prev->keycode[i]) { found = true; break; }
            }
            if (!found) queue_push(0, prev->keycode[i]);
        }
    }
}

static Ps2Kbd_Mrmltr *kbd = nullptr;

extern "C" void ps2kbd_init(void) {
    kbd = new Ps2Kbd_Mrmltr(pio0, PS2_PIN_CLK, key_handler);
    kbd->init_gpio();
}

extern "C" void ps2kbd_tick(void) {
    if (kbd) kbd->tick();
}

extern "C" int ps2kbd_get_event(int *pressed, uint8_t *hid_code) {
    uint8_t p, k;
    if (!queue_pop(&p, &k)) return 0;
    *pressed = p;
    *hid_code = k;
    return 1;
}
