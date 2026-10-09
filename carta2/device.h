/* device.h -- Carta 2 ("Quantum"), PROD-111224. Each value confirmed from the
 * stock code that consumes it:
 *   FUN_0000af2c  orchestrator: session gate +0x1, reload gate +0x2, mode +0x9,
 *                 target pair +0x20 (C) / +0x22 (F) filled from the active slot
 *   FUN_0000a7f4  PID step: +0x7 != 0 -> (+0x22 - +0x1c) else (+0x20 - +0x1e)
 *   FUN_00011cc4  0xCC handler: writes +0x7 from the packet scale byte (1 = F)
 *                 (traced, C branch 0x11cd8-0x11d96): big-endian, never clamped;
 *                 C -> +0x36/+0x4e, F = floor(9c/5)+32 -> +0x2a/+0x42, holds ->
 *                 +0x5a/+0x66 (the custom, rank 0 slot); the marker (byte 13, read
 *                 at 0x11d96 = the hook) is only acted on on screens 1 (via
 *                 0x124cc, which redraws view 15 first), 4 and 16 (gate
 *                 0x11d7a, screen = 0x84309c+5); on any other screen the
 *                 packet is dropped without a reply (the app wakes it first)
 *   0xb5b8 session timer (40 Hz, 1 s every 40 calls): returns at once unless
 *                 +0x2 (reached) is set -- the clock waits for temperature,
 *                 like the Aeris and Sport; +0x1a countdown; at zero the
 *                 counters on 0x8430e0 (flower +0,4,6,10,14 / concentrate
 *                 +2,4,8,12,16), save arming +31 = 50 / +33 = 0, stop 0x97f0,
 *                 cue 0x843260 +3 = 5 / +4 = 2; on screen 15 it zeroes the
 *                 countdown instead
 *   0xb8b8 (40 Hz): stops a session that's still cold (+0x1e <= 54 C) after
 *                 300 ticks, or never reached (0x84531d, set with reached at
 *                 0xb58c, cleared only by the stop) after 2600 ticks, and any
 *                 session after 270000 ticks (~112 min); the screensaver
 *                 (screen 16) only starts with no session
 */
#ifndef DEVICE_H
#define DEVICE_H

#define DEV_STRUCT          0x843028
#define OFF_SESSION         0x01   /* 1 = session running; cleared by 0x97f0 */
#define OFF_REACHED         0x02   /* 0 = af2c reloads the target every tick */
#define OFF_COUNTDOWN       0x1a   /* seconds; counts only while +0x2 (reached) is set */
#define OFF_MEAS_F          0x1c
#define OFF_MEAS_C          0x1e
#define OFF_SCREEN          0x79   /* 1 = live heating */

#define DEV_SCALE_IS_F()    (STRUCT_BASE[0x07] != 0)
#define DEV_MODE_IS_CONC()  (STRUCT_BASE[0x09] != 0)
#define DEV_RANK(conc)      (STRUCT_BASE[(conc) ? 0x0b : 0x0a])

/* preset tables: 6 x u16 (custom + 5 slots); entry = (rank + base) * 2 + 2 */
#define DEV_PRESET_OFF(base, rank)  (((rank) + (base)) * 2 + 2)
#define TBL_FL_F            0x14
#define TBL_FL_C            0x1a
#define TBL_CO_F            0x20
#define TBL_CO_C            0x26
#define TBL_FL_HOLD         0x2c
#define TBL_CO_HOLD         0x32

#define DEV_DAB_BASE        0x8430e0
#define DEV_SAVE_ARM(d)     do { (d)[31] = 50; (d)[33] = 0; } while (0)
#define DEV_END_CUE         0x843260
/* Settings save, every preset slot included (0x80000: rank 0 at 0x6740-0x679c,
 * ranks 1-5 in the loop at 0x67a6): 0x63b8 counts 0x843100 (= DEV_DAB_BASE +
 * 32 = 0x8430a8 + 0x58) down and saves at zero (0x64da -> 0x6524); the app's
 * settings / preset-table / power-off commands arm it with 100 (0xfc9e,
 * 0x10d46, 0x10da0, 0x11c2c). */
#define DEV_SAVE_TIMER      (*(volatile u8 *)(DEV_DAB_BASE + 32))
#define DEV_SAVE_DELAY      100

#define DEV_PID_TICK        0xaf2c
#define DEV_STOP            0x97f0
#define DEV_ROM_DIV         0x1ac

