# focusv-ramp-firmware

![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)
![Status: active, not yet hardware-verified](https://img.shields.io/badge/status-active%2C%20not%20yet%20hardware--verified-yellow.svg)
![Not affiliated with Focus V](https://img.shields.io/badge/affiliation-independent%2C%20unofficial-lightgrey.svg)

Independent, hobbyist firmware patches that add an autonomous, phone-independent temperature
ramp to Focus V's Carta 2, Aeris, and Carta Sport dab rigs — save a schedule of
temperature/hold-duration steps, arm it, and the device runs through them on its own. No phone
has to stay connected once it's started.

This repo is the patch itself: the source for what gets added, the exact bytes it changes and
where, and a script that applies it to a firmware file you supply. It's a companion to
**[focusv-ble-research](https://github.com/hashking710/focusv-ble-research)**, the reverse-engineering
project that found every address and protocol detail this relies on — read that repo first if you
want to understand *how* any of this was figured out; this one is about *using* what it found.

Not affiliated with, endorsed by, or sponsored by Focus V.

## Why this exists, and how it's built to be used responsibly

Most Focus V devices ship with a software-defined "ramp" feature already — but it works by having
the official app (or [Terpline](https://terpline.app), an independent companion app for this same
hardware) send the device a new target temperature every few seconds, on a schedule. That means
the phone, and the app, has to stay connected and awake for the entire ramp. Close the tab, lock
the phone wrong, walk out of range — the ramp stops.

This patch moves that schedule *onto the device itself*. Save it once, start it with the device's
own physical button, and it keeps running with nothing else connected — the same way starting any
other saved preset already works today. It doesn't change how the device measures temperature,
limits it, or protects itself from overheating; all of that stays exactly as the manufacturer
built it. It only adds a schedule on top of the existing, unmodified temperature control.

**This is a from-scratch reimplementation, not a copy of anything Focus V wrote.** Nobody involved
in this project has ever seen Focus V's source code. Every function this patches, every struct
offset it reads, and every byte it writes was worked out independently by disassembling the public
firmware binary Focus V serves to any device that asks for an update — the same file your own
device already received, or would receive, from Focus V's own servers.

**This repo never contains Focus V's firmware, and never will.** Every folder here ships only:
our own original source code, our own compiled code blob (built from that source, with the real
TC32 toolchain, so you can verify it yourself), and a small patcher script that describes *where*
a handful of bytes change and *what* they change to — conceptually identical to an IPS/BPS patch
in the ROM-hacking world. You always supply your own copy of the stock firmware, obtained the same
way the official app itself obtains it. **See [`LEGAL.md`](LEGAL.md)** for the full reasoning —
the interoperability law this relies on, and specifically *why* shipping a patch rather than a
complete firmware build is the meaningful legal distinction here, not just a formality.

## Devices

| | Status | |
|---|---|---|
| **[`carta2/`](carta2/)** | ✅ software-verified, not yet hardware-verified — all five patch sites independently re-confirmed against the real binary | Has a screen — ramp progress shows as a live graph replacing the normal heating dial |
| **[`aeris/`](aeris/)** | Blocked on further reverse-engineering — not started | No screen — ramp progress would show as an LED color gradient instead |
| **[`carta-sport/`](carta-sport/)** | Blocked on further reverse-engineering — not started | No screen — same LED-based approach as Aeris |

Each device folder is self-contained: its own source, its own patch script, its own README with
the exact build fingerprint it was verified against and step-by-step instructions. They are
genuinely different compiled binaries with different addresses, even though all three share the
same chip, architecture, and BLE protocol family — nothing is copy-pasted between them without
being independently re-confirmed for that specific device's firmware. Aeris and Carta Sport
specifically: initial research found strong material on both devices' button/LED/struct layout,
but it also surfaced that the equivalent of the Carta 2 patch's core call site (where the
per-tick control routine gets invoked) isn't confirmed for either device yet — same category of
gap that's being fixed for Carta 2 first. Nothing gets written for either device until that's
closed the same rigorous way.

## How to flash it

Covered in detail in each device folder, but the short version: either a cheap USB-serial adapter
and the physical/UART recovery process documented in
[focusv-ble-research's hardware setup guide](https://github.com/hashking710/focusv-ble-research/blob/main/docs/hardware-setup.md),
or entirely over Bluetooth using
[`ota-flash.html`](https://github.com/hashking710/focusv-ble-research/blob/main/tools/ota-flash.html)
in the research repo, which uses the device's own real firmware-update mechanism — the same one
the official app uses for legitimate updates. The same tool, with no changes, is also how you
flash your original firmware back if you ever want to fully revert.

## Status and safety

**Software-verified, not yet hardware-verified — and "software-verified" means something specific
here, worth spelling out exactly rather than just asserting.** For a patch in this repo to be
considered done, every one of these has to be independently true, not inferred from an earlier
build or carried over from research notes without being re-checked against the exact file being
patched:

1. **Every function this patches is confirmed to exist, by address, in the exact firmware build
   the patch targets** — queried directly against that build's own disassembly, not assumed from a
   similar-looking older build. (This project has already caught and fixed real mistakes of exactly
   this kind during development — an address correct for one build silently carried over into a
   patch for a different one. Worth stating plainly rather than pretending every address was right
   the first time.)
2. **Every byte about to be changed is checked against what's actually there** immediately before
   the patch script touches it — if it doesn't match, the script stops instead of guessing.
3. **The replacement code is compiled with the real toolchain** for the target chip, and every new
   call site's target is independently re-disassembled afterward to confirm it lands exactly on the
   intended function — read back out of the actual assembled bytes, not inferred from the source.
4. **A complete patched image is built and diffed byte-for-byte** against what the actual patcher
   script (the one in this repo, not a one-off) produces from a reconstructed stock file. They have
   to match exactly.

What none of this can catch is anything that only shows up on a real device — whether the timing
feels right, whether a display element is positioned exactly where it should be, whether the
device's own session-length ceiling cuts a long ramp short. Treat the specific geometry/color/timing
constants in each device's source as considered first guesses, not measurements, until someone
(possibly you) confirms them on hardware and opens a PR.

If you do try this on a device you depend on, read that device's own README fully first — including
its reversibility section and its own current status note, since "listed in this repo" and
"finished being verified" are not the same claim until that device's README says so explicitly —
and keep your original unpatched firmware file somewhere safe. It's your way back if anything goes
wrong.

## Contributing

PRs very welcome, especially: real-hardware confirmation (does it work, does it look right, what
needed adjusting), ports to firmware builds other than the ones already confirmed, and the Aeris /
Carta Sport patches themselves if you get there first. If you're changing what a patch actually
sends to the device, please verify it the same way the existing patches were (byte-level checks
before and after, independently re-disassembled targets) rather than just "it compiled" — getting
this wrong risks leaving a real device unresponsive.
