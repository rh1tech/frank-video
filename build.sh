#!/bin/bash
# Build frank-video for RP2350.
#
# Defaults match the design point: BOARD_VARIANT=M2, 504/166/66, USB_HID off
# (so the USB CDC console works as before).
#
# Override on the command line, e.g.:
#     ./build.sh -DBOARD_VARIANT=M2
#
# Optional environment variables:
#     USB_HID=1   Enable USB HID host (TinyUSB). Disables USB CDC stdio so
#                 debug output goes to UART instead. Both PS/2 and USB
#                 keyboards work in this mode. Default 0.

set -e

USB_HID=${USB_HID:-0}
if [ "$USB_HID" = "1" ]; then
    USB_HID_FLAG=ON
else
    USB_HID_FLAG=OFF
fi

echo "Building frank-video: USB_HID=${USB_HID_FLAG}"

rm -rf ./build
mkdir build
cd build
cmake -DPICO_PLATFORM=rp2350 \
      -DBOARD_VARIANT=M2 \
      -DCPU_SPEED=504 \
      -DPSRAM_SPEED=166 \
      -DFLASH_SPEED=66 \
      -DUSB_HID_ENABLED=$USB_HID_FLAG \
      "$@" ..
make -j4