#define DEV_RAMP_FLASH      0x32000   /* code gets 0x30000-0x31fff */
#define DEV_FLASH_READ      0x9c0
#define DEV_FLASH_ERASE     0x924
#define DEV_FLASH_WRITE     0x19a78   /* write, read back, retry x3. (0x19308 is the
                                       * PROD-071024 address -- mid-function in 111224) */
/* 0x19a78 (addr, len, buf): page program 0x964, then reads the bytes back into
 * a 64-byte stack buffer (sub sp, #0x40, just below the saved r8) and compares
 * (0x13a80). Any len over 64 overwrites the caller's saved r8. The store is
 * 66 bytes, so it's written in pieces of at most 64. */
#define DEV_FLASH_WRITE_MAX 64

#define DEV_RAMP_STATE      0x848000

#define RAMP_TRACE_LEN      210   /* the ramp chart width in columns (ramp_display.c) */

#define DEV_MAX_F           635      /* official app's concentrate ceiling */

/* Notify path (SDK bls_att_pushNotifyData(handle, data, len), returns 0 when
 * queued): 0x15a34, confirmed by its body building an ATT Handle Value
 * Notification. Handle 27 is the read/notify characteristic every stock reply
 * uses. 0x11562 is the stock send of the 0xAA dab-counter reply, which the
 * patch wraps to announce itself (ramp_announce.c). */
#define DEV_NOTIFY          0x15a34
#define DEV_NOTIFY_HANDLE   27
#define DEV_ID              1

/* Preset picker (ramp_input.c): a single click on the idle live view opens it,
 * + and - step through the six presets, a double click switches the ramp
 * system on or off, a click leaves it. Events are the stock consumer's codes
 * (decoder 0xbe00-0xc180): 1 = - short, 2 = + short, 3 = - held, 4 = + held,
 * 7..11 = 1..5+ clicks, 12 = 4 clicks + hold, 13 = main long hold, 14 = any
 * press (posted first).
 *
 * Screens (+0x79; the consumer's table pointer 0x1a3c0 is a runtime address,
 * the table is at disassembly 0x1a398): 0 = off / asleep, 1 = the live view
 * (idle, and the heating screen in a session), 4 / 12 = only while the device
 * is locked, 5-9 = editors and menus. +0x82 is the device lock -- the 0x99
 * status packet's "Device Locked" bit (byte 16, 0x10) is built from it
 * (0x121d0, base 0x84309c + 0xe), and four clicks toggle it.
 *
 * Unlocked, on the idle live view (screen 1, no session), stock uses: + / -
 * short or held (editors, 0x61c8 / 0x610a), double click (start), triple click
 * (menu, 0x5da2), long hold (preset cycle, 0x61f6), four clicks (lock), four
 * clicks + hold (low power), five clicks (power). A single click wakes the
 * screen from the screensaver back to the main view (seen on the device; the
 * consumer path 0x5708 -> 0x571e -> 0x5728 -> return doesn't show it, so the
 * wake happens elsewhere). Every gesture is taken, so the picker is DISABLED
 * on the Carta 2: DEV_PICK_ENTER is a code no event has, and nothing opens it.
 * Ramps still start from the app or a sentinel slot, using the stored preset
 * choice (Balanced until one is set). The rest of the picker code is kept for
 * a future entry that doesn't shadow a stock gesture. */
#define OFF_LOCKED          0x82
#define SCREEN_LIVE         1
#define DEV_PICKER_SCREEN() (STRUCT_BASE[OFF_SCREEN] == SCREEN_LIVE && STRUCT_BASE[OFF_LOCKED] == 0)
/* The session timer (0xb5b8) counts only while "reached" is set. */
#define DEV_CLOCK_WAITS_FOR_REACHED  1
/* Screen 15: the orchestrator (0xaf2c) forces its own 85 C / 184 F target and
 * the session timer zeroes the countdown -- the Carta 2's quick heat. No code
 * in this build writes 15 to the screen, but if it's ever there a sentinel
 * slot must not turn it into a ramp. */
#define DEV_ARM_BLOCKED()   (STRUCT_BASE[OFF_SCREEN] == 15)
#define DEV_PICK_COUNT      6
#define DEV_PICK_ENTER      (-2)   /* disabled: no free gesture (see above) */
#define DEV_PICK_NEXT       2
#define DEV_PICK_PREV       1
#define DEV_PICK_EXIT       7
#define DEV_PICK_TOGGLE     8      /* double click: ramp system on / off */
void ramp_picker_closed_redraw(void);
#define DEV_PICKER_CLOSED() ramp_picker_closed_redraw()

#define DEV_AFTER_TICK(st)  ((void)0)   /* the screen draws from its own hooks */

#endif
