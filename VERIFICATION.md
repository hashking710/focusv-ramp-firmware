# Verification ledger

Nothing here ships until every row is **traced** or **hardware**-confirmed. Status:

- **traced** -- read from the stock code that consumes it, in this build, with the address given
- **carried** -- traced in an earlier pass, not re-traced since; re-trace before shipping
- **open** -- not confirmed; needs more tracing or a device
- **hardware** -- can only be confirmed on a device

Builds: Carta 2 PROD-111224, Aeris PROD-111224, Carta Sport PROD-030426.

## All devices

| Item | Carta 2 | Aeris | Sport |
| --- | --- | --- | --- |
| Patch sites hold the expected stock instruction; hooked functions have exactly the expected callers | traced (`tools/build.py`, every build) | traced | traced |
| Every call into stock lands 0x28 past a real function entry, bit 0 set | traced (`build.py`) | traced | traced |
| Store writes stay inside one 256-byte flash page (Carta 2 0x964 and Aeris 0xa5c program without splitting at page boundaries; Sport 0xc0e8 splits) | traced: the store starts on its sector and is 66 bytes -- keep it under 256 | traced | traced |
| Image within the stock OTA size limit (`bls_ota_set_fwSize_and_fwBootAddr` in `main()`) | traced: 248 KB, bank 0x40000; image 0x33004 | traced: 124 KB, bank 0x20000; image 0x17004 | traced: 124 KB, bank 0x20000; image 0x1b004 |
| Stock never erases / writes the patch's code and store sectors | traced: OTA bank 0x40000+, settings 0x8e000, 0x80000, screensaver slots 0x8f000-0xc8000 (pointer bounds at 0x129a0), pairing | traced: settings 0x40000-0x43fff, other-bank erase loops, pairing | traced: settings 0x40000-0x43fff, pairing 0x74000+, other bank |
| Ramp RAM (0x848000) clear of .data / .bss / stack | traced: .bss ends 0x845829, stack 0x850000; stock only compares 0x848000 / 0x84c000 (retention size) and fills 0x84c000-0x84ffe0 | traced: .bss ends 0x845ac1, stack 0x850000 | traced: .bss ends 0x845054, stack 0x850000 |
| Flash read / write / erase primitives and their arguments | traced: read 0x9c0 (cmd 0x03), erase 0x924 (0x06 + 0x20), write 0x19a78 verifies into a 64-byte stack buffer -> store written in pieces of at most 64 (DEV_FLASH_WRITE_MAX); host test enforces it | traced: read 0xab8 (cmd 0x03), erase 0xa1c (0x06 + 0x20), write 0xa5c (0x02, no stack buffer) | traced: 0xc0cc (cmd 0x03), 0xc0e8 (page-split), 0xc178 (sector erase 0x20) |
| Orchestrator: target reload while not reached, mode / rank / table offsets, measured F | traced (0xaf2c) | traced (0x8154) | traced (0x7c00) |
| Session timer: countdown, "reached" gate, completion counters, save arm, end cue | traced (0xb5b8): the clock waits for reached -- corrected (DEV_CLOCK_WAITS_FOR_REACHED) | traced (0x8980) | traced (0x8448, 0x8494) |
| Session hard cap longer than the longest ramp (~35 min) | traced (0xb8b8, 40 Hz): 270000 ticks ~ 112 min; also stops if still cold after 7.5 s or never reached in 65 s (0x84531d, kept set across ramp stages) | traced: 270000 passes ~ 90 min (0x8538) | traced: ~6 h (0x7fe0) |
| Marker hook (0xCC handler) register safety | traced: r0-r2 saved, r2 read after the chain (0x11db2); markers acted on on screens 1 (via 0x124cc), 4, 16 | traced: r4 callee-saved, ip written before read (0xb1f8) | traced: ip written before read (0xad60) |
| App markers post A5 / AF / 66 as events | traced: 16 / 17 / 18 (0x125f2, 0x125e6, 0x12726) | traced: 18 / 19 / 20 (0xb934, 0xbe3a, 0xbe2c) | traced: 18 / 19 / 20 (0xb76e, 0xb786, 0xb77a) |
| Announce wrapper on the 0xAA reply; notify returns 0 when queued | traced (0x11562, 0x15a34) | traced (0xb066, 0xe734) | traced (0xa9ea, 0xec3c) |
| Quick heat never arms a ramp | traced: screen 15 forces 85 C / 184 F (0xaf2c) and zeroes the countdown (0xb5b8); guard added; no writer of 15 found in this build | traced (0x81aa -> 0x85ac, 80 C) | traced (0x7c00, 90 C) |
| A stage that never reaches temperature can't stall the ramp | host test (clock waits for reached) | host test (clock waits for reached) | host test |

