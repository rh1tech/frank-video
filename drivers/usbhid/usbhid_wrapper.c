/*
 * frank-video -- USB HID wrapper.
 *
 * Thin facade over hid_app.c. Lives in its own translation unit so the
 * top-level CMakeLists can drop usbhid out of the build entirely when
 * USB_HID_ENABLED is off (header-only stubs handle that case).
 *
 * SPDX-License-Identifier: MIT
 */

#include "../usbhid_wrapper.h"

#ifdef USB_HID_ENABLED

extern void usbhid_app_init(void);
extern void usbhid_app_task(void);
extern int  usbhid_app_keyboard_connected(void);
extern int  usbhid_app_pop_event(int *pressed, uint8_t *hid_code);

static int g_initialized = 0;

void usbhid_wrapper_init(void) {
    if (g_initialized) return;
    usbhid_app_init();
    g_initialized = 1;
}

void usbhid_wrapper_task(void) {
    if (!g_initialized) return;
    usbhid_app_task();
}

int usbhid_wrapper_get_event(int *pressed, uint8_t *hid_code) {
    if (!g_initialized) return 0;
    return usbhid_app_pop_event(pressed, hid_code);
}

int usbhid_wrapper_keyboard_connected(void) {
    if (!g_initialized) return 0;
    return usbhid_app_keyboard_connected();
}

#endif /* USB_HID_ENABLED */
