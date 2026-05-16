#!/bin/bash
#
# frank-video — MPEG-1 video player for Raspberry Pi Pico 2 (RP2350)
#
# Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
# https://github.com/rh1tech/frank-video
#
# Licensed under the GNU General Public License v3.0 or later.
# See LICENSE for the full license text.
#

#
# release.sh - Build release firmware for frank-video.
#
# Usage: ./release.sh [VERSION]
#   VERSION  - version string (e.g. "1.01"), prompted interactively if omitted
#
# Output format: <prefix>frank-video_A_BB.uf2
#
# Build matrix (3 variants):
#   m1p2_frank-video_*.uf2       (M1, legacy PIO HDMI + I2S audio)
#   m2p2_frank-video_*.uf2       (M2, legacy PIO HDMI + I2S audio)
#   m2p2alt_frank-video_*.uf2    (M2, HSTX HDMI with HDMI audio over Data Islands)
#
# All variants ship at 504 MHz CPU / 166 MHz PSRAM / 66 MHz Flash.
# USB HID host is enabled, USB CDC stdio is disabled.
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m' # No Color

# Release-wide clock defaults (override via env for one-off rebuilds).
RELEASE_CPU_SPEED="${RELEASE_CPU_SPEED:-504}"
RELEASE_PSRAM_SPEED="${RELEASE_PSRAM_SPEED:-166}"
RELEASE_FLASH_SPEED="${RELEASE_FLASH_SPEED:-66}"

# Build matrix: "board_variant:hstx_flag:prefix"
BUILD_MATRIX=(
    "M1:OFF:m1p2_"
    "M2:OFF:m2p2_"
    "M2:ON:m2p2alt_"
)

# Version file
VERSION_FILE="version.txt"

# Read last version or initialize
if [[ -f "$VERSION_FILE" ]]; then
    read -r LAST_MAJOR LAST_MINOR < "$VERSION_FILE"
else
    LAST_MAJOR=1
    LAST_MINOR=0
fi

# Calculate next version (for default suggestion)
NEXT_MINOR=$((LAST_MINOR + 1))
NEXT_MAJOR=$LAST_MAJOR
if [[ $NEXT_MINOR -ge 100 ]]; then
    NEXT_MAJOR=$((NEXT_MAJOR + 1))
    NEXT_MINOR=0
fi

# Interactive version input
echo ""
echo -e "${CYAN}┌─────────────────────────────────────────────────────────────────┐${NC}"
echo -e "${CYAN}│                  frank-video Release Builder                    │${NC}"
echo -e "${CYAN}└─────────────────────────────────────────────────────────────────┘${NC}"
echo ""
echo -e "Last version: ${YELLOW}${LAST_MAJOR}.$(printf '%02d' $LAST_MINOR)${NC}"
echo -e "Variants: ${CYAN}${#BUILD_MATRIX[@]}${NC} (M1/HDMI, M2/HDMI, M2/HDMI_ALT)"
echo -e "Clocks:   ${CYAN}${RELEASE_CPU_SPEED}/${RELEASE_PSRAM_SPEED}/${RELEASE_FLASH_SPEED} MHz${NC}"
echo ""

DEFAULT_VERSION="${NEXT_MAJOR}.$(printf '%02d' $NEXT_MINOR)"

# Accept version from command line or prompt interactively
if [[ -n "$1" ]]; then
    INPUT_VERSION="$1"
    echo -e "Version (from command line): ${CYAN}${INPUT_VERSION}${NC}"
else
    read -p "Enter version [default: $DEFAULT_VERSION]: " INPUT_VERSION
    INPUT_VERSION=${INPUT_VERSION:-$DEFAULT_VERSION}
fi

# Parse version (handle both "1.00" and "1 00" formats)
if [[ "$INPUT_VERSION" == *"."* ]]; then
    MAJOR="${INPUT_VERSION%%.*}"
    MINOR="${INPUT_VERSION##*.}"
else
    read -r MAJOR MINOR <<< "$INPUT_VERSION"
