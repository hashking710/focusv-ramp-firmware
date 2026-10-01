← [Back to main README](../README.md)

# Carta 2 ("Quantum") ramp patch

Adds an autonomous, on-device temperature ramp to the Carta 2's stock firmware: save up to
5 temperature/hold-duration waypoints, arm the ramp, and the device walks through them on its
own — no phone connected, nothing to keep open. While a ramp is running, the normal single-number
heating screen is replaced with a live graph of the plan vs. the measured temperature.

**Status: all five patch sites independently re-confirmed against the real firmware build below —
built, verified at the byte level, not yet installed on real hardware.** This patch previously
shipped (briefly, never flashed by anyone) with two of its five addresses silently wrong — carried
over from an older, different firmware build without being re-checked, despite being labeled
"confirmed." That was caught by re-verifying every single address directly against the real
binary rather than trusting an earlier pass's label, and both are now fixed and independently
re-confirmed the same rigorous way as the other three. The full story, including exactly how each
one was found and fixed, is below rather than quietly smoothed over — read "How this was verified"
if you want the details before trusting this with a device you depend on. "Verified in software"
and "confirmed safe to flash" are still different claims, and only the second one matters once
you're about to do it to a device you use.

## What it actually changes

Five same-length call-site patches inside the stock firmware — each one replaces an existing
4- or 8-byte instruction sequence with another one of the identical length, pointing at new code
instead. Nothing is inserted, nothing is deleted, no other byte in the image moves. The full
reasoning behind each design choice (why a temperature gradient instead of a user-picked color,
why the target line is dashed, why the countdown timer and preset badge are suppressed during a
ramp, exactly which struct offsets and functions each file relies on) is written directly into the
header comments of [`ramp_tick.c`](ramp_tick.c), [`ramp_save.c`](ramp_save.c), and
[`ramp_display.c`](ramp_display.c) — kept there rather than in a separate doc so it can't drift out
of sync with the code it's explaining. See
[focusv-ble-research](https://github.com/hashking710/focusv-ble-research) for how every address and
struct offset here was originally found.

| Site | Stock function | Replaced with | What it does | Confirmed against this exact build? |
|---|---|---|---|---|
| `0x6e2e` | call to the per-tick PID/session orchestrator (`FUN_0000af2c`) | `ramp_trampoline` | Runs the original tick unmodified, then the ramp sequencer | ✅ yes — see "The PID-tick call site" below |
| `0x11d96` | the marker-byte load immediately before the stock `0xA5`/`0xAF`/`0x66` compare chain in the `0xCC` handler's marker dispatch | `ramp_marker_entry` | Replicates that load, calls `ramp_marker_dispatch` for the 5 new waypoint-save markers (`0xB1`-`0xB5`), leaves the untouched chain right after it working exactly as before for everything else | ✅ yes — see "The marker-dispatch site" below |
| `0xfa38` | main temp/gauge display (`FUN_0000d3c0`) | `ramp_temp_display` | Draws the progress graph while a ramp is active; falls through to the original dial otherwise | ✅ yes — independently re-disassembled against the real binary |
| `0xfa50` | session countdown timer (`FUN_0000d048`) | `ramp_countdown_or_skip` | Suppressed during a ramp — a ramp has its own per-stage timing, the session countdown doesn't apply | ✅ yes |
| `0xfa54` | active-preset-slot badge (`FUN_0000e42c`) | `ramp_badge_or_skip` | Suppressed during a ramp, freeing that screen space for the bigger graph | ✅ yes |

New code lives in previously-unused flash at `0x30000` (confirmed free space ahead of the OTA
staging area), sized well under the 4KB it has before the next sector. Waypoints are stored in
their own dedicated sector at `0x31000`, never touching the normal session-preset storage.

## Build fingerprint this was verified against

```
Device:  Carta 2 ("Quantum")
Build:   PROD-111224
Size:    143,604 bytes (the file exactly as downloaded, header included)
SHA-1 (first 12 of the full file): a6b741dc0508
```

`apply_patch.py` checks this automatically and refuses to touch a file that doesn't match, unless
you pass `--force`. A different build will almost certainly have these same call sites at
different addresses — forcing it through isn't "probably fine, slightly off," it's "will very
likely corrupt the image."

## How to apply it

1. **Get your own copy of the stock firmware.** This repo doesn't include it (see the main
   README for why) — pull it from Focus V's own update infrastructure, the same way the official
   app does. The research repo's protocol docs reference exactly where.
