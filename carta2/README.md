← [Back to main README](../README.md)

# Carta 2 ("Quantum") ramp patch

> [!CAUTION]
> **Do not flash this patch.** It has not run on real hardware. Earlier published versions had two
> crash-on-boot bugs; see the [main README](../README.md). They're fixed in this source, but only
> a bench test can confirm it works.

Runs a Terpline-uploaded ramp of up to 5 stages on the device itself. While a ramp runs, the
heating screen is replaced by the ramp screen, and the buttons change meaning.

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
| y 20 | battery % and icon (charging animation when charging) | current stage's target + unit, underlined in the stage's colour |
| y 44 | time left, M:SS | live temperature (large) |
| y 78–191 | the whole ramp | |
| y 193+ | stock dab counter / mode icon, or the READY banner | |

In the ramp chart, each stage is one column, and its width is that stage's share of the total
time. Finished stages are filled, the current stage fills as it progresses, and upcoming stages
are outlined. A dashed line marks the current target, and a white line marks the measured
temperature. Colours run blue → violet → magenta → orange → gold across this ramp's own coolest
to hottest stage.

Every stock element whose space this uses is hidden only while a ramp is active, at every one of
its call sites. When no ramp is active, every hook calls the original function, unchanged. The
drawing primitives were decompiled before use: `fill_rect` (0x74d0) and `blit` (0x7e2c) both
draw (w+1) x (h+1) pixels, opaquely.

## Buttons during a ramp

The patch hooks the only call to the stock button-event consumer (0x5618):

- **main button, single click:** stop, at any stage, through the stock heating screen's own stop path
- **+ / − short press:** next / previous stage
- **+ / − held, double click:** ignored. Auto-repeat would skip every stage, and the stock +10 s
  would rewind the ramp clock.

Outside a ramp, every event goes to the stock consumer unchanged.

## Patch sites (24, all written and checked by `tools/build.py`)

| Site(s) | Stock call | Replaced with |
| --- | --- | --- |
| 0x6e2e | orchestrator `0xaf2c` (the only caller) | `ramp_trampoline`: stock tick, then the ramp |
| 0x11d96 | marker-byte load before the A5/AF/66 chain | `ramp_marker_entry`: waypoint upload markers |
| 0x6d0c | event consumer `0x5618` (the only caller) | `ramp_event_entry` |
| 0xfa38 / 0xf546 | `0xd3c0` live temperature (full / view) | ramp screen |
| 0xfa3c / 0xf39c | `0xdcac` session countdown (full / view) | ramp screen |
| 7 sites | `0xce70` target | hidden during a ramp |
| 7 sites | `0xcf58` slot hold time | hidden during a ramp |
| 3 sites | `0xdb40` battery | hidden during a ramp (drawn by the ramp screen) |

The code goes at flash 0x30000 and runs at 0x30028. The waypoint store has its own sector at
0x32000. The output image ends at 0x33000.

Every address in [`device.h`](device.h) is listed with the stock code that proves what it means.

## Device-specific behaviour

- The stock session clock runs from the start of each stage, so each hold is wall-clock time.
- The dab counter base is 0x8430e0, and saving is armed by setting +31 = 50 and +33 = 0, exactly as
  the stock timer does when a session finishes.
- Flash writes go through `0x19a78`. An earlier version used `0x19308`, which is the PROD-071024
  address and lands mid-function in this build.
