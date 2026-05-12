#!/bin/bash
# Build frank-video for RP2350.
#
# Defaults match the design point: BOARD_VARIANT=M1, 504/166/66.
# Override on the command line, e.g.:
#     ./build.sh -DBOARD_VARIANT=M2

set -e
rm -rf ./build
mkdir build
cd build
cmake -DPICO_PLATFORM=rp2350 \
      -DBOARD_VARIANT=M2 \
      -DCPU_SPEED=504 \
      -DPSRAM_SPEED=166 \
      -DFLASH_SPEED=66 \
      "$@" ..
make -j4
