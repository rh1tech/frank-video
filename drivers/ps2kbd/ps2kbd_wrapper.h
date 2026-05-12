#ifndef PS2KBD_WRAPPER_H
#define PS2KBD_WRAPPER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize the PS/2 keyboard driver. Pin assignments come from
 * board_config.h via PS2_PIN_CLK / PS2_PIN_DATA. */
void ps2kbd_init(void);

/* Pump the PS/2 keyboard state machine. Call this in your main loop. */
void ps2kbd_tick(void);

/* Pop the next pending key event. Returns 1 if an event was returned,
 * 0 if the queue was empty.
 *   *pressed = 1 on press, 0 on release
 *   *hid_code = USB HID usage code (e.g. 0x29 = Esc, 0x2C = Space) */
int ps2kbd_get_event(int *pressed, uint8_t *hid_code);

#ifdef __cplusplus
}
#endif

#endif
