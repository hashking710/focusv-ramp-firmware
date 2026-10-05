#!/bin/sh
# Carta 2 screen test: real display code against your own stock image (not in this repo).
# Usage: sh tools/hosttest/run_display.sh path/to/carta2-PROD-111224.bin
set -e
IMG=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
mkdir -p tools/hosttest/out
docker run --rm -v "$(pwd):/r" -v "$IMG:/img.bin:ro" -w /r gcc:13 sh -c \
  "gcc -std=gnu99 -Wall -Werror -include tools/hosttest/display_hdr.h -Icarta2 -Icommon common/*.c carta2/ramp_display.c carta2/ramp_input.c tools/hosttest/display.c -o /tmp/d && /tmp/d /img.bin tools/hosttest/out"
