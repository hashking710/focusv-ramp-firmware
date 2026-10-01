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
- **Closing a "reasoned, not single-instruction-confirmed" gap** — each device README calls these
  out specifically (e.g. the live PID target field on Aeris/Sport). Turning one of these into a
  fully-traced, single-instruction-confirmed fact is a welcome, scoped contribution on its own.

## Verification bar for any PR that changes what a patch sends to a device

This isn't optional process for its own sake — every item below exists because skipping it is
exactly how a wrong address ends up in a merged patch:

1. Confirm the function/address you're relying on exists, by address, in the **exact** firmware
   build your patch targets — queried directly against that build's own disassembly. Don't carry an
   address over from a different build (even one that looks similar) without re-confirming it.
2. State in the PR description, plainly, what's confirmed vs. inferred vs. guessed — the existing
   device READMEs do this with a confirmed/unconfirmed marker per patch site; match that.
3. If you're changing a patch site's bytes: show the original bytes being replaced, matched against
   the real binary, and the new bytes independently re-disassembled (not just "it compiled") to
   confirm they land on the intended target.
4. Run the device's patcher script against a reconstructed copy of the real stock file and confirm
   the output is what you expect — byte-for-byte, not "looks about right."
5. If you can't clear one of the above for a given claim, say so in the PR rather than rounding up
   to "confirmed." A PR that says "I believe this is right but haven't independently re-verified
   address X" is more useful, and much safer to review, than one that states it as settled fact.

## Looking for something to work on?

All three devices (Carta 2, Aeris, Carta Sport) have a complete, software-verified patch — real
hardware access is now the main thing this project needs, not more static analysis. Each device
folder's README documents exactly what's confirmed vs. reasoned-from-converging-facts vs.
genuinely unverifiable-from-the-dump for that device specifically; the "reasoned" and
"unverifiable" items are the most useful things to go close if you have hardware and Ghidra access,
and real-hardware test reports are useful even without touching any code at all.

## Code of conduct

Same expectation as [focusv-ble-research](https://github.com/hashking710/focusv-ble-research/blob/main/CONTRIBUTING.md#code-of-conduct):
be straightforward about what you know versus what you're guessing, credit sources, and keep
disagreements about the evidence, not the person presenting it. Here specifically: overstating
confidence in an address is the one mistake this project cares most about avoiding.