2. Run the patcher against your own file:
   ```
   python3 apply_patch.py --input your-stock-firmware.bin --output patched.bin
   ```
   It validates the file, checks every patch site's original bytes match what's expected before
   touching anything, and refuses to proceed if they don't.
3. Flash `patched.bin` to the device — see "How to flash it" below.
4. Keep your original, unpatched file. It's your way back (see "Reversibility").

## How to flash it

Two options, same device, same file format:

- **Physical/UART**, via a cheap USB-serial adapter and the SWire recovery process documented in
  [focusv-ble-research's hardware setup guide](https://github.com/hashking710/focusv-ble-research/blob/main/docs/hardware-setup.md)
  — the more battle-tested route, closer to flashing a hobbyist board than installing an app
  update.
- **Over Bluetooth**, via [`ota-flash.html`](https://github.com/hashking710/focusv-ble-research/blob/main/tools/ota-flash.html)
  in the research repo — uses the device's own real firmware-update mechanism (the same one the
  official app uses for legitimate updates), implemented from the protocol documented there. No
  install, open it in Chrome/Edge and connect. This path is newer and has seen less real-world use
  than the physical route — if you have the equipment for UART, that's the one to trust first.

## Reversibility

This patch doesn't touch anything outside the three areas above — the device's temperature
limits, safety cutoffs, and core control loop are exactly what the manufacturer shipped. Going
back to stock is the same process in reverse: flash your original, unpatched file (UART or
`ota-flash.html`, same as above). Nothing about having run this patch changes how that works.

## How this was verified

The bar for "done" on each patch site, applied consistently rather than assumed once and reused:

1. **The function being replaced is confirmed to exist, by address, in this exact build** —
   queried directly against this build's own disassembly, not carried over from a different build
   because it "should" be the same.
2. **The original bytes at the patch site are checked against the real file before anything is
   touched** (see `apply_patch.py`'s `PATCHES` table) — the script stops if they don't match.
3. **The replacement code is compiled with the real TC32 cross-compiler** for this chip
   (`ramp_firmware_v1.bin`), linked at its real injection address, and every new call site's target
   is independently re-disassembled afterward to confirm it lands exactly on the intended function
   — read back out of the actual assembled bytes, not inferred from the source.
4. **A full patched image is built and diffed byte-for-byte** against what `apply_patch.py` itself
   produces from a reconstructed copy of the real stock file, plus its waypoint-storage sector
   checked to confirm it's left in the correct erased state and its Telink CRC32 trailer recomputed
   and independently re-verified against the algorithm Focus V's own official images use (confirmed
   by matching both real downloaded firmware files' existing trailers before relying on it here).

All five patch sites above have cleared all four steps. Two of them took real additional work to
get there, worth recording in detail rather than just marking ✅ and moving on:

**The PID-tick call site.** An earlier draft had this address right (`0x6e2e`) but called the wrong
function from it — `0xad4c`, which isn't a function at all in this build, carried over from an
older firmware build where the orchestrator really did live there. Standard tooling couldn't
re-find the real call site either: this build's Ghidra auto-analysis never wraps the code around
`0x6e2e` in a named function (a known quirk with this chip's Ghidra module — see the research
repo's methodology doc), so there was no cross-reference to follow. The fix: generate, with the
real assembler, the exact correct instruction bytes a `tjl 0xaf2c` (the real orchestrator, confirmed
separately by decompile) would have at *every single possible position* in the entire 143,564-byte
firmware, then compare each one against what's actually there. Exactly one position in the whole
file matched: `0x6e2e`. The same address as before — just the wrong callee had been attached to it.

**The marker-dispatch site.** This one wasn't just a wrong address, it was the wrong *kind* of
site. The stock firmware recognizes exactly three marker values (`0xA5` start, `0xAF` stop, `0x66`
test-fire) through a fixed compare-and-branch chain, each a separate leaf. An earlier draft patched
the `0x66` leaf specifically — which works for replicating `0x66`, but means the five new
ramp-waypoint markers (`0xB1`-`0xB5`) would never reach the patch at all, since none of them match
any of the three existing comparisons; they'd just fall straight through the whole chain unseen.
The fix was architectural, not just a corrected address: intercept the marker *load*, which runs
unconditionally before any of the three comparisons, rather than any one leaf of the chain. See
`ramp_marker_entry.s` for exactly how that's done without disturbing the untouched stock chain
immediately after it.

What none of this covers: actually running on a real device. Software verification catches "is this
byte-accurate against what we confirmed," not "does the display look right," "does the timing feel
right," or anything else that only shows up on a real screen. Geometry, colors, and timing constants
in `ramp_display.c` are informed best guesses, not measurements — flagged as such inline in the
source.
