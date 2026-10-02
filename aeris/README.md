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

## Patch sites (2, written and checked by `tools/build.py`)

| Site | Stock | Replaced with |
| --- | --- | --- |
| 0x6464 | call to orchestrator `0x8154` (the only caller) | `ramp_trampoline`: stock tick, the ramp, then the LEDs |
| 0xb490 | marker-byte load before the A5/AF/66 chain | `ramp_marker_entry`: waypoint upload markers |

The code goes at flash 0x14000 and runs at 0x14028. The waypoint store has its own sector at
0x15000. The output image ends at 0x20000.

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

Traced directly (the BLE attribute table, by the OTA characteristic's own UUID, down to the
write handler and the flash call it makes):

- **The stock OTA write path is completely separate from this patch.** It lives at
  0x116cc-0x1354c -- the GATT attribute table, the write handler, and its CRC check -- entirely
  below this patch's own code at 0x14000+, with the stock image's real end (0x13b2c) in between.
  This patch touches none of it: not the attribute table, not the write handler, not the two
  stock functions (0x8154, the marker chain) it hooks have anything to do with OTA. An update
  should work exactly as it does on stock firmware.
- **The write handler confirms the mechanism**: each 16-byte block from the app is written with
  the same verified-write primitive this patch itself uses (`0xa5c`, `DEV_FLASH_WRITE`), at
  `block_sequence * 16 + base`. That `base` is a per-session value set by the OTA START command,
  not a fixed address visible in the disassembly -- so while nothing in the code ties it to
  0x14000-0x1fff, this file stops short of a confirmed address for it.
- **Reverting is a property of the patch sites, not of this patch's own flash region.** The patch
  changes behaviour only through its 2 call-site swaps (0x6464, 0xb490). Any OTA update that
  writes a real stock image necessarily overwrites that low flash range -- it's where the firmware
  itself begins -- restoring those two sites to their original bytes. Once that's true, nothing on
  the device calls into this patch's code again, regardless of what happens to the leftover bytes
  at 0x14000+. Flashing the original stock file back, by any method, is a full, working revert.

**Not independently verified:** whether an OTA update's `base` ever lands inside 0x14000-0x1fff,
which would overwrite this patch's own code and waypoint store mid-update (harmless -- see above
-- but not confirmed either way), and whether anything else on the device uses that flash range at
all. Both are open questions in focusv-ble-research (issues #3, #4); only a hardware test, or
finishing that trace, settles them.
