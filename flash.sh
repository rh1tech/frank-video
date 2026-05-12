#!/bin/bash
# Flash frank-video onto an RP2350 already in BOOTSEL or rebootable via picotool.
picotool load ./build/frank-video.elf -f && picotool reboot
