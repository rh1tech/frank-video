#!/bin/bash
# Convert a video to a 320x240 MPEG-1 stream that the RP2350 can decode in
# real time.
#
# The pl_mpeg path on this hardware is fast on intra-only streams and
# quickly falls behind on streams with B-frames (motion compensation is
# the dominant cost, and audio decode runs synchronously in the same
# loop). Encode parameters here are picked to keep the realtime budget:
#
#   -g 1 -bf 0      : all I-frames, no P, no B. Each frame decodes in
#                     isolation, no reference-frame chasing, so peak
#                     decode cost is roughly the average decode cost.
#   320x240         : display-native, no runtime scaling.
#   24 fps          : matches typical movie source rate.
#   -q:v 6          : aggressive quality target for I-frame-only
#                     content. Files run larger than predictive encodes
#                     but decode is what we are budget-limited on, not
#                     SD bandwidth.
#   yuv420p         : pl_mpeg expects 4:2:0 chroma.
#   Audio: 32000 Hz mono MP2 @ 64 kbps. The lowest sample rate that
#                     selects MPEG-1 Layer-II (rather than MPEG-2 LSF);
#                     pl_mpeg's plm_get_samplerate() can fail to pick
#                     up MPEG-2 audio at startup before the audio_buffer
#                     has been primed, which silently disables audio.
#                     32 kHz mono saves ~30% of the synthesis-filter
#                     cost vs the source 48 kHz stereo and is plenty
#                     for "smooth and intelligible" voice + score.
#   Letterbox       : Sintel is 1280x544 (cinemascope). Scale to 320 wide,
#                     keep aspect, pad to 320x240 with black bars.
#
# Usage:   ./convert_video.sh input.mkv [output.mpg]
# Example: ./convert_video.sh ~/Documents/Sintel.2010.720p.mkv \
#              sdcard/video/sintel.mpg

set -e

if [ -z "$1" ]; then
    echo "Usage: $0 <input_video> [output.mpg]" >&2
    echo "Converts a video to 320x240 MPEG-1 optimised for frank-video." >&2
    exit 1
fi

INPUT="$1"
if [ ! -f "$INPUT" ]; then
    echo "Error: '$INPUT' not found" >&2
    exit 1
fi

if [ -n "$2" ]; then
    OUTPUT="$2"
else
    DIR="$(dirname "$INPUT")"
    BASE="$(basename "$INPUT" | sed 's/\.[^.]*$//')"
    OUTPUT="${DIR}/${BASE}.mpg"
fi

# scale=320:240:force_original_aspect_ratio=decrease keeps aspect, fits
# inside 320x240. pad=320:240:(ow-iw)/2:(oh-ih)/2 letterboxes/pillarboxes
# whatever's left over with black. setsar=1 tells the player the pixels
# are square (otherwise pl_mpeg would carry over the source's anamorphic
# SAR and the frame would render too wide).
VFILTER="scale=320:240:force_original_aspect_ratio=decrease,pad=320:240:(ow-iw)/2:(oh-ih)/2,setsar=1"

ffmpeg -y -i "$INPUT" \
    -vf "$VFILTER" \
    -r 24 \
    -c:v mpeg1video -pix_fmt yuv420p \
    -q:v 6 -g 1 -bf 0 \
    -c:a mp2 -b:a 64k -ar 32000 -ac 1 \
    -f mpeg "$OUTPUT"

echo "Wrote: $OUTPUT"
ls -lh "$OUTPUT"
