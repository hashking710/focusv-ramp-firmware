/* device.h -- Carta Sport, PROD-030426. Each value confirmed from the stock code
 * that consumes it:
 *   FUN_00007c00  orchestrator: reload gate +0x1, mode +0x6 (1 = flower,
 *                 otherwise concentrate), ranks +0x7 / +0x8, target pair
 *                 +0x2c (C) / +0x2e (F) filled from the active slot
 *                 ((rank + 0x1e)*2 / (rank + 0x18)*2 flower, +0x2a / +0x24
 *                 concentrate); measured +0x2a (C) / +0x28 (F) -- the
 *                 out-of-range fallback writes 25 / 77 there; countdown +0x1a
 *                 reloaded from the hold tables (rank + 0x30)*2 / (rank + 0x36)*2
 *   0x8448        session timer: +0x1a, decremented only once +0x1 (reached) is
 *                 set; at zero: stop 0x6a98, counters on 0x842910 (flower +0,4,6,
 *                 10,14 / concentrate +2,4,8,12,16), +31 = 200, +32 = 250,
 *                 cue 0x842870 +3 = 5, +4 = 2 -- identical to the Aeris
 *   0x2948/0x294c the SDK's flash read / write pointers: 0xc0f5 / 0xc111
 *                 (= 0xc0cc / 0xc0e8 + 0x28, bit 0 set), both (addr, len, buf);
 *                 erase 0xc178 (addr), sector erase 0x20 -- the settings save
 *                 uses all three
 *   0x1529c       hardware divider, quotient (the same 1/2/0 entry table the
 *                 other two devices have at 0x1ac)
 * 0xCC handler (traced, 0xaf2e-0xb002): packet at r7+0x28, big-endian, never
 *                 clamped. C branch: flower/conc C -> +0x3c/+0x54, F = floor(
 *                 9c/5)+32 -> +0x30/+0x48. F branch (0xb706): F -> +0x30/+0x48,
 *                 C = floor(5(f-32)/9) -> +0x3c/+0x54 (150 F -> 65 C). Holds ->
 *                 +0x60/+0x6c, ranks -> +0x07/+0x08 -- the custom (rank 0) slot.
 *                 The marker (byte 13, read at 0xb002 = the hook) is only acted
 *                 on while UI state 0x842694+3 is 1 (the awake screen) or 8;
 *                 otherwise dropped without a reply (the app wakes it first).
 * Single click during a session already stops it in stock (consumer 0x45cc,
 * state 1: event 11 -> 0x6a98 unconditionally, events 7 / 15 / 19 -> 0x6a98
 * while heating), so, like the Aeris, no button hook.
 */
#ifndef DEVICE_H
#define DEVICE_H

#define DEV_STRUCT          0x8426ec
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

#define DEV_DAB_BASE        0x842910
#define DEV_SAVE_ARM(d)     do { (d)[31] = 200; (d)[32] = 250; } while (0)
#define DEV_END_CUE         0x842870
/* Settings save (preset slots included, sector 0x40000): 0x4ce0 counts d[32]
 * down and saves at zero (0x4df4 -> 0x4e60); idle button changes arm it with
 * 250 (0x4748, 0x46f6, ...) and the session completion too (0x8502). */
#define DEV_SAVE_TIMER      (*(volatile u8 *)(DEV_DAB_BASE + 32))
#define DEV_SAVE_DELAY      250

#define DEV_PID_TICK        0x7c00
#define DEV_STOP            0x6a98
#define DEV_ROM_DIV         0x1529c

/* The ramp store's own sector, outside both OTA banks: stock erases the bank
 * it isn't running from at every boot (SDK init 0xeb4c -> 0xe9b8, and the
 * app's own wipe 0x5b80-0x5cc0: 32 sectors at 0x0 or 0x20000), so a store
 * inside either bank is lost after an OTA install. Stock flash use (every
 * erase site traced): banks 0x0-0x3ffff, settings 0x40000-0x43fff (0x4e68,
 * 0x60ee, 0x5b08, 0x5410), session log ring 0x45000 + (n % 28) * 0x1000 up to
 * 0x60fff (0x4dc2), SDK pairing / MAC / calibration from 0x74000 (.data
 * 0x2654 / 0x264c / 0x2650). 0x61000-0x73fff is unused. Code gets
 * 0x18000-0x1afff. */
