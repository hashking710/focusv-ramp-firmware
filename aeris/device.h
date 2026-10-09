/* device.h -- Aeris, PROD-111224. Each value confirmed from the stock code that
 * consumes it:
 *   FUN_00008154  orchestrator: session gate +0x0, reload gate +0x1, mode +0x6
 *                 (1 = flower, otherwise concentrate), ranks +0x7 / +0x8, target
 *                 pair +0x2c (C) / +0x2e (F) filled from the active slot; measured
 *                 +0x2a (C, 25 + delta) and +0x28 (F = C*9/5+32)
 *   FUN_000079cc  PID step: +0x4 == 1 -> (+0x2c - +0x2a) else (+0x2e - +0x28)
 *   0xb3c6 / 0xb940  0xCC handler: C path writes the C tables (+0x3c / +0x54),
 *                 F path the F tables (+0x30 / +0x48); with the PID pairing above
 *                 that makes +0x4 == 1 Celsius (opposite to the Carta 2)
 *   0x8980 session timer: +0x1a, decremented only once +0x1 (reached) is set;
 *                 at zero: stop 0x7200, bookkeeping on 0x8432ec (+31 = 200,
 *                 +32 = 250), cue 0x84324c
 * 0xCC handler (traced, 0xb3bc-0xb490): packet at r7+0x28, big-endian, never
 *                 clamped; C branch stores flower/conc C -> +0x3c/+0x54 and
 *                 F = floor(9c/5)+32 -> +0x30/+0x48; holds -> +0x60/+0x6c;
 *                 ranks -> +0x07/+0x08 -- all the custom (rank 0) slot. The
 *                 marker (byte 13, read at 0xb490 = the hook) is only acted on
 *                 while UI state 0x84308c+2 is 1 or 8; otherwise the packet is
 *                 dropped without a reply (the app wakes the device first).
 * Single click / hold during a session already stops it in stock (0x4ff4 ->
 * 0x50c4 -> 0x7200; quick-heat state 7 stops on any press), so no button hook.
 */
#ifndef DEVICE_H
#define DEVICE_H

#define DEV_STRUCT          0x8430e4
#define OFF_SESSION         0x00
#define OFF_REACHED         0x01   /* also gates the session clock */
#define OFF_COUNTDOWN       0x1a
#define OFF_MEAS_F          0x28
#define OFF_MEAS_C          0x2a

#define DEV_SCALE_IS_F()    (STRUCT_BASE[0x04] != 1)
#define DEV_MODE_IS_CONC()  (STRUCT_BASE[0x06] != 1)
#define DEV_RANK(conc)      (STRUCT_BASE[(conc) ? 0x08 : 0x07])

/* preset tables: entry = (rank + base) * 2 */
#define DEV_PRESET_OFF(base, rank)  (((rank) + (base)) * 2)
#define TBL_FL_F            0x18
#define TBL_FL_C            0x1e
#define TBL_CO_F            0x24
#define TBL_CO_C            0x2a
#define TBL_FL_HOLD         0x30
#define TBL_CO_HOLD         0x36

#define DEV_DAB_BASE        0x8432ec
#define DEV_SAVE_ARM(d)     do { (d)[31] = 200; (d)[32] = 250; } while (0)
#define DEV_END_CUE         0x84324c
/* Settings save: 0x5658 counts d[32] down and saves at zero (0x5762); button
 * changes and the session completion arm it with 250, as on the Sport. */
#define DEV_SAVE_TIMER      (*(volatile u8 *)(DEV_DAB_BASE + 32))
#define DEV_SAVE_DELAY      250

#define DEV_PID_TICK        0x8154
#define DEV_STOP            0x7200
#define DEV_ROM_DIV         0x1ac

/* The ramp store's own sector, outside both OTA banks: stock erases the bank
 * it isn't running from at every boot (the SDK's init and the app's wipe
 * loops 0x6004 / 0x637a / 0x692e), so a store inside either bank is lost
 * after an OTA install. Stock flash use (every erase site traced): banks
 * 0x0-0x3ffff, settings 0x40000-0x43fff (0x57d0, 0x6d88, 0x66dc, 0x5d7e),
 * session log ring 0x45000 + (n % 28) * 0x1000 up to 0x60fff (0x573a), SDK
 * pairing 0x74000 and MAC / calibration 0xff000 / 0xfe000 (.data). 0x61000-
 * 0x73fff is unused. Code gets 0x14000-0x16fff. */
/* Two sectors: 0x70000 and 0x71000 (RAMP_STORE_ALT), one copy each. */
#define DEV_RAMP_FLASH      0x70000
#define DEV_FLASH_READ      0xab8
#define DEV_FLASH_ERASE     0xa1c
#define DEV_FLASH_WRITE     0xa5c

