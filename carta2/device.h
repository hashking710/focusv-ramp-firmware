/* device.h -- Carta 2 ("Quantum"), PROD-111224. Each value confirmed from the
 * stock code that consumes it:
 *   FUN_0000af2c  orchestrator: session gate +0x1, reload gate +0x2, mode +0x9,
 *                 target pair +0x20 (C) / +0x22 (F) filled from the active slot
 *   FUN_0000a7f4  PID step: +0x7 != 0 -> (+0x22 - +0x1c) else (+0x20 - +0x1e)
 *   FUN_00011cc4  0xCC handler: writes +0x7 from the packet scale byte (1 = F)
 *                 (traced, C branch 0x11cd8-0x11d96): big-endian, never clamped;
 *                 C -> +0x36/+0x4e, F = floor(9c/5)+32 -> +0x2a/+0x42, holds ->
 *                 +0x5a/+0x66 (the custom, rank 0 slot); the marker (byte 13, read
 *                 at 0x11d96 = the hook) is only acted on in UI states 16 or 4,
 *                 otherwise dropped without a reply (the app wakes it first)
 *   0xb5c2..0xb6f6 session timer: +0x1a countdown, completion bookkeeping on
 *                 0x8430e0, save arming +31/+33, stop 0x97f0, cue 0x843260
 */
#ifndef DEVICE_H
#define DEVICE_H

#define DEV_STRUCT          0x843028
#define OFF_SESSION         0x01   /* 1 = session running; cleared by 0x97f0 */
#define OFF_REACHED         0x02   /* 0 = af2c reloads the target every tick */
#define OFF_COUNTDOWN       0x1a   /* seconds; runs from the start of a session */
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

#define DEV_PID_TICK        0xaf2c
#define DEV_STOP            0x97f0
#define DEV_ROM_DIV         0x1ac

#define DEV_RAMP_FLASH      0x32000   /* code gets 0x30000-0x31fff */
#define DEV_FLASH_READ      0x9c0
#define DEV_FLASH_ERASE     0x924
#define DEV_FLASH_WRITE     0x19a78   /* write, read back, retry x3. (0x19308 is the
                                       * PROD-071024 address -- mid-function in 111224) */

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

/* Preset picker (ramp_input.c): a hold of - from the idle screen (0) opens it,
 * + and - step through the six presets, a double click switches the ramp
 * system on or off, a click leaves it. Events are the
 * stock consumer's codes: 1 = - short, 2 = + short, 3 = - held, 7 = click. */
#define DEV_PICK_COUNT      6
#define DEV_PICK_ENTER      3
#define DEV_PICK_NEXT       2
#define DEV_PICK_PREV       1
#define DEV_PICK_EXIT       7
#define DEV_PICK_TOGGLE     8      /* double click: ramp system on / off */
void ramp_picker_closed_redraw(void);
#define DEV_PICKER_CLOSED() ramp_picker_closed_redraw()

#define DEV_AFTER_TICK(st)  ((void)0)   /* the screen draws from its own hooks */

#endif
