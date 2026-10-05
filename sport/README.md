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
writes. The stock push routine `0x8cf8` clears the buffer to black when that setting is off, then
scales it by the user's brightness itself.

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

## Patch sites (4, written and checked by `tools/build.py`)

| Site | Stock | Replaced with |
| --- | --- | --- |
| 0x58b0 | call to orchestrator `0x7c00` (the only caller) | `ramp_trampoline`: stock tick, the ramp, then the LEDs |
| 0xb002 | marker-byte load before the A5/AF/66 chain | `ramp_marker_entry`: waypoint upload markers |
| 0x58a8 | call to button-event consumer `0x45cc` (the only caller) | `ramp_event_entry`: the preset picker, then the stock consumer |
| 0xa9ea | stock send of the `0xAA` dab-counter reply (notify `0xec3c`) | `ramp_announce_entry`: sends it unchanged, then announces the patch (`0xBC`) |

The code goes at flash 0x18000 and runs at 0x18028. The waypoint store has its own sector at
0x19000. The output image ends at 0x20000. An earlier version put the waypoint store at 0x18000,
the same address as its own code, so the first save would have erased the patch.

## Preset picker and on/off

From the idle state (no session), **hold the button** to open the picker. Inside it:

- **single click**: next built-in preset (Flavor first, Rosin, Balanced, Sauce; Balanced by default),
  shown as one lit blue LED per position, up to the chosen one;
- **triple click**: switch the ramp system on or off -- with it off, every LED is dim red;
- **hold**: leave, saving the choice in flash.

The picker opens even when the system is off, so it can be switched back on. While the system is
off, no ramp arms and no stage or offset is saved. The picker closes, passing the event on, as soon
as the device leaves the idle state (sleep, a session). Its events never reach the stock handler,
so the stock gestures are unchanged outside it: a click cycles the temperature preset, a triple
click cycles the LED preset, four clicks show the battery. In the idle state a stock hold does
nothing (it only stops a running session), so the picker's hold doesn't shadow a stock gesture.

Like stock, the picker ignores button events during the power-on transition (struct `+8` set and
`+10` == 1, which the stock consumer also checks). It follows the LED setting: with LEDs off, the
picker still works but shows nothing.

**Not done: the button light.** The control button has its own light, but the stock LED effects
write its colour and output it within the same call, every tick, so a colour the patch writes
afterwards is never shown. Driving it needs a hook on that output path; until then the patch leaves
the light alone.

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

**Not verifiable from the firmware file:** that nothing else on the device uses flash
0x18000–0x19fff, which lies past the end of the stock image. Only a hardware test confirms it.
