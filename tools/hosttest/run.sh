#!/bin/sh
# Host tests for the shared ramp code (and the Aeris button hook). Needs Docker.
set -e
docker run --rm -v "$(pwd):/r" -w /r gcc:13 sh -c \
  "gcc -std=gnu99 -Wall -Werror -Itools/hosttest -Icommon common/*.c aeris/ramp_event.c tools/hosttest/sim.c -o /tmp/t && /tmp/t"
