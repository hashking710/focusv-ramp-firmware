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
| 0x50d2 | LED-preset click counter's increment+store | `ramp_click_entry`: 4 clicks toggles the ramp system (see above) |
| 0x645c | call to button-event consumer `0x4ee8` (the only caller) | `ramp_event_entry`: the preset picker, then the stock consumer |
| 0xb066 | stock send of the `0xAA` dab-counter reply (notify `0xe734`) | `ramp_announce_entry`: sends it unchanged, then announces the patch (`0xBC`) |

The code goes at flash 0x14000 and runs at 0x14028. The waypoint store has its own sector at
0x15000. The output image ends at 0x20000.

## Preset picker

A hold from the idle screen (the state that shows the temperature) opens the picker. Single clicks step
through the first four built-in presets (Flavor first, Rosin, Balanced, Sauce; Balanced is the default),
and the LEDs show the selection: one lit LED per preset, up to the chosen one. A hold leaves the picker
and keeps the choice in flash. The picker is consumed before the stock handler sees its events, so the
stock clicks and holds are unchanged outside it.

A hold from idle, with no picker open, would otherwise increment the shared gesture counter at struct
`+0x60` (the counter the click gestures use), so the hook takes it. The picker only opens with no session
running and with the ramp system on.

## Button light

The control button has its own light. During a ramp it shows the same temperature colour as the LEDs
(blue at the coolest stage, through violet, magenta and orange, to gold at the hottest). While the
preset picker is open it shows the selected preset's colour, the same blue as the LED selection. The
light follows the LED setting: with LEDs off it shows nothing and the stock colour is left alone.

The patch writes the light's three colour bytes each tick, after the stock tick has run, and the
stock button routine drives the light from them. The stock colour is saved the first time the patch
takes the light and put back when the ramp or picker stops using it. This relies on the stock button
routine running on each tick, which has been traced in the code but not confirmed on hardware.

## On/off switch: four clicks

Click the button four times in a row (the same gesture that otherwise cycles through the
device's 5 LED presets, 1 at a time) to toggle the whole ramp system on or off, directly on the
device, no app needed:

- **Off**: no new ramp can arm, and no new waypoint can be saved -- a stage-save packet is
  dropped with no flash write at all, the same as on stock firmware. A ramp already running
  finishes or stops normally; it isn't interrupted.
- **On**: back to normal.

It's stored as one more byte in the same flash sector as the waypoints, so it survives a power
cycle. A device that's never had this toggled reads as **on** -- today's behaviour, unchanged.

Landing on the 4th click is repurposed, not just read: the click counter resets to 0 right then
(matching "0 = LEDs off"), so LED preset 4 itself is never actually selected -- whatever it was
factory-set to never lights up. Clicks 1-3 are completely untouched; the normal preset cycling
through 1-5 still works exactly as it does on stock firmware for those.

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
