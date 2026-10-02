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

#define DEV_PID_TICK        0x7c00
#define DEV_STOP            0x6a98
#define DEV_ROM_DIV         0x1529c

#define DEV_RAMP_FLASH      0x19000   /* code gets 0x18000-0x18fff */
#define DEV_FLASH_READ      0xc0cc
#define DEV_FLASH_ERASE     0xc178
#define DEV_FLASH_WRITE     0xc0e8

#define DEV_RAMP_STATE      0x848000

#define RAMP_TRACE_LEN      1   /* no screen on this device; the chart trace array is unused */

#define DEV_MAX_F           635      /* official app's concentrate ceiling */

void ramp_led_update(void);
#define DEV_AFTER_TICK(st)  ramp_led_update()

#endif
