/*
 * frank-video -- TinyUSB host callbacks for USB keyboard input.
 *
 * Adapted from frank-fodder/drivers/usbhid/hid_app.c, simplified to
 * keyboard-only (no mouse) and emitting raw USB HID usage codes that
 * the rest of frank-video already speaks.
 *
 * SPDX-License-Identifier: MIT
 */

#include "tusb.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if CFG_TUH_ENABLED

/* Per-instance HID descriptor cache for generic (non-boot) devices. */
#define MAX_REPORT 4
static struct {
    uint8_t                 report_count;
    tuh_hid_report_info_t   report_info[MAX_REPORT];
} hid_info[CFG_TUH_HID];

/* Keep the previous keyboard report so we can diff press / release. */
static hid_keyboard_report_t prev_kbd_report = { 0, 0, {0} };

/* SPSC ring of (HID code, pressed) tuples. The IRQ-side TinyUSB
 * callbacks push; the application drains via usbhid_wrapper_get_event(). */
#define KEY_QUEUE_SIZE 32
typedef struct { uint8_t hid_code; uint8_t pressed; } key_evt_t;
static key_evt_t        key_queue[KEY_QUEUE_SIZE];
static volatile uint8_t key_queue_head = 0;
static volatile uint8_t key_queue_tail = 0;

static volatile int keyboard_connected = 0;

static void enqueue(uint8_t hid_code, uint8_t pressed) {
    uint8_t next = (uint8_t)((key_queue_head + 1) % KEY_QUEUE_SIZE);
    if (next == key_queue_tail) return; /* drop on overflow */
    key_queue[key_queue_head].hid_code = hid_code;
    key_queue[key_queue_head].pressed  = pressed;
    key_queue_head = next;
}

static int contains_keycode(const hid_keyboard_report_t *r, uint8_t kc) {
    for (int i = 0; i < 6; i++) if (r->keycode[i] == kc) return 1;
    return 0;
}

/* Emit press / release events for both modifier keys and the six-slot
 * roll-over set. Modifiers are reported using the standard HID usage
 * codes 0xE0..0xE7 (LCtrl/LShift/LAlt/LGUI/RCtrl/...) so they round-trip
 * cleanly through the rest of the codebase. */
static void process_kbd_report(const hid_keyboard_report_t *r) {
    const uint8_t prev_mod = prev_kbd_report.modifier;
    const uint8_t new_mod  = r->modifier;
    const uint8_t released = (uint8_t)(prev_mod & ~new_mod);
    const uint8_t pressed  = (uint8_t)(new_mod & ~prev_mod);

    /* HID modifier bit i corresponds to usage 0xE0 + i. */
    for (int i = 0; i < 8; i++) {
        if (released & (1u << i)) enqueue((uint8_t)(0xE0 + i), 0);
        if (pressed  & (1u << i)) enqueue((uint8_t)(0xE0 + i), 1);
    }

    for (int i = 0; i < 6; i++) {
        uint8_t kc = prev_kbd_report.keycode[i];
        if (kc && !contains_keycode(r, kc)) enqueue(kc, 0);
    }
    for (int i = 0; i < 6; i++) {
        uint8_t kc = r->keycode[i];
        if (kc && !contains_keycode(&prev_kbd_report, kc)) enqueue(kc, 1);
    }

    prev_kbd_report = *r;
}

/* Generic HID parser fallback for devices that don't use the boot
 * keyboard protocol. Mice and other report types are silently ignored. */
static void process_generic_report(uint8_t dev_addr, uint8_t instance,
                                   const uint8_t *report, uint16_t len) {
    (void)dev_addr;

    const uint8_t rpt_count = hid_info[instance].report_count;
    tuh_hid_report_info_t *arr = hid_info[instance].report_info;
    tuh_hid_report_info_t *info = NULL;

    if (rpt_count == 1 && arr[0].report_id == 0) {
        info = &arr[0];
    } else {
        const uint8_t rpt_id = report[0];
        for (uint8_t i = 0; i < rpt_count; i++) {
            if (rpt_id == arr[i].report_id) { info = &arr[i]; break; }
        }
        report++;
        len--;
    }
    if (!info) return;

    if (info->usage_page == HID_USAGE_PAGE_DESKTOP &&
        info->usage      == HID_USAGE_DESKTOP_KEYBOARD) {
        process_kbd_report((const hid_keyboard_report_t *)report);
    }
}

/* ===== TinyUSB host callbacks ============================================ */

void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t instance,
                      const uint8_t *desc_report, uint16_t desc_len) {
    const uint8_t itf = tuh_hid_interface_protocol(dev_addr, instance);

    if (itf == HID_ITF_PROTOCOL_KEYBOARD) {
        keyboard_connected = 1;
        printf("USB keyboard connected\n");
    }

    /* Generic devices need their report descriptor parsed up front so
     * we know which usage IDs to dispatch on. */
    if (itf == HID_ITF_PROTOCOL_NONE) {
        hid_info[instance].report_count = tuh_hid_parse_report_descriptor(
            hid_info[instance].report_info, MAX_REPORT, desc_report, desc_len);
    }

    tuh_hid_receive_report(dev_addr, instance);
}

void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t instance) {
    (void)dev_addr;
    const uint8_t itf = tuh_hid_interface_protocol(dev_addr, instance);
    if (itf == HID_ITF_PROTOCOL_KEYBOARD) {
        keyboard_connected = 0;
        printf("USB keyboard disconnected\n");
        memset(&prev_kbd_report, 0, sizeof(prev_kbd_report));
    }
}

void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t instance,
                                const uint8_t *report, uint16_t len) {
    const uint8_t itf = tuh_hid_interface_protocol(dev_addr, instance);
    if (itf == HID_ITF_PROTOCOL_KEYBOARD) {
        process_kbd_report((const hid_keyboard_report_t *)report);
    } else if (itf == HID_ITF_PROTOCOL_NONE) {
        process_generic_report(dev_addr, instance, report, len);
    }
    /* Mouse and others: ignored. */

    tuh_hid_receive_report(dev_addr, instance);
}

/* ===== Internal API consumed by usbhid_wrapper.c ========================= */

void usbhid_app_init(void) {
    tuh_init(BOARD_TUH_RHPORT);
    memset(&prev_kbd_report, 0, sizeof(prev_kbd_report));
    key_queue_head = 0;
    key_queue_tail = 0;
}

void usbhid_app_task(void) {
    tuh_task();
}

int usbhid_app_keyboard_connected(void) {
    return keyboard_connected;
}

int usbhid_app_pop_event(int *pressed, uint8_t *hid_code) {
    if (key_queue_head == key_queue_tail) return 0;
    const key_evt_t e = key_queue[key_queue_tail];
    key_queue_tail = (uint8_t)((key_queue_tail + 1) % KEY_QUEUE_SIZE);
    *pressed  = e.pressed ? 1 : 0;
    *hid_code = e.hid_code;
    return 1;
}

#endif /* CFG_TUH_ENABLED */