#define DEV_RAMP_FLASH      0x70000
#define DEV_FLASH_READ      0xc0cc
#define DEV_FLASH_ERASE     0xc178
#define DEV_FLASH_WRITE     0xc0e8

#define DEV_RAMP_STATE      0x848000

#define RAMP_TRACE_LEN      1   /* no screen on this device; the chart trace array is unused */

#define DEV_MAX_F           635      /* official app's concentrate ceiling */

/* Notify path (SDK bls_att_pushNotifyData(handle, data, len), returns 0 when
 * queued): 0xec3c, confirmed by its body building an ATT Handle Value
 * Notification. Handle 27 is the read/notify characteristic every stock reply
 * uses. 0xa9ea is the stock send of the 0xAA dab-counter reply, which the
 * patch wraps to announce itself (ramp_announce.c). */
#define DEV_NOTIFY          0xec3c
#define DEV_NOTIFY_HANDLE   27
#define DEV_ID              3

/* Button events (consumer 0x45cc, mailbox 0x844b30, UI state 0x842694+3 = 1
 * on the awake screen). A hold from idle opens the preset picker; single clicks
 * step through the four presets, a triple click switches the ramp system on or
 * off, and a hold leaves it.
 *
 * The button scanner (0x8868) posts 16 on every press and 15 once a press is
 * held 200 scans; on release it posts 7..11 for 1..5+ presses, and a long
 * press posts no click. Presses of the current gesture count at 0x842694+0x20,
 * so a hold that ends a multi-press gesture -- 2 presses + hold (13), 4 presses
 * + hold (12: dim mode), 7 presses + hold (14) -- posts its 15 with a count
 * above 1: only a hold from a single press opens the picker. The 0xCC handler
 * posts 18 / 19 / 20 for the app's A5 / AF / 66 markers (start, stop, +10 s). */
#define DEV_EV_CLICKS()     (*(volatile u8 *)(0x842694 + 0x20))
#define DEV_EV_APP_MIN      18
/* 11 is power off / sleep: five presses, but also posted by the app's power-off
 * command (0xCC handler, 0xb300) and re-posted from 0x844baa by 0x4934 -- the
 * picker never takes it. */
#define DEV_EV_POWER_OFF    11
/* The user's LED preset, 0 = off (cycled by event 9 in the consumer). With it
 * off the picker doesn't open: it would be an invisible mode taking clicks. */
#define DEV_LEDS_ON()       (*(volatile u8 *)(0x842694 + 0x0f))
/* The consumer's prelude, run for every pending event, resets the idle
 * auto-off timer (+0x42) and refreshes 0x842910+0x20. Events the picker takes
 * are handed on as 16 (a press), which state 1 ignores, so that still runs. */
#define DEV_EV_NOOP         16
#define DEV_EV_MB           0x844b30
#define DEV_EV_CONSUMER     0x45cc
#define DEV_UI_STATE_ADDR   0x842697
#define DEV_PICK_COUNT      4
#define DEV_PICK_ENTER      15
#define DEV_PICK_NEXT       7
#define DEV_PICK_PREV       (-1)
#define DEV_PICK_EXIT       15
#define DEV_PICK_TOGGLE     9      /* triple click: ramp system on / off */
/* The stock consumer drops every event while 0x842694+8 is set and +10 == 1
 * (the power-on transition); the picker honours the same gate. */
#define DEV_EV_IGNORED()    ((*(volatile u8 *)(0x842694 + 8)) != 0 && (*(volatile u8 *)(0x842694 + 10)) == 1)
#define DEV_IDLE()          ((*(volatile u8 *)DEV_UI_STATE_ADDR) == 1 && STRUCT_BASE[OFF_SESSION] == 0)
/* UI state 7 is quick heat (two presses + a long hold from sleep, consumer
 * 0x4778 -> session start 0x6c30): the orchestrator forces its own 90 C /
 * 193 F target, the first reach zeroes the countdown, and it runs until a
 * press stops it. A sentinel slot must not turn it into a ramp -- it would
 * take over the countdown, end it and could count it as a dab. */
#define DEV_ARM_BLOCKED()   ((*(volatile u8 *)DEV_UI_STATE_ADDR) == 7)
/* The session timer decrements the countdown only once "reached" (+0x1) is
 * set -- see the header comment. */
#define DEV_CLOCK_WAITS_FOR_REACHED  1


void ramp_led_tick(void);
#define DEV_AFTER_TICK(st)  ramp_led_tick()

#endif
