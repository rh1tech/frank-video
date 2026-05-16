# FRANK VIDEO

Official page: **[frank.rh1.tech](https://frank.rh1.tech/)** — hub for all FRANK boards and firmware.

FRANK Video is an MPEG-1 video player for Raspberry Pi Pico 2 (RP2350) — runs on FRANK boards with HDMI output, SD card streaming, PS/2 keyboard, and optional USB HID host. Plays 320×240 MPEG-1 video with stereo MPEG-1 Layer-II audio in real time, decoded by [pl_mpeg](https://github.com/phoboslab/pl_mpeg).

## Supported Platforms

frank-video supports two RP2350 hardware variants and two HDMI back-ends. Pin assignments are selected at build time via `BOARD_VARIANT=M1|M2`; the HDMI driver is selected via `HDMI_HSTX=ON|OFF`.

| Platform | Board | Video | Audio |
|----------|-------|-------|-------|
| **M1** | Murmulator 1.x | HDMI (PIO) | I2S |
| **M2** | [FRANK](https://rh1.tech/projects/frank?area=about) / [Murmulator 2.0](https://murmulator.ru) | HDMI (PIO) | I2S |
| **M2 / HDMI_ALT** | FRANK / Murmulator 2.0 | HDMI (HSTX) | HDMI Audio (Data Islands) |

The default build target is **M2**. The HDMI_ALT variant uses RP2350's native HSTX peripheral for HDMI output and embeds audio inside the HDMI stream as Data Island packets — a single HDMI cable carries both video and audio, no I2S DAC needed.

## Features

- MPEG-1 video playback at 320×240, line-doubled to HDMI 640×480
- MPEG-1 Layer-II audio decoded to stereo PCM, output via I2S DAC or HDMI audio (HDMI_ALT)
- Wall-clock-paced decode with frame skipping and audio drift compensation — playback stays in sync over hours
- Heavy-frame escape: I-frame decode skip on scenes that would blow the realtime budget
- 8 MB QSPI PSRAM for the streaming ring buffer and pl_mpeg decoder state
- SD card data loading (FAT32) for the video files
- 6×6×6 sRGB-correct paletted output with 4×4 Bayer ordered dither
- File browser UI with self-contained 5×7 font, scroll, paged navigation
- PS/2 keyboard support (always on)
- Optional USB HID host (USB keyboard alongside PS/2)
- All-I-frame MPEG-1 conversion script (`tools/convert_video.sh`) tuned for RP2350's realtime decode budget

## Hardware Requirements

- **Raspberry Pi Pico 2** (RP2350) or compatible board
- **8 MB QSPI PSRAM** (mandatory — used for the streaming ring buffer)
- **HDMI connector** (driven via 270 Ω resistors directly from the GPIO; no encoder needed)
- **SD card module** (SPI mode)
- **PS/2 keyboard** (or a USB keyboard if `USB_HID=1`)
- **I2S DAC module** (e.g. TDA1387, PCM5102) for the standard build, OR an HDMI audio sink for the HDMI_ALT build

> **Note:** When `USB_HID=1` is set, the native USB port is used as a USB HID host for keyboard input. USB serial console (CDC) is disabled in this mode; use UART for debug output. PS/2 stays enabled either way.

### PSRAM

frank-video requires 8 MB PSRAM for the 256 KB pl_mpeg streaming ring buffer plus internal demux buffers that grow at runtime. Without PSRAM, the player cannot stream video. You can obtain PSRAM-equipped hardware in several ways:

1. **Solder a PSRAM chip** on top of the Flash chip on a Pico 2 clone (SOP-8 flash chips are only available on clones, not the original Pico 2).
2. **Build a [Nyx 2](https://rh1.tech/projects/nyx?area=nyx2)** – a DIY RP2350 board with integrated PSRAM.
3. **Purchase a [Pimoroni Pico Plus 2](https://shop.pimoroni.com/products/pimoroni-pico-plus-2?variant=42092668289107)** – a ready-made Pico 2 with 8 MB PSRAM.

## Pin Assignments (M1 / M2)

> The PSRAM CS pin is auto-detected from the RP2350 chip package at boot:
> - **RP2350B**: GPIO 47 (M1 and M2)
> - **RP2350A**: GPIO 19 (M1) or GPIO 8 (M2)

| Function | Signal      | M1 GPIO | M2 GPIO |
|----------|-------------|:-------:|:-------:|
| HDMI     | CLK−        | 6       | 12      |
| HDMI     | CLK+        | 7       | 13      |
| HDMI     | D0−         | 8       | 14      |
| HDMI     | D0+         | 9       | 15      |
| HDMI     | D1−         | 10      | 16      |
| HDMI     | D1+         | 11      | 17      |
| HDMI     | D2−         | 12      | 18      |
| HDMI     | D2+         | 13      | 19      |
| SD Card  | CLK         | 2       | 6       |
| SD Card  | MOSI        | 3       | 7       |
| SD Card  | MISO        | 4       | 4       |
| SD Card  | CS          | 5       | 5       |
| PS/2 Keyboard | CLK    | 0       | 2       |
| PS/2 Keyboard | DATA   | 1       | 3       |
| I2S Audio | DATA       | 26      | 9       |
| I2S Audio | BCLK/LRCLK | 27      | 10      |

> **HDMI** pins are driven directly through 270 Ω series resistors. No HDMI encoder is needed.
> **HDMI_ALT** (HSTX) requires M2 only — HSTX is hard-wired to GPIO 12-19, which only the M2 layout exposes for HDMI.
> When the HDMI_ALT build is used, audio is embedded in the HDMI stream and the I2S pins are unused.

## How to Use

### SD Card Setup

1. Format an SD card as **FAT32**.
2. Copy your `.mpg` files to a `/video` directory on the card (or place them at the root — both work).
3. Insert the SD card and power on the device.

### Converting Source Material

frank-video plays MPEG-1 streams encoded for the RP2350's decode budget. Use the bundled converter:

```bash
./tools/convert_video.sh input.mp4 [output.mpg]
```

The converter produces:
- 320×240 video at 24 fps, all I-frames (`-q:v 14 -g 1 -bf 0`)
- 32 kHz mono MPEG-1 Layer-II audio at 64 kbps
- Letterboxed/pillarboxed to fit 320×240 with black bars

If no output path is given, the file is written next to the input with a `.mpg` extension.

### File Browser

On boot, frank-video shows a file browser listing the `.mpg` files found on the SD card. Use the arrow keys to navigate, **Enter** to start playback, **ESC** during playback to stop and return to the browser.

If no SD card is inserted, an error message is shown.

### Controls

| Action | Key |
|--------|-----|
| Navigate file list | ↑ ↓ |
| Page up / down | PgUp / PgDn |
| Jump to first / last | Home / End |
| Start playback | Enter |
| Stop playback | ESC |
| Pause / resume | Space |

## Building

### Prerequisites

1. Install the [Raspberry Pi Pico SDK](https://github.com/raspberrypi/pico-sdk) (version 2.0+).
2. Set the environment variable: `export PICO_SDK_PATH=/path/to/pico-sdk`.
3. Install the ARM GCC toolchain (`arm-none-eabi-gcc`).

### Build

```bash
git clone https://github.com/rh1tech/frank-video.git
cd frank-video
./build.sh                                          # Default: M2, 504/166/66 MHz, PIO HDMI, USB CDC
USB_HID=1 ./build.sh                                # Enable USB HID host (debug over UART)
USB_HID=1 ./build.sh -DHDMI_HSTX=ON                 # M2 with HSTX HDMI + HDMI audio
./build.sh -DBOARD_VARIANT=M1                       # M1 layout
```

Output: `build/frank-video.uf2`

### Build Options

- **`BOARD_VARIANT`** (default `M2`): `M1` or `M2` — selects the GPIO map.
- **`CPU_SPEED`** (default `504`): one of `252` / `378` / `504` MHz. Higher is faster but needs higher core voltage.
- **`PSRAM_SPEED`** (default `166`): one of `84` / `100` / `133` / `166` MHz. Lower is more conservative for noisy boards.
- **`FLASH_SPEED`** (default `66`): the QSPI flash maximum frequency in MHz.
- **`HDMI_HSTX`** (default `OFF`): `ON` enables the RP2350 native HSTX HDMI driver with HDMI audio over Data Islands. Requires `BOARD_VARIANT=M2` (HSTX is hard-wired to GPIO 12-19).
- **`USB_HID=1`** (env var): enable USB HID keyboard host. Disables the USB CDC stdio console; debug output is routed to UART.

### Release Build

Build all release variants (M1/HDMI, M2/HDMI, M2/HDMI_ALT — all at 504/166/66 with USB_HID=1) at once:

```bash
./release.sh             # Interactive version prompt
./release.sh 1.01        # Specify version
```

This produces three UF2 files in `release/`:

- `m1p2_frank-video_<version>.uf2` — M1 board, legacy PIO HDMI + I2S audio
- `m2p2_frank-video_<version>.uf2` — M2 board, legacy PIO HDMI + I2S audio
- `m2p2alt_frank-video_<version>.uf2` — M2 board, HSTX HDMI with HDMI audio (no I2S needed)

### Flashing

```bash
# With device in BOOTSEL mode:
./flash.sh

# Or manually:
picotool load build/frank-video.uf2
```

## Troubleshooting

### Crashes or HDMI dropouts

Lower the PSRAM and flash speeds:

```bash
./build.sh -DPSRAM_SPEED=100 -DFLASH_SPEED=50
```

PSRAM at 166 MHz is right at the edge of the SPI margin; a small drop to 100 MHz is usually enough on imperfect boards.

### Audio drifts behind video on long playback

Make sure your `.mpg` was produced by the bundled `tools/convert_video.sh`. The 32 kHz Layer-II encoding is matched to the player's drift-compensation logic; arbitrary MPEG-1 streams (especially those with B-frames or sample rates other than 32/44.1/48 kHz) may not stay in sync.

### Heavy-frame freezes on busy scenes

The default `-q:v 14` quality target keeps per-frame decode cost bounded. If you tweaked the converter to a lower quantiser (`-q:v 6` or so) and see freezes during high-detail scenes, raise the quantiser back toward 14. The player's IDCT cost scales with the number of non-zero AC coefficients, not bytes per frame.

## License

Copyright (c) 2026 Mikhail Matveev <<xtreme@rh1.tech>>

frank-video's RP2350 platform layer, drivers, and integration code added by Mikhail Matveev are licensed under the GNU General Public License v3.0 — see [LICENSE](LICENSE) for details.

frank-video is built on top of [pl_mpeg](https://github.com/phoboslab/pl_mpeg) (MIT) and various Pico-side drivers ported from sister rh1tech projects. The combined work is distributed under GPL-3.0-or-later.

> **Note:** No copyrighted video material is included or redistributed. You must convert your own video files using `tools/convert_video.sh` and copy them to the SD card.

## Acknowledgments

This project builds on a long chain of open-source work. frank-video is the RP2350 platform port; the MPEG-1 decoder and most of the heavy lifting come from upstream projects.

| Project | Author(s) | License | Used For |
|---------|-----------|---------|----------|
| [pl_mpeg](https://github.com/phoboslab/pl_mpeg) | Dominic Szablewski | MIT | MPEG-1 video + audio decoder |
| [pico-spec](https://github.com/DnCraptor/pico-spec) | DnCraptor | GPL-3.0 | PIO HDMI driver foundation |
| [pico_hdmi](https://github.com/fliperama86/pico_hdmi) | fliperama86 | Unlicense | HSTX HDMI driver foundation (HDMI_ALT build) |
| [FatFS](http://elm-chan.org/fsw/ff/) | ChaN | Custom permissive | FAT32 filesystem for SD card |
| [pico_fatfs_test](https://github.com/elehobica/pico_fatfs_test) | Elehobica | BSD-2-Clause | SD card PIO-SPI driver |
| [TinyUSB](https://github.com/hathach/tinyusb) | Ha Thach | MIT | USB HID host driver (when `USB_HID=1`) |
| [Raspberry Pi Pico SDK](https://github.com/raspberrypi/pico-sdk) | Raspberry Pi Foundation | BSD-3-Clause | Hardware abstraction layer |

Special thanks to:

- **Dominic Szablewski (phoboslab)** for pl_mpeg, the single-header MPEG-1 decoder that makes this whole project tractable.
- **DnCraptor** for the pico-spec project — the PIO HDMI driver patterns used here trace back to that work.
- **fliperama86** for pico_hdmi — the HSTX driver and HDMI Data Island packet code is adapted from that project.
- **shuichitakano** for the original pico-infones port that inspired the Pico-side audio approach across the rh1tech projects.
- **ChaN** for FatFS, the lifeblood of every SD-card-using Pico project.
- **Elehobica** for the PIO-SPI SD card driver.
- The **Murmulator community** for hardware designs, pinouts, and testing.
- The **Raspberry Pi Foundation** for the RP2350 and Pico SDK.

## Author

Mikhail Matveev <<xtreme@rh1.tech>>

[https://rh1.tech](https://rh1.tech) | [GitHub](https://github.com/rh1tech/frank-video)