## Aeris and Sport: buttons and lights

| Item | Status |
| --- | --- |
| Button scanner: 16 press, 15 hold (200 scans), 7-11 clicks, 12 / 13 / 14 multi-press holds, clicks counter | traced (Sport 0x8868, clicks +0x20; Aeris 0x8c60, clicks +0x1d) |
| Stock hold (15) from idle does nothing; picker entry only from a single press, LEDs on | traced (Sport 0x47be, Aeris 0x50f4) |
| Power off (11: five clicks, the app's command, the sleep request) and app events always reach stock | traced (Sport 0xb300 / 0x4934, Aeris 0xb71a / 0xb9bc / 0x53b8) |
| Taken events reach the consumer as 16 (ignored in UI 1), so the auto-off timer resets | traced (Sport prelude +0x42, Aeris +0x3f / +0x40) |
| Hold from standby (UI 8): the press wakes, then the hold opens the picker | open: stock does nothing on that hold; decide whether that's acceptable |
| Ring and button share PWM0 / DMA7 (Sport): pushed button -> rail -> ring, from the dispatcher site | traced (0x8efc, 0x8cf8, 0x8ff8 tail) |
| Animation gate (blinks +5, fades +6 / +7, level +1, warning effect 9) | traced (Sport 0x8ff8, 0x9976; Aeris 0x920c, 0x9b62); nothing else writes those fields |
| Aeris LED rail pin (0x84317c) and OTA flag (0x843184) | traced (0x920c tail) |
| Colours, brightness, flicker, the session-start blinks then the ramp | hardware |
| Hold length in seconds (scanner runs every 100th timer interrupt) | hardware |

## Carta 2: buttons and screen

| Item | Status |
| --- | --- |
| Screen table at 0x1a398 (pointer 0x1a3c0 is a runtime address) | traced: entries 4 and 14 both 0x5740, which tests for 4 and 14 |
| Event codes: 1 / 2 short, 3 / 4 held, 7-11 clicks, 13 long hold, 14 any press | traced (decoder 0xbe00-0xc180) |
| Device lock is +0x82 (app "Device Locked", 0x99 byte 16 bit 0x10) | traced (0x99 builder 0x1127e, 0x121d0, base 0x84309c + 0xe) |
| Idle live view: every gesture has a stock meaning -> picker disabled | traced for + / -, double / triple / four / five clicks, long hold; single click wakes the screensaver (seen on the device) |
| Single click during a ramp passed to stock untouched (stop on the heating screen) | traced (screen 1 handler 0x5708 -> 0x97f0); hardware for the screensaver case |
| Screensaver | traced: screen 16 (0xb8b8 sets it, slot 0x8432fc 3 / 4 -> image 0xac000 / 0x8f000), only with no session (idle counter cleared while a session runs); a press (14) wakes it (0x5660 -> 0x5b20 -> 0x5a9e: screen 1, live view); locked, the decoder swallows the click (0xc058 / 0xc146) |
| + / - during a ramp step stages (stock: temperature editor, screen 6, 0x610a); locked they pass to stock, which ignores them | traced; no screensaver during a session |
| Display hooks skip only draw functions | traced: ce70 returns the target it drew -- used by 0xe9a8 (view 4, also the stock temperature editor), so the hook now returns it on both paths; cf58 / db40 / d048 / e42c / e300 / e2b4 / dcac draw only (d048 sets a layout x used by e300, also hidden); d3c0 also clamps the display temperature (+0x0e / +0x10), which 0x9a18 keeps moving, so the app's live temperature (0x99 bytes 6-7) stays live |
| Ramp screen drawing (visual) | host render test only; hardware |
| Events during a ramp | traced: only 7 / 17 (stop), 11 (power off), 14, 18-21 pass; 3 / 4 / 6 / 8 / 9 / 10 / 12 / 13 / 15 / 16 are taken -- each would move the screen off 1 (where alone reached is set, 0xb21c) or the active slot (13: next preset rank, 0x61f6 / 0x6236) |
| Events taken mid-ramp skip the consumer's prelude | traced: it only reloads the delayed log / save timers (0x8430a8 +0x57 -> log record 0x63fc, +0x58 -> save flag 0x64da) while they're already running, and resets the idle counter (no screensaver during a session); skipping it lets a pending save fire a little sooner |