fi

# Remove leading zeros for arithmetic, then re-pad
MINOR=$((10#$MINOR))
MAJOR=$((10#$MAJOR))

# Validate
if [[ $MAJOR -lt 0 ]]; then
    echo -e "${RED}Error: Major version must be >= 1${NC}"
    exit 1
fi
if [[ $MINOR -lt 0 || $MINOR -ge 100 ]]; then
    echo -e "${RED}Error: Minor version must be 0-99${NC}"
    exit 1
fi

# Format version strings
VERSION="${MAJOR}_$(printf '%02d' $MINOR)"
VERSION_DOT="${MAJOR}.$(printf '%02d' $MINOR)"
echo ""
echo -e "${GREEN}Building release version: ${VERSION_DOT}${NC}"

# Save new version
echo "$MAJOR $MINOR" > "$VERSION_FILE"

# Create release directory
RELEASE_DIR="$SCRIPT_DIR/release"
mkdir -p "$RELEASE_DIR"

SUCCEEDED=()
FAILED=()

for ENTRY in "${BUILD_MATRIX[@]}"; do
    IFS=':' read -r BOARD HSTX PREFIX <<< "$ENTRY"
    LABEL="${PREFIX%_}"
    OUTPUT_NAME="${PREFIX}frank-video_${VERSION}.uf2"

    echo ""
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    echo -e "${CYAN}Building: $OUTPUT_NAME${NC}"
    echo -e "  Board=${BOARD}  HDMI_HSTX=${HSTX}  USB_HID=ON"
    echo ""

    # Clean and create build directory
    rm -rf build
    mkdir build
    cd build

    # Configure with CMake (USB HID enabled for release).
    if cmake .. \
        -DPICO_PLATFORM=rp2350 \
        -DBOARD_VARIANT="$BOARD" \
        -DCPU_SPEED="$RELEASE_CPU_SPEED" \
        -DPSRAM_SPEED="$RELEASE_PSRAM_SPEED" \
        -DFLASH_SPEED="$RELEASE_FLASH_SPEED" \
        -DHDMI_HSTX="$HSTX" \
        -DUSB_HID_ENABLED=ON > /dev/null 2>&1; then

        # Build
        if make -j$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4) > /dev/null 2>&1; then
            # Copy UF2 to release directory
            if [[ -f "frank-video.uf2" ]]; then
                cp "frank-video.uf2" "$RELEASE_DIR/$OUTPUT_NAME"
                echo -e "  ${GREEN}✓ $LABEL${NC} → release/$OUTPUT_NAME"
                SUCCEEDED+=("$OUTPUT_NAME")
            else
                echo -e "  ${RED}✗ $LABEL: UF2 not found${NC}"
                FAILED+=("$LABEL")
            fi
        else
            echo -e "  ${RED}✗ $LABEL: Build failed${NC}"
            FAILED+=("$LABEL")
        fi
    else
        echo -e "  ${RED}✗ $LABEL: CMake configure failed${NC}"
        FAILED+=("$LABEL")
    fi

    cd "$SCRIPT_DIR"
done

# Clean up build directory
rm -rf build

echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

if [[ ${#SUCCEEDED[@]} -gt 0 ]]; then
    echo -e "${GREEN}Succeeded: ${SUCCEEDED[*]}${NC}"
fi
if [[ ${#FAILED[@]} -gt 0 ]]; then
    echo -e "${RED}Failed: ${FAILED[*]}${NC}"
fi

echo ""
echo "Release files:"
for FNAME in "${SUCCEEDED[@]}"; do
    ls -la "$RELEASE_DIR/$FNAME" 2>/dev/null | awk '{printf "  %-55s (%s bytes)\n", $9, $5}'
done
echo ""
echo -e "Version: ${CYAN}${VERSION_DOT}${NC}"

if [[ ${#FAILED[@]} -gt 0 ]]; then
    echo -e "${YELLOW}Warning: ${#FAILED[@]} variant(s) failed to build${NC}"
fi
