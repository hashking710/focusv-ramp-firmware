← [Back to main README](../README.md)

# Carta Sport ramp patch

> [!WARNING]
> **Not yet tested on real hardware.** It passes every build check and an independent
> disassembler cross-check; flash only a device you can recover over SWire. See the
> [main README](../README.md).

Runs a Terpline-uploaded ramp of up to 5 stages on the device itself. It shows progress on the
Sport's 5 RGB LEDs, the same way as the Aeris:

- **How many are lit** shows progress through the ramp (1 to 5).
- **Their colour** is the measured temperature, on this ramp's own scale.

LEDs follow the user's LED setting (0x842694 + 15, 0 = off), which the patch reads and never
writes: with it off, the stock effects keep both lights. The stock push routine `0x8cf8` applies
dim mode (four presses + hold) and low battery itself.

**Stopping:** a single click during a session already stops it in stock firmware. In the event
consumer at 0x45cc, event 11 stops unconditionally, and events 7, 15 and 19 stop while heating.
So the patch adds no button hook.

## Firmware build

```text
Device:  Carta Sport
Build:   PROD-030426
Body:    90,860 bytes (after the 40-byte header), SHA-1 4b57f086a175...
```

`apply_patch.py` patches only this exact build. There is no override.

## Patch sites (5, written and checked by `tools/build.py`)

| Site | Stock | Replaced with |
| --- | --- | --- |
| 0x58b0 | call to orchestrator `0x7c00` (the only caller) | `ramp_trampoline`: stock tick, the ramp, then the LEDs |
| 0xb002 | marker-byte load before the A5/AF/66 chain | `ramp_marker_entry`: waypoint upload markers |
| 0x58a8 | call to button-event consumer `0x45cc` (the only caller) | `ramp_event_entry`: the preset picker, then the stock consumer |
| 0x57ee | call to the LED effect dispatcher `0x8ff8` (the only caller) | `ramp_led_entry`: button light and LEDs while a ramp or the picker owns them, else the stock effects |
| 0xa9ea | stock send of the `0xAA` dab-counter reply (notify `0xec3c`) | `ramp_announce_entry`: sends it unchanged, then announces the patch (`0xBC`) |

