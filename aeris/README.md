← [Back to main README](../README.md)

# Aeris ramp patch

> [!CAUTION]
> **Do not flash this patch.** It has not run on real hardware. Earlier published versions had two
> crash-on-boot bugs; see the [main README](../README.md). They're fixed in this source, but only
> a bench test can confirm it works.

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

**Not verifiable from the firmware file:** that nothing else on the device uses flash
0x14000–0x15fff, which lies past the end of the stock image. Only a hardware test confirms it.
