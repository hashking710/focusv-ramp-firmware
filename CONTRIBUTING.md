← [Back to README](README.md)

# Contributing

This project patches real firmware on real hardware — the bar for "ready to merge" is
meaningfully higher than for a documentation repo. A wrong address here doesn't just produce a
wrong doc page, it risks leaving someone's device unresponsive.

## What's useful

- **Real-hardware confirmation** for any patch currently marked software-verified-only: does it
  actually work, does the display/LED look right, did anything need adjusting from what the source
  predicted.
- **A corrected address**, with the Ghidra evidence behind it, for anything currently marked
  unconfirmed or wrong in a device's README.
- **A port to a different firmware build** than the one a given device folder currently targets —
  as its own addition (a new build-fingerprint section with its own addresses), not a replacement
  of the existing one, since different builds need independently-confirmed addresses.

## Verification bar for any PR that changes what a patch does on a device

This isn't process for its own sake — every item below exists because skipping it is exactly how
a wrong address, or a right address with the wrong meaning, ends up in a merged patch:

1. Confirm every address you rely on in the **exact** firmware build your patch targets, by reading
   the stock code that *consumes* it — not where a value seems to come from, and never carried over
   from a different build. Add it to that device's `device.h` with the evidence in a comment.
2. Calls into stock code go through `STOCK_FN` (disassembly address + 0x28, bit 0 set). Decompile any
   stock primitive before relying on its argument format.
3. `tools/build.py <device> --firmware <file>` must report **ALL CHECKS PASSED**, unchanged — it
   regenerates the patch table, the blob hash and runs the end-to-end and refusal checks.
4. State in the PR, plainly, what's confirmed vs. inferred, and whether it ran on hardware. A PR that
   says "I believe this is right but haven't verified X" is far safer to review than one that rounds
   up to "confirmed."

## Looking for something to work on?

Bench testing on real hardware — with SWire recovery at hand — is what this project needs most;
see the main README's warning. Each device README lists what can't be verified from the firmware
file alone.

## Code of conduct

Same expectation as [focusv-ble-research](https://github.com/hashking710/focusv-ble-research/blob/master/CONTRIBUTING.md#code-of-conduct):
be straightforward about what you know versus what you're guessing, credit sources, and keep
disagreements about the evidence, not the person presenting it. Here specifically: overstating
confidence in an address is the one mistake this project cares most about avoiding.
