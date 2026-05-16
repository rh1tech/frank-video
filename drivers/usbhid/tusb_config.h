/*
 * frank-video -- TinyUSB host configuration.
 *
 * Native USB controller is owned by Host mode here, so CDC stdio is
 * disabled. This header is only included when USB_HID_ENABLED is set
 * (drivers/usbhid/CMakeLists.txt only adds this directory to the include
 * path in that case).
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef TUSB_CONFIG_H
#define TUSB_CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

#ifndef CFG_TUSB_MCU
#define CFG_TUSB_MCU OPT_MCU_RP2040
#endif

#ifndef BOARD_TUH_RHPORT
#define BOARD_TUH_RHPORT 0
#endif

#define BOARD_TUH_MAX_SPEED OPT_MODE_FULL_SPEED

/* Host on, Device off — these can't coexist on the native USB. */
#define CFG_TUH_ENABLED 1
#define CFG_TUD_ENABLED 0

#define CFG_TUH_MAX_SPEED BOARD_TUH_MAX_SPEED

/* No PIO-USB. The native port is enough for one keyboard (or a hub). */
#ifndef CFG_TUH_RPI_PIO_USB
#define CFG_TUH_RPI_PIO_USB 0
#endif

#define CFG_TUH_ENUMERATION_BUFSIZE 256

/* 1 hub + a few HID interfaces is plenty for a video player. */
#define CFG_TUH_DEVICE_MAX 5
#define CFG_TUH_HUB        1
#define CFG_TUH_HID        8
#define CFG_TUH_CDC        0
#define CFG_TUH_VENDOR     0
#define CFG_TUH_MSC        0

#define CFG_TUH_HID_EPIN_BUFSIZE  64
#define CFG_TUH_HID_EPOUT_BUFSIZE 64

#ifdef __cplusplus
}
#endif

#endif /* TUSB_CONFIG_H */