#define DEV_RAMP_STATE      0x848000

#define RAMP_TRACE_LEN      1   /* no screen on this device; the chart trace array is unused */

#define DEV_MAX_F           600      /* official app's concentrate ceiling */

/* Notify path (SDK bls_att_pushNotifyData(handle, data, len), returns 0 when
 * queued): 0xe734, confirmed by its body building an ATT Handle Value
 * Notification. Handle 27 is the read/notify characteristic every stock reply
 * uses. 0xb066 is the stock send of the 0xAA dab-counter reply, which the
 * patch wraps to announce itself (ramp_announce.c). */
#define DEV_NOTIFY          0xe734
#define DEV_NOTIFY_HANDLE   27
#define DEV_ID              2

/* Button events (consumer 0x4ee8, mailbox 0x845620, UI state 0x84308c+2 = 1
 * on the idle screen). A hold from idle opens the preset picker; single clicks
 * step through the four presets, a triple click switches the ramp system on or
 * off, and a hold leaves it.
 *
 * The button scanner (0x8c60) posts 16 on every press and 15 once a press is
 * held 200 scans; on release it posts 7..11 for 1..5+ presses, and a long
 * press posts no click. Presses of the current gesture count at 0x84308c+0x1d,
 * so a hold that ends a multi-press gesture -- 2 presses + hold (13), 4 presses
 * + hold (12: dim mode, +0x1c), 7 presses + hold (14) -- posts its 15 with a
 * count above 1: only a hold from a single press opens the picker. The 0xCC
 * handler posts 18 / 19 / 20 for the app's A5 / AF / 66 markers (0xb934,
 * 0xbe3a, 0xbe2c). */
#define DEV_EV_CLICKS()     (*(volatile u8 *)(0x84308c + 0x1d))
#define DEV_EV_APP_MIN      18
/* 11 is power off / sleep: five presses, but also posted by the app's power-off
 * handling (0xb71a, 0xb9bc) and by the sleep request at 0x53b8 -- the picker
 * never takes it. */
#define DEV_EV_POWER_OFF    11
/* The consumer's prelude, run for every pending event, resets the idle
 * auto-off timer (+0x3f/+0x40) and refreshes 0x8432ec+0x20. Events the picker
 * takes are handed on as 16 (a press), which state 1 ignores, so that still
 * runs. */
#define DEV_EV_NOOP         16
/* A hold (15) is posted at 200 scans of the press, whatever the press count;
 * stock's longer multi-press holds use counts 2, 4 and 7, and a release after
 * a long press clears the count, so the five-click power off never follows.
 * Five presses with the fifth held -- a hold with the count at 5 -- does
 * nothing in stock when idle (0x47be / 0x50f4): it's the mode switch. */
#define DEV_EV_HOLD         15
/* The user's LED preset, 0 = off (cycled by event 9 in the consumer). With it
 * off the picker doesn't open: it would be an invisible mode taking clicks. */
#define DEV_LEDS_ON()       (*(volatile u8 *)(0x84308c + 0x0e))
#define DEV_EV_MB           0x845620
#define DEV_EV_CONSUMER     0x4ee8
#define DEV_UI_STATE_ADDR   0x84308e
#define DEV_PICK_COUNT      4
#define DEV_PICK_ENTER      15
#define DEV_PICK_NEXT       7
#define DEV_PICK_PREV       (-1)
#define DEV_PICK_EXIT       15
#define DEV_PICK_TOGGLE     9      /* triple click: ramp system on / off */
/* The stock consumer drops every event while 0x84308c+7 is set and +9 == 1
 * (the power-on transition); the picker honours the same gate. */
#define DEV_EV_IGNORED()    ((*(volatile u8 *)(0x84308c + 7)) != 0 && (*(volatile u8 *)(0x84308c + 9)) == 1)
#define DEV_IDLE()          ((*(volatile u8 *)DEV_UI_STATE_ADDR) == 1 && STRUCT_BASE[OFF_SESSION] == 0)
/* UI state 7 is quick heat (two presses + a long hold from sleep, consumer
 * 0x50a6 -> session start 0x7394): the orchestrator (0x81aa -> 0x85ac) forces
 * its own 80 C / 176 F target, the first reach zeroes the countdown, and it
 * runs until a press stops it. A sentinel slot must not turn it into a ramp --
 * it would take over the countdown, end it and could count it as a dab. */
#define DEV_ARM_BLOCKED()   ((*(volatile u8 *)DEV_UI_STATE_ADDR) == 7)
/* The session timer decrements the countdown only once "reached" (+0x1) is
 * set -- see the header comment. */
#define DEV_CLOCK_WAITS_FOR_REACHED  1


void ramp_led_tick(void);
#define DEV_AFTER_TICK(st)  ramp_led_tick()

#endif
