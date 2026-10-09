← [Back to main README](../README.md)

# Aeris ramp patch

> [!WARNING]
> **Not yet tested on real hardware.** It passes every build check and an independent
> disassembler cross-check; flash only a device you can recover over SWire. See the
> [main README](../README.md).

Runs a Terpline-uploaded ramp of up to 5 stages on the device itself. The Aeris has no screen, so
progress shows on its 4 RGB LEDs:

- **How many are lit** shows progress through the ramp (1 to 4).
- **Their colour** is the measured temperature, on a blue → violet → magenta → orange → gold scale
  spanning this ramp's own coolest to hottest stage.

LEDs follow the user's own LED setting. With LEDs switched off, the patch draws nothing; it reads
that setting and never writes it. An earlier version force-enabled the LEDs every tick.

**Stopping:** a single press or hold during a session already stops it in stock firmware (0x4ff4 →
0x50c4 → 0x7200), so the patch adds no button hook.

## Firmware build

```text
Device:  Aeris
Build:   PROD-111224
Body:    80,684 bytes (after the 40-byte header), SHA-1 7e3569fabd06...
```

`apply_patch.py` patches only this exact build. There is no override.

## Patch sites (5, written and checked by `tools/build.py`)

| Site | Stock | Replaced with |
| --- | --- | --- |
| 0x6464 | call to orchestrator `0x8154` (the only caller) | `ramp_trampoline`: stock tick, the ramp, then the LEDs |
| 0xb490 | marker-byte load before the A5/AF/66 chain | `ramp_marker_entry`: waypoint upload markers |
| 0x645c | call to button-event consumer `0x4ee8` (the only caller) | `ramp_event_entry`: the preset picker, then the stock consumer |
| 0x61ae | call to the LED effect dispatcher `0x920c` (the only caller) | `ramp_led_entry`: button light and LEDs while a ramp or the picker owns them, else the stock effects |
| 0xb066 | stock send of the `0xAA` dab-counter reply (notify `0xe734`) | `ramp_announce_entry`: sends it unchanged, then announces the patch (`0xBC`) |

The code goes at flash 0x14000 and runs at 0x14028, with 12 KB to grow into, and the output image
ends at 0x17000 (94,212 bytes with the trailer) -- well
inside the 124 KB the stock OTA accepts (`main()` calls `bls_ota_set_fwSize_and_fwBootAddr(124,
0x20000)`).

The ramp store has two sectors, 0x70000 and 0x71000 (two copies, so a power cut during a save
never loses it), outside both OTA banks. Stock erases the bank it
isn't running from at every boot, so a store inside the image would be lost after an OTA install
(the image then runs from bank 1). Every stock erase site is traced in [`device.h`](device.h):
nothing stock touches 0x61000-0x73fff.

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
whether it was already awake. A hold that ends a multi-press
gesture (two presses + hold, four presses + hold for dim mode, seven presses + hold) stays the
stock gesture. Inside it:

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
+10 s commands always reach the stock code. The events it takes reach the stock handler only as a
plain press, which still counts as activity for the auto-off timer, so the stock gestures are
unchanged outside it: a click cycles the temperature preset, a triple
click cycles the LED preset, four clicks show the battery. In the idle state a stock hold does
nothing (it only stops a running session), so the picker's hold doesn't shadow a stock gesture.

Like stock, the picker ignores button events during the power-on transition (struct `+7` set and
`+9` == 1, which the stock consumer also checks). With the LEDs off it doesn't open at all: it
would be an invisible mode taking clicks for up to 30 s. Turn the LEDs on (triple click) to use it.

**Quick heat.** Two presses + a long hold from sleep starts the stock quick heat (UI state 7, a
session at a forced 80 °C / 176 °F that runs until a press stops it). It never arms a ramp, even from a sentinel slot.

**The button light.** During a ramp it shows the same temperature colour as the LEDs (blue at
the coolest stage through to gold at the hottest); in the picker, the preset's colour, or red when the
system is off. It's an RGB LED on PB5/PB6/PB7 driven by a software-PWM timer interrupt (`0x49c`)
from three duty values, 0-100 (`0x845602` red, `0x8455fc` green, `0x8455fe` blue). The stock LED
effect dispatcher (`0x920c`, one caller at `0x61ae`, every other 10 ms main-loop pass: 50 Hz) normally sets both
lights; the patch wraps that call. While a ramp or the picker owns the lights, it fills both, sets
the LED power bit the dispatcher sets after every push (the pin at `0x84317c`, chosen at boot) and
pushes the ring (`0x90bc`) itself, from that one place, instead of running the stock effects.
Otherwise the dispatcher runs unchanged, which also restores the stock colours. A stock cue or
warning always plays out first: the dispatcher's animation state at `0x84562c` (+1 level, +5
blinks, +6 fade-in, +7 fade-out) must be idle at level 100, and its effect (+0) must not be 9, the
warning flash, before the patch draws -- so the three blinks at every session start play, warnings
show, and the level is never frozen mid-blink. The lights stay stock's while the OTA flag the
dispatcher checks (`0x843184`) is set. Both lights follow the LED setting: with LEDs off, the stock
effects keep both.

## Device-specific behaviour

- The stock session clock only runs once the target is reached, so each hold is time **at**
  temperature.
- Scale: +0x4 == 1 is Celsius, the opposite polarity to the Carta 2. Measured temperature is +0x28
  (°F) and +0x2a (°C).
- Dab counters are at 0x8432ec, and saving is armed by setting +31 = 200 and +32 = 250. The end cue
  is at 0x84324c.
- Concentrate ceiling: 600 °F, the official app's limit for this device.

Every address in [`device.h`](device.h) is listed with the stock code that proves what it means.

## Reverting, and OTA compatibility

The Aeris, like the Sport and the Carta 2, updates over two banks (`main()` sets them with
`bls_ota_set_fwSize_and_fwBootAddr(124, 0x20000)`: images at 0x0 and 0x20000). Traced on the Sport's
copy of the same SDK code:

- **An OTA writes the bank the device isn't running from**, never the running one: each 16-byte
  block goes to the other bank's base plus its offset (the app's packets carry only a block index;
  the device adds the bank). The image's boot flag (the "K" of "KNLT" at byte 8) is held at 0xFF
  while it's written.
- **Only a complete, verified transfer switches.** At the end the new bank's flag is written and
  the old one's cleared, then the device reboots into the new image. A transfer that fails partway
  never gets a valid flag, so the device keeps booting the image it was running.
- **The bank left behind is wiped at the next boot** (the SDK's own clear, and the app's wipe loops
  0x6004 / 0x637a / 0x692e), which is why the ramp store lives outside both banks (0x70000 /
  0x71000). Stock never touches those sectors, so after a revert they simply sit unused.

Flashing the original stock file back, over OTA or SWire, is a full, working revert: it becomes the
running image, and the patched one is wiped from the other bank at the next boot.
