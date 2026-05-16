/*
 * frank-video -- USB HID wrapper.
 *
 * Provides keyboard input via the RP2350's native USB controller in Host
 * mode. Mirrors the ps2kbd_wrapper API (pumps + an event-pop function
 * that returns USB HID usage codes), so it can be drained alongside the
 * PS/2 queue without any translation.
 *
 * When USB_HID_ENABLED is undefined (default build) all functions are
 * inline no-ops so the rest of the code can call them unconditionally.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef USBHID_WRAPPER_H
#define USBHID_WRAPPER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef USB_HID_ENABLED

/* Bring up TinyUSB Host. Must be called once after stdio_init_all().
 * USB_HID and USB CDC are mutually exclusive (a single controller can't
 * be both Host and Device), so the build also disables stdio_usb when
 * USB_HID is on. */
void usbhid_wrapper_init(void);

/* Pump the TinyUSB host stack. Cheap, IRQ-safe; a 1 kHz repeating timer
 * calls this so HID reports never queue up between frames. */
void usbhid_wrapper_task(void);

/* Pop the next pending key event. Returns 1 if an event was returned,
 * 0 if the queue was empty. *hid_code is the USB HID usage (e.g. 0x29
 * for Esc, 0x2C for Space). Modifier keys are reported via the
 * pseudo-codes 0xE0 (Ctrl), 0xE1 (Shift), 0xE2 (Alt). */
int usbhid_wrapper_get_event(int *pressed, uint8_t *hid_code);

/* True if a USB keyboard is currently mounted. */
int usbhid_wrapper_keyboard_connected(void);

#else /* !USB_HID_ENABLED */

static inline void usbhid_wrapper_init(void) {}
static inline void usbhid_wrapper_task(void) {}
static inline int  usbhid_wrapper_get_event(int *pressed, uint8_t *hid_code) {
    (void)pressed; (void)hid_code; return 0;
}
static inline int  usbhid_wrapper_keyboard_connected(void) { return 0; }

#endif /* USB_HID_ENABLED */

#ifdef __cplusplus
}
#endif

#endif /* USBHID_WRAPPER_H */
