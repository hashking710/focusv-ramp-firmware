#!/bin/sh
# Host tests for the shared ramp code. Needs Docker.
set -e
docker run --rm -v "$(pwd):/r" -w /r gcc:13 sh -c \n  "gcc -std=gnu99 -Wall -Werror -Itools/hosttest -Icommon common/*.c tools/hosttest/sim.c -o /tmp/t && /tmp/t"
