# focusv-ramp-firmware

![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)
![Status: untested on hardware](https://img.shields.io/badge/status-untested%20on%20hardware-orange.svg)
![Not affiliated with Focus V](https://img.shields.io/badge/affiliation-independent%2C%20unofficial-lightgrey.svg)

> [!WARNING]
> **Not yet tested on real hardware.** Every patch passes the build's full verification and an
> independent disassembler cross-check, but that proves the bytes are right, not how a device
> behaves. Flash only a device you can recover over SWire (see focusv-ble-research's
> [hardware guide](https://github.com/hashking710/focusv-ble-research/blob/master/docs/hardware-setup.md)),
> and keep your original firmware file. Bench-test reports are the most useful contribution right now.

Independent, hobbyist firmware patches that add an autonomous temperature ramp to Focus V's
Carta 2, Aeris and Carta Sport. You build a ramp in [Terpline](https://terpline.app) and upload
it once, then start it like any session. The device runs every stage on its own, with no phone
connected.

This is a companion to **[focusv-ble-research](https://github.com/hashking710/focusv-ble-research)**,
the reverse-engineering project behind every address used here. Not affiliated with, endorsed
by, or sponsored by Focus V.

## What it does

- **Ramps from Terpline or the device.** Terpline uploads up to 5 stages per mode (flower /
  concentrate), each a temperature plus a hold time, stored in their own flash sector. The device
  also ships six built-in concentrate ramps (the same profiles as Terpline's presets).
- **Choosing a built-in ramp on the device.** A hold from the idle screen opens the picker, and
  the choice is kept across power cycles. Carta 2: hold − and the + / − buttons step through the
  six presets, a click leaves the picker. Aeris and Sport: single clicks step through the first
  four presets, and the LEDs show which one is selected; a hold leaves the picker. Outside the
  picker, the stock buttons behave exactly as before.
- **Starting a ramp** is an ordinary session started at a sentinel temperature (150 °F). If
  usable stages are saved for the attached atomizer's mode, the device runs them. Otherwise, in
  concentrate mode, it runs the selected built-in preset, and in flower mode it's a normal stock
  session at that temperature. A setup offset (−10 to +15 °F, set from the app) shifts the
  built-in presets; it defaults to zero. It's sent as marker 0xBB with the offset in packet byte 14,
  which the stock packet handler never reads.
- **Stages reuse the stock heater.** Each stage changes the active preset's temperature exactly
  the way the stock firmware changes temperature mid-session. The stock heat-up, ready cue, PID
  control and safety limits run every stage unchanged; the patch never drives the heater itself.
- **One press stops it**, at any stage: the main button on the Carta 2, the single button on the
  Aeris and Sport. Both use the stock stop path. On the Carta 2, **+ / −** while heating jump to
  the next or previous stage.
- **The dab counter counts real dabs.** A ramp counts once, when it has reached stage 2 and has run
  for 20 seconds at temperature. Time at temperature is the stock "reached" flag, so on the Carta 2
  heat-up doesn't count. It uses the same counters, and the same save, as a finished stock session,
  so the official app reports it accurately. A ramp stopped before that point isn't counted, just as
  a stopped stock session isn't. A single-stage ramp is counted by the stock firmware when it finishes.
- **Progress display.** On the Carta 2, the top of the screen shows battery % and time left on
  the left, and the stage target over the live temperature on the right; a chart of the whole ramp
  sits below. On the Aeris (4 LEDs) and Sport (5 LEDs), the number of lit LEDs shows progress and
  their colour shows temperature, within the user's own LED setting.

## Safeguards

- **Exact build only.** `apply_patch.py` patches only the exact firmware build each patch was
  verified against: fingerprint and length, with deliberately no override. It also checks every
  patch site's original bytes, and the SHA-256 of the code blob the patch table points into. If
  any check fails, it writes nothing.
- **Waypoints are validated before arming.** They're read through the stock flash routine into
  RAM when a ramp arms. Every stage must fall within the official app's limits (flower
  275–500 °F, concentrate 365 °F up to the device's ceiling, at most 300 s per stage), with its °F
  and °C values agreeing. Anything else means no ramp, just a stock session. A running ramp works
  from its RAM copy, so a new upload can't change it mid-session.
- **Ramp state is checked.** It's checked every tick, and a preset slot is only ever restored from
  state the patch provably wrote. That covers a reboot mid-ramp, or uninitialised RAM that happens
  to look valid.
- **Nothing temporary is saved to flash.** The patch never changes a preset's hold time. The stock
  save that persists the dab counter is triggered only after the preset slot has its own values
  back, so a ramp's temporary stage temperature can never be written to flash.
- **Stale stages are cleared.** Uploading stage 1 clears that mode's stages 2–5, so a shorter ramp
  can never inherit stages from an older, longer one.

## Building it

**This repo publishes no compiled code, and no Focus V firmware.** You supply your own stock file,
downloaded the same way the official app gets it, and build the patch locally:

```sh
python3 tools/build.py carta2 --firmware your-carta2-PROD-111224.bin
python3 tools/build.py aeris  --firmware your-aeris-PROD-111224.bin
python3 tools/build.py sport  --firmware your-sport-PROD-030426.bin
```

This needs Docker and a Linux TC32 toolchain (`--toolchain DIR` or `$TC32_TOOLCHAIN`). The build
compiles with `-Wall -Werror` and links the code at its real runtime address. It writes the
device's patch table into `apply_patch.py`, then checks everything below; any single failure
stops it:

- no undefined symbols, and the code ends before the waypoint sector
- every call into stock code lands on an **odd** address exactly **0x28 past a real function
  entry** in your file
- every patch site decodes to exactly the expected stock instruction, and every caller of each
  hooked function is found, so none can be missed
- each replacement `tjl` lands exactly on the intended new function
- end to end, `apply_patch.py` run on your file:
  - every site holds its replacement, and zero other stock bytes change
  - the blob is placed exactly at its address, with 0xFF filling the gap before it
  - the waypoint sector ships erased, and no flash sector past the image is touched
  - the header length and Telink CRC32 are correct
- the finished image is re-disassembled with the real `tc32-elf-objdump`, and every site must
  decode to a call to its function: an independent cross-check of the build's own decoder
- `apply_patch.py` **refuses**, writing nothing, when given: an altered patch site, a one-bit
  different build, a truncated file, or a mismatched code blob

Then `python3 <device>/apply_patch.py --input your-file.bin --output patched.bin`.

What none of this proves is behaviour on a real device: timing, the screen, the LEDs, and the
stock code paths the patch relies on but that were read rather than run. Bench testing on
hardware is the remaining step.

## Layout

```text
common/ramp.h        design notes, state, store format, safeguard limits
common/ramp_core.c   ramp sequencer: arming, stages, countdown, dab counting
common/ramp_store.c  waypoint store (0xB1-0xB5 flower, 0xB6-0xBA concentrate)
common/ramp_presets.c built-in concentrate ramps and the setup offset
common/ramp_picker.c preset picker state machine, shared by every device
aeris/ramp_event.c, sport/ramp_event.c   button-event hook: the picker, then stock
<device>/device.h    that device's confirmed addresses, each with the stock code that proves it
carta2/ramp_display.c, carta2/ramp_input.c   Carta 2 screen and buttons
aeris/ramp_led.c, sport/ramp_led.c           LED progress
<device>/ramp_marker_entry.s                 the upload-marker hook
<device>/apply_patch.py                      the patcher (patch table written by tools/build.py)
tools/build.py                               build + verification
```

| Device | Firmware build | Patch sites | Code / waypoint sector |
| --- | --- | --- | --- |
| [`carta2/`](carta2/) | PROD-111224 | 32 | 0x30000 / 0x32000 |
| [`aeris/`](aeris/) | PROD-111224 | 4 | 0x14000 / 0x15000 |
| [`sport/`](sport/) | PROD-030426 | 4 | 0x18000 / 0x19000 |

## History

Earlier versions published here had two separate bugs, each of which would have crashed the
device on its first tick after boot:

1. **Even call targets.** Calls into stock code compile to `tjex`, which treats bit 0 of the
   target like Arm's `bx` does, so the target must be odd. The patches passed even addresses.
2. **The 0x28 image offset.** Code runs at its disassembly address + 0x28: the 40-byte firmware
   header sits at flash 0 and the body follows it. The patches called and linked everything 0x28
   bytes too low.

The same audits also found:

- Carta 2: a flash-write address left over from an older build.
- Carta 2: the flower/concentrate flag the patch read was actually the °F/°C setting.
- Stage targets were overwritten by stock code before the heater ever used them.
- Marker hook: it clobbered a register the stock code still needed.
- Aeris: the patch force-enabled LEDs the user had switched off.
- Carta Sport: the waypoint store sat on top of the patch's own code.
- Carta Sport: Celsius was read as Fahrenheit.

All of these are fixed, and `tools/build.py` now checks for each class of mistake on every build.
No one is known to have flashed any earlier version.

## Reverting

Keep your original firmware file. Flashing it back (see focusv-ble-research's
[hardware guide](https://github.com/hashking710/focusv-ble-research/blob/master/docs/hardware-setup.md)
or [`ota-flash.html`](https://github.com/hashking710/focusv-ble-research/blob/master/tools/ota-flash.html))
is a full revert. See [`LEGAL.md`](LEGAL.md) for why this repo ships only source and patch bytes.

## Contributing

The most useful contribution now is careful bench testing on hardware that you're prepared to
recover over SWire, plus a report of what you saw. If you change anything the device executes,
it has to pass `tools/build.py` unchanged. See [`CONTRIBUTING.md`](CONTRIBUTING.md).