The code goes at flash 0x18000 and runs at 0x18028, with 12 KB to grow into, and the output image
ends at 0x1b000 (110,596 bytes with the trailer). The ramp store has its own sector at 0x70000,
outside both OTA banks: stock erases the bank it isn't running from at every boot, so a store
inside the image would be lost after an OTA install. Every stock erase site is traced in
[`device.h`](device.h); nothing stock touches 0x61000-0x73fff. The stock
OTA accepts at most 124 KB: `main()` calls the SDK's `bls_ota_set_fwSize_and_fwBootAddr(124,
0x20000)`, and the OTA start rejects a larger header length. An earlier version ended the image at
0x20000 (128 KB + 4), which the stock OTA would have refused. An earlier version still put the
waypoint store at 0x18000, the same address as its own code, so the first save would have erased
the patch.

## Switching between Terpline and Focus V on the device

On and idle, press the button five times and hold the fifth: the patch switches between ramp mode
(Terpline) and stock mode (Focus V, the patch a pass-through) and remembers it. The lights confirm
it for 1.5 s -- the logo's flame colours across the LEDs and a green button for Terpline, white for
Focus V -- if your LEDs are on.

The scanner posts its hold (15) at 200 scans whatever the press count; stock's own multi-press holds
use counts 2, 4 and 7, a hold when idle does nothing in stock, and a release after a long press
clears the count, so the five-click power off never follows. The hook takes a hold with the count
at 5 (`ramp_event.c`, checked before the stock-mode pass-through) and hands it on as a press.
During a session the hold goes to stock, which stops the session. A picker left open closes.

## Preset picker and on/off

With the device fully on (awake, not in standby), idle (no session) and the LEDs on, **hold the
button** (a single press, held until
the stock long hold registers: 200 button scans) to open the picker. A hold whose press woke the device from standby
only wakes it, as in stock: the hook notes at the press (16, before the consumer wakes the device)
whether it was already awake. A hold that ends a multi-press gesture (two presses + hold, four presses + hold for dim mode,
seven presses + hold) stays the stock gesture. Inside it:

- **single click**: next built-in preset (Flavor first, Rosin, Balanced, Sauce; Balanced by default).
  The **button light** shows the preset's colour (cyan, green, amber, magenta), and the LEDs light up
  to its position in that colour;
- **triple click**: switch the ramp system on or off -- with it off, the button light is red and
  every LED is dim red;
- **hold**: leave, saving the choice in flash.

The picker opens even when the system is off, so it can be switched back on. While the system is
off, no ramp arms and no stage or offset is saved. The picker closes, passing the event on, as soon
as the device leaves the idle state (sleep, standby, a session). Power off (five presses, the app's
power-off command, the firmware's own sleep request -- all event 11) and the app's start / stop /
+10 s commands always reach the stock code. The events it takes reach the stock handler only as a plain press,
which still counts as activity for the auto-off timer, so the stock gestures are unchanged outside
it: a click cycles the temperature preset, a triple
click cycles the LED preset, four clicks show the battery. In the idle state a stock hold does
nothing (it only stops a running session), so the picker's hold doesn't shadow a stock gesture.

Like stock, the picker ignores button events during the power-on transition (struct `+8` set and
`+10` == 1, which the stock consumer also checks). With the LEDs off it doesn't open at all: it
would be an invisible mode taking clicks for up to 30 s. Turn the LEDs on (triple click) to use it.

**Quick heat.** Two presses + a long hold from sleep starts the stock quick heat (UI state 7, a
session at a forced 90 °C / 193 °F that runs until a press stops it). It never arms a ramp, even from a sentinel slot.

**The button light.** During a ramp it shows the same temperature colour as the LEDs (blue at
the coolest stage through to gold at the hottest); in the picker, the preset's colour, or red when the
system is off. It's one more addressable RGB LED, sent by `0x8efc` from `0x844b12`/`0x844b0c`/`0x844b0e`
(red, green, blue). The button and the ring share one PWM output and one DMA buffer, so every stock
push runs button (`0x8efc`, which waits for its own transfer), LED rail PA0 on, ring (`0x8cf8`, which
doesn't wait) -- and so does the patch, from one place: the call of the stock LED effect dispatcher
(`0x8ff8`, one caller at `0x57ee`, every 10 ms main-loop tick). While a ramp or the picker owns the
lights, it fills and pushes both instead of running the stock effects; otherwise the dispatcher
runs unchanged, which also restores the stock colours. A stock cue or warning always plays out
first: the dispatcher's animation state at `0x844b3c` (+1 level, +5 blinks, +6 fade-in, +7
fade-out) must be idle at level 100, and its effect (+0) must not be 9, the warning flash (low
battery at session start, heater faults), before the patch draws -- so it never freezes the level
mid-blink or hides a warning. Nothing else in the image writes those fields, so this holds the
lights for a second or two at most: the three blinks at every session start, then the ramp. Both lights follow the LED setting: with LEDs off, the stock effects
keep both.

## Device-specific behaviour, confirmed from the code that uses it

- **Orchestrator `0x7c00`:**
  - reloads the target pair +0x2c (°C) / +0x2e (°F) from the active slot while +0x1 is 0
  - mode +0x6: 1 = flower, otherwise concentrate
  - ranks +0x7 / +0x8
  - countdown +0x1a, loaded from the hold tables
  - its out-of-range fallback writes 25 / 77 into +0x2a / +0x28, which proves +0x28 is °F (an
    earlier version read +0x2a as °F)
- **PID step `0x72bc`:** +0x4 == 1 means Celsius.
- **Session timer:** only runs once the target is reached, so holds are time at temperature. At zero
  it stops via 0x6a98, bumps the counters at 0x842910 (same offsets as the Aeris), arms saving
  with +31 = 200 and +32 = 250, and sets the end cue at 0x842870.
- **Flash routines:** confirmed through the firmware's own settings-save pointer table (0xc0f5 /
  0xc111 are 0xc0cc / 0xc0e8 + 0x28 with bit 0 set): read and write take (address, length, buffer),
  and erase is 0xc178.
- **Divide helper:** at 0x1529c, the same hardware-divider entry as 0x1ac on the other two devices.

Every address in [`device.h`](device.h) is listed with the stock code that proves what it means.

**Flash use, from every stock erase call:** settings at 0x40000–0x43fff, pairing at 0x74000+, and
the OTA writes the other bank (0x20000 or 0) -- nothing stock touches 0x18000–0x1afff. The store
address is physical: running from bank 0 it is the image's own erased sector; after an OTA that
lands in bank 1 (0x20000) it is that sector of the now-inactive bank 0, which the next OTA erases.
Either way a ramp starts from a fresh or an older store, both checked before use.
