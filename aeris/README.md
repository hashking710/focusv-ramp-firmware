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

The code goes at flash 0x14000 and runs at 0x14028, with 8 KB to grow into. The waypoint store has
its own sector at 0x16000, and the output image ends with it, at 0x17000 (94,212 bytes with the
trailer) -- well
inside the 124 KB the stock OTA accepts (`main()` calls `bls_ota_set_fwSize_and_fwBootAddr(124,
0x20000)`). Stock erases only its settings (0x40000-0x43fff), the SDK's pairing area and the
other OTA bank, so nothing stock touches 0x14000-0x16fff while it runs from bank 0.

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
effect dispatcher (`0x920c`, one caller at `0x61ae`, every main-loop tick) normally sets both
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

Traced directly -- the BLE attribute table (by the OTA characteristic's own UUID), its write
handler, and cross-checked against `terpline-web`'s own OTA client (`lib/protocol/ota.ts`), the
real consumer of this wire format:

- **The write base is flash address 0, confirmed, not inferred.** The app's `BEGIN`/block/`FINISH`
  commands carry no address or bank field at all -- just a sequential block index starting at 0,
  covering the file from its own byte 0 (header included). The firmware writes block `i` to
  `i * 16 + base`; for the file's first bytes to land where the header actually lives, `base` must
  be 0. There is no separate staging bank: an OTA update overwrites flash in place, sequentially,
  from the start.
- **The stock OTA write path is completely separate from this patch** regardless: it lives at
  0x116cc-0x1354c -- the attribute table, the write handler, its CRC check -- entirely below this
  patch's own code at 0x14000+, with the stock image's real end (0x13b2c) in between. This patch
  touches none of it.
- **A real update of the stock file never even reaches this patch's flash region.** Writing
  sequentially from 0, an 80,684-byte stock image stops at 0x13b2c -- short of 0x14000. This
  patch's own code and waypoint store are left untouched, not overwritten.
- **Reverting is confirmed by the two patch sites, not by this patch's own flash region.** The
  patch changes behaviour only through its 2 call-site swaps (0x6464, 0xb490), both well before
  0x14000 in write order. Any OTA update of a real stock image writes over them within the first
  quarter of the transfer, restoring stock bytes there long before the transfer finishes -- whether
  it completes or not. Once that's true, nothing on the device calls into this patch's code again.
- **The transfer protects itself against a lost connection.** Writing block 0 deliberately corrupts
  its own copy of the "KNLT" magic byte (forces it to 0xFF); only `FINISH` restores it. An update
  that fails partway leaves an image with an invalid header -- recognized as such on the next boot,
  not run as if it were a complete one. This is a stock safety mechanism, unrelated to this patch
  and unaffected by it.

Flashing the original stock file back, over OTA or any other method, is a full, working revert.
