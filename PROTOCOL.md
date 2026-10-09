# Ramp firmware BLE protocol

Everything an app needs to drive the patched firmware: detect it, switch it
between ramp mode and stock mode, upload ramps, set the offset and start a ramp.
Terpline is one implementation (`terpline-web/lib/protocol/packets.ts`); nothing
here is specific to it. The stock protocol itself (status packets, settings,
OTA) is documented in [focusv-ble-research](https://github.com/hashking710/focusv-ble-research).

All three devices (Carta 2, Aeris, Carta Sport) use the same packets.

## Connection

| | UUID |
| --- | --- |
| Service | `1011123e-8535-b5a0-7140-a304d2495cb7` |
| Notify (read) characteristic | `1011123e-8535-b5a0-7140-a304d2495cb8` |
| Write characteristic | `1011123e-8535-b5a0-7140-a304d2495cb9` |

## Detecting the patch: the announcement

After the stock `0xAA` dab-counter reply (part of the burst the device sends
when it gets the stock date/time sync, opcode `0xDD`), a patched device also
notifies:

```
[BC, 0C, 'T', 'R', 'M', 'P', protocol, device, flags, preset, offset, BC]
```

| Byte | Meaning |
| --- | --- |
| 6 `protocol` | 3 for this version. Protocol 2 had no bit 2; protocol 1 had no stock mode, and byte 8 was 0 / 1 (ramps enabled). |
| 7 `device` | 1 Carta 2, 2 Aeris, 3 Carta Sport |
| 8 `flags` | bit 0: ramps enabled (the on-device on / off switch). bit 1: stock mode. bit 2: a ramp is running. |
| 9 `preset` | the selected built-in preset, 0-5 |
| 10 `offset` | the setup offset, signed °F (-10 to +15) |

Stock firmware never sends `0xBC`. No announcement after a sync means the
device isn't patched (or runs a build from before the announcement existed).

The device also announces right after a mode switch, and whenever a ramp starts
or ends. Use bit 2 to follow a ramp, not the `0x99` status: its session byte
(byte 5) is the countdown's low byte, which reads 0 at every multiple of 256
seconds of a ramp.

## Commands: the stock set-temperature packet

Every command is the stock 16-byte `0xCC` packet, which the stock handler parses
before the patch sees it:

| Byte | Meaning |
| --- | --- |
| 0 | `0xCC` |
| 1 | `0x10` |
| 2-3 | flower temperature, big-endian, in the unit of byte 6 |
| 4-5 | concentrate temperature, big-endian |
| 6 | unit: `0x11` °F, `0x22` °C |
| 7-8 | flower hold, seconds, big-endian |
| 9-10 | concentrate hold, seconds, big-endian |
| 11 | flower preset rank (0 = custom, 1-5) |
| 12 | concentrate preset rank |
| 13 | marker (below) |
| 14 | argument (stock never reads this byte) |
| 15 | `0xCC` |

**Stock writes bytes 2-12 into the device on every one of these packets.** It
sets both custom (rank 0) presets and both active ranks. So unless a command
is meant to change them, **echo the device's current values**. Take them from
its `0x99` status notification:

| `0x99` status byte | Value |
| --- | --- |
| 4 | flower rank << 4, concentrate rank |
| 8-9 / 10-11 | custom flower / concentrate temperature, big-endian, in the device's unit (byte 3: `0x11` °F, `0x22` °C) |
| 14 / 15 | custom flower / concentrate hold, seconds (one byte each; the official app's limit is 240) |

The device acts on a `0xCC` packet only while it's awake: on the Carta 2 only on
its live, lock or screensaver screens, and on the Aeris and Sport only when on or
in standby. Otherwise it drops the packet silently. Wake it first with any stock
write that changes nothing: Terpline echoes the current settings (opcode `0x11`)
and waits 800 ms. Leave about 400 ms between commands that write flash.

| Marker (byte 13) | Byte 14 | Effect | Works in stock mode |
| --- | --- | --- | --- |
| `0xBD` | `0x53` ('S') | switch to **stock mode** | yes |
| `0xBD` | `0x52` ('R') | switch to **ramp mode** | yes |
| `0xA5` (stock start) | `0x52` ('R') | start the session as a ramp (below) | no: a plain stock start |
| `0xB1`-`0xB5` | 0 | save flower stage 1-5 from the custom flower values | no |
| `0xB6`-`0xBA` | 0 | save concentrate stage 1-5 from the custom concentrate values | no |
| `0xBB` | offset, signed °F | set the setup offset for the built-in presets | no |
| `0xBE` | preset number | choose the built-in preset a concentrate session runs with no stages saved: 0-5 on the Carta 2, 0-3 on the Aeris and Sport (Flavor first, Rosin / solventless, Balanced, Sauce / badder, Long session, Clouds); out of range is ignored | no |

The patch ignores every marker while a ramp is running, and every marker except
`0xBD` while ramps are switched off on the device. After `0xBB`, `0xBD` and
`0xBE` the device announces itself with the new values.

### Stock mode

Stock mode turns the patch into a pass-through. Nothing arms a ramp, not even a
preset slot set to the 150 °F trigger. Every button event and every LED update
goes to the stock code untouched, and the Carta 2's screen is stock. All
markers except `0xBD` are ignored. The device keeps announcing itself, so an app
can find it and switch it back. The mode is kept in flash and survives
power-off; saved ramps, the preset choice and the offset are kept too. The
switch is refused while a ramp runs. To remove the patch entirely, reflash the
original firmware.

The devices can also switch with no app. The Carta 2 at power-on: holding −
while pressing the power button five times starts it in ramp mode, holding +
starts it in stock mode. The Aeris and Sport, on and idle: five presses with the
fifth held switch between the two. The choice is kept either way, and the
announcement reports it like any other switch.

### Starting a ramp

Send the stock start with byte 14 = `0x52` and **every value echoed**, including
both ranks. The device starts the preset that's already active; the patch runs
the mode's saved stages (in concentrate with none saved, the selected built-in
preset) and puts that preset back exactly afterwards. With nothing to run, the
session is stopped. The request is valid for 3 seconds. The device announces
the ramp starting and ending (flags bit 2).

Stop a ramp the stock way, marker `0xAF` -- **with every value echoed**, like
any other `0xCC` packet: stock writes the presets and ranks before it reads the
marker, so zeros there would set the custom preset to 0 and select it. Or press
the device's button.

During a ramp, any `0xCC` packet that changes either rank (or the device's
atomizer changing) hands the session back to stock: the ramp ends, its preset
is put back, and the session carries on as a plain session of the newly active
preset, at that preset's temperature and for its hold. Echo the ranks to avoid
it.

### Uploading a ramp

For stage *n* of a mode, send the stage's temperature (in the unit of byte 6)
and hold as that mode's custom values, with marker `0xB0 + n` (flower) or
`0xB5 + n` (concentrate). The other mode's values and both ranks are echoed.
Stage 1 clears stages 2-5, so upload 1..n in order. Afterwards, write the
custom preset back to what it was with marker `0x00` (stock ignores it, the
patch too). Limits: flower 275-500 °F, concentrate 365 °F to the device's
ceiling, holds 1-300 s; a ramp with any stage outside them never runs.
