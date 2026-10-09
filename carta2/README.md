← [Back to main README](../README.md)

# Carta 2 ("Quantum") ramp patch

> [!WARNING]
> **Not yet tested on real hardware.** It passes every build check and an independent
> disassembler cross-check; flash only a device you can recover over SWire. See the
> [main README](../README.md).

Runs a Terpline-uploaded ramp of up to 5 stages on the device itself. While a ramp runs, the
heating screen is replaced by the ramp screen, and the buttons change meaning.

![Mockup of the ramp screen](ramp-mockup.png)

*A rendered mockup, not a photo: the screen's drawing logic run against the real glyph
bitmaps from this build. Scenario: stage 3 of 5, 41 s into a 75 s stage, 68% battery. The
measured trace is simulated. Not confirmed against a real screen.*

## Firmware build

```text
Device:  Carta 2 ("Quantum")
Build:   PROD-111224
File:    143,604 bytes as downloaded (143,564-byte body + 40-byte header)
SHA-1:   a6b741dc0508... (whole file)
```

`apply_patch.py` patches only this exact file. There is no override.

## The ramp screen (240 x 240)

| Area | Left | Right (right-aligned to x 233) |
| --- | --- | --- |
| y 20 | battery % and icon (charging animation when charging) | current stage's goal / the ramp's highest goal, e.g. `400/450°F` |
| y 44 | time left, M:SS, counting down from the ramp's total | live temperature (large) |
| y 72–196 | the chart (x 6–219) | the heat meter (x 226–233) |
| y 202–224 | the Terpline flame mark + wordmark | `DABS` + the dab count |

- **The chart** is framed by a left axis, a baseline and a right axis. Time runs left to right
  across the whole ramp.
  - **The arc** is the goal: the ramp's planned temperature over time, a smooth line through each
    stage's start, coloured by heat.
  - **The trace** is the measured temperature since the ramp began, in white, one sample per
    column. Its newest point bounces with the live reading. It climbs in steps as each stage
    heats up to meet the arc. Pressing **−** rewinds it along with the ramp.
- **The Terpline mark** sits under the chart: the flame logo and wordmark in four flat brand
  colours, stored as 266 rectangle runs (806 bytes), generated from the logo by
  [`tools/make_logo.py`](../tools/make_logo.py).
- **The dab count** is the same per-mode counter the stock screen shows (flower total with a
  flower atomizer, concentrate total with a concentrate one), right-aligned to the same edge as
  the temperatures, and it ticks up as soon as a ramp qualifies (stage 2 and 20 s at temperature).
- **The heat meter** is a column of 10 circles, blue at the bottom through green, yellow and
  orange to red at the top, lit up to the live temperature.

Both the chart's height and the meter use this ramp's own range, never an absolute degree
scale: from its coolest stage minus a margin, up to its hottest. So every ramp fills the box the
same way.

During a ramp the stock bottom row -- dab counter, mode icon, status icon and READY banner
(y 193–225) -- is hidden so the chart and logo can use it; the dab count and the ready cue still
work, they just aren't drawn until the ramp ends.

Every stock element whose space this uses is hidden only while a ramp is active, at every one of
its call sites. When no ramp is active, every hook calls the original function, unchanged. The
drawing primitives were decompiled before use: `fill_rect` (0x74d0) and `blit` (0x7e2c) both
draw (w+1) x (h+1) pixels, opaquely.

## Buttons during a ramp

The patch hooks the only call to the stock button-event consumer (0x5618):

- **main button, single click:** stop, at any stage, through the stock heating screen's own stop path
  (screen 1, handler `0x5708` -> `0x97f0`). An earlier version forced screen 5 instead -- from a
  screen table read 0x28 bytes late -- which is the edit screen, so a click didn't stop the ramp.
- **+ / − short press:** next / previous stage
- **+ / − held, double click:** ignored. Auto-repeat would skip every stage, and the stock +10 s
  would rewind the ramp clock.

Outside a ramp, every event goes to the stock consumer unchanged.

## Preset picker and on/off

From the idle screen, with no session, **hold −** to open the picker. A box over the stock target
line shows the selected preset's number and a row of six markers. Inside it:

- **+ / −**: next / previous built-in preset;
- **double click**: switch the ramp system on or off -- with it off, the number and the selected
  marker turn orange;
- **click**: leave, saving the choice in flash. The stock idle screen is redrawn.

The box is redrawn on every change, from the event hook, and again whenever stock redraws the idle
screen. The picker opens even when the system is off, so it can be switched back on; while off, no
ramp arms and no stage or offset is saved. It closes, passing the event on, as soon as the screen
leaves idle. Five clicks (power on / off) and the app's start / stop / +10 s always reach the stock
code.

> [!WARNING]
> **Known issue -- the picker's entry point needs a Carta 2 pass.** The picker treats screen 0 as
> idle, but in the stock consumer's screen table (pointer `0x1a3c0`, a runtime address: the table is
> at disassembly `0x1a398`) screen 0 is the off / sleep state -- it only handles five clicks (power
> on), the press event and two system events. The home screen is 4 / 14, and depending on a saved
> setting (`+0x82`) a press there either starts a session at once or moves to menu screen 12, so a
> held − never arrives on home either. As built, the picker can only open while the screen is off,
> where it's invisible. It can't block power-on (five clicks always pass), but it doesn't work as
> described above until it's redesigned against the home and menu screens, ideally with the Carta 2
> open in Ghidra and on hardware.

The earlier on/off gesture, + and − held together, is gone: it counted any − press followed by any +
press, so stepping through presets could switch the system off.

## Patch sites (33, all written and checked by `tools/build.py`)

| Site(s) | Stock call | Replaced with |
| --- | --- | --- |
| 0x6e2e | orchestrator `0xaf2c` (the only caller) | `ramp_trampoline`: stock tick, then the ramp |
| 0x11d96 | marker-byte load before the A5/AF/66 chain | `ramp_marker_entry`: waypoint upload markers |
| 0x6d0c | event consumer `0x5618` (the only caller) | `ramp_event_entry` |
| 0x11562 | stock send of the `0xAA` dab-counter reply (notify `0x15a34`) | `ramp_announce_entry`: sends it unchanged, then announces the patch (`0xBC`) |
| 0xfa38 / 0xf546 | `0xd3c0` live temperature (full / view) | ramp screen |
| 0xfa3c / 0xf39c | `0xdcac` session countdown (full / view) | ramp screen |
| 7 sites | `0xce70` target | hidden during a ramp |
| 7 sites | `0xcf58` slot hold time | hidden during a ramp |
| 3 sites | `0xdb40` battery | hidden during a ramp (drawn by the ramp screen) |
| 2 sites each | `0xd048` dab counter, `0xe42c` mode icon, `0xe300` status icon, `0xe2b4` READY banner | hidden during a ramp |

The code goes at flash 0x30000 and runs at 0x30028. The waypoint store has its own sector at
0x32000. The output image ends at 0x33000.

Every address in [`device.h`](device.h) is listed with the stock code that proves what it means.

## Device-specific behaviour

- The stock session clock runs from the start of each stage, so the patch gives back every second
  spent heating: each hold is time at temperature, the same as on Aeris and Sport. If a stage hasn't
  reported reached after 120 s of heating, its hold counts down anyway, so a ramp can't stall.
- The dab counter base is 0x8430e0, and saving is armed by setting +31 = 50 and +33 = 0, exactly as
  the stock timer does when a session finishes.
- Flash writes go through `0x19a78`. An earlier version used `0x19308`, which is the PROD-071024
  address and lands mid-function in this build.
