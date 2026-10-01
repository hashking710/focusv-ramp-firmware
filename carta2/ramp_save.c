/* ramp_save.c
 *
 * Custom-ramp waypoint storage, saved via the real app's own custom-
 * temperature entry flow -- no new top-level BLE opcode, no dispatcher
 * patch. Confirmed facts this relies on (re-derived fresh this session
 * directly against the real PROD-111224 build, after an earlier pass's
 * addresses for this exact file turned out to be stale -- see
 * firmware-analysis-notes.md and the "corrected address" note below):
 *
 *   - The real 0xCC/SET_TEMP handler is FUN_00011cc4, reached from the
 *     real top-level opcode dispatcher FUN_000115f2 (which checks the
 *     incoming packet's opcode byte against 0xCC, 0xAE, 0xAF, 0xD1, 0xDD,
 *     0x83, 0x80, 0x81, 0xE1-0xE4 -- matches ble-protocol.md exactly). By
 *     the time control reaches the marker-dispatch block below, the
 *     handler chain has ALREADY parsed and written the packet's
 *     temperature (Celsius, converted) into struct+0x36 (flower) /
 *     struct+0x4e (concentrate), and its duration into struct+0x5a
 *     (flower) / struct+0x66 (concentrate) -- same as previously
 *     documented, independently re-confirmed via decompile of
 *     FUN_00011cc4 this session.
 *   - The marker-dispatch block itself lives inside a different, larger
 *     merged function (FUN_00011d82 in Ghidra's current auto-analysis --
 *     one of several giant functions this build's auto-analysis merges
 *     unrelated opcode-handling logic into; see methodology.md), at
 *     0x11d96-0x11db1:
 *         0x11d96: tmovs r3, #0x28
 *         0x11d98: tloadrb r3, [r5, r3]   -- r3 = marker byte (r5 = the
 *                                            incoming packet buffer; +0x28
 *                                            is the marker byte position
 *                                            within it, confirmed via
 *                                            decompile, matches this
 *                                            file's original doc)
 *         0x11d9a: tcmp r3, #0xa5 -> call FUN_000125f2 (start)
 *         0x11da2: tcmp r3, #0xaf -> call FUN_000125e6 (stop)
 *         0x11daa: tcmp r3, #0x66 -> call FUN_00012726 (test-fire)
 *     None of 0xB1-0xB5 match any of these three checks, so a packet
 *     carrying one of those markers falls through this chain untouched by
 *     the stock firmware -- confirmed harmless no-op on stock firmware,
 *     exactly as intended.
 *   - CORRECTED ADDRESS, this session: an earlier pass of this same file
 *     patched the 0x66/test-fire call specifically (at what was then
 *     thought to be 0x1163e), on the theory that intercepting just that
 *     one leaf and manually re-dispatching 0x66 was sufficient. Re-deriving
 *     the real control flow this session (direct decompile of
 *     FUN_00011d82) showed that's wrong for two reasons: the real test-fire
 *     call site is 0x11dae, not 0x1163e, AND -- more importantly --
 *     intercepting only the 0x66 leaf can never see markers 0xB1-0xB5 at
 *     all, since execution only reaches that leaf when the marker already
 *     equals 0x66. The fix patches the marker LOAD itself (0x11d96, before
 *     any of the three comparisons), not any one leaf -- see
 *     marker_entry.s. The 0xA5/0xAF/0x66 chain downstream is left
 *     completely untouched by this patch; this file no longer needs to
 *     know anything about FUN_00012726 or replicate its behaviour.
 *   - FUN_00000964/FUN_000009c0/FUN_00000924 (write/read/erase) and
 *     FUN_00019308 (write-verify-retry) are the confirmed real flash
 *     primitives used throughout this firmware; reused here rather than
 *     inventing new flash-write logic.
 *
 * Storage: a dedicated flash sector at 0x31000 -- the sector immediately
 * after the one holding the injected code (0x30000), so an erase-and-
 * rewrite here never touches that code. Confirmed free flash ahead of the
 * OTA staging area; see firmware-architecture.md.
 */

typedef unsigned char  u8;
typedef unsigned short u16;

#define STRUCT_BASE          ((volatile u8 *)0x843028)
#define MODE_FLAG_OFF        0x7
#define CUSTOM_FLOWER_OFF    0x36
#define CUSTOM_CONC_OFF      0x4e
#define DUR_FLOWER_OFF       0x5a
#define DUR_CONC_OFF         0x66

#define RAMP_FLASH_SECTOR 0x31000
#define RAMP_NUM_SLOTS    5
#define RAMP_SLOT_SIZE    4   /* u16 temp_c + u16 duration */

typedef int (*flash_read_fn)(int addr, int len, void *buf);
#define flash_read ((flash_read_fn)0x9c0)          /* FUN_000009c0 */

typedef void (*flash_erase_fn)(int addr);
#define flash_erase ((flash_erase_fn)0x924)        /* FUN_00000924 */

typedef int (*flash_write_verify_fn)(int addr, int len, void *buf);
#define flash_write_verify ((flash_write_verify_fn)0x19308)  /* FUN_00019308 */

static void save_ramp_waypoint(u8 slot, u16 temp_c, u16 duration)
{
    u8 buf[RAMP_NUM_SLOTS * RAMP_SLOT_SIZE];

    if (slot >= RAMP_NUM_SLOTS)
        return;

    /* NOR flash requires a full-sector erase before any bit can go
     * 0->1, and this sector holds all 5 slots together -- read the
     * other 4 slots first so saving one waypoint doesn't erase the
     * rest. */
    flash_read(RAMP_FLASH_SECTOR, sizeof(buf), buf);
    flash_erase(RAMP_FLASH_SECTOR);

    buf[slot * RAMP_SLOT_SIZE + 0] = (u8)temp_c;
    buf[slot * RAMP_SLOT_SIZE + 1] = (u8)(temp_c >> 8);
    buf[slot * RAMP_SLOT_SIZE + 2] = (u8)duration;
    buf[slot * RAMP_SLOT_SIZE + 3] = (u8)(duration >> 8);

    flash_write_verify(RAMP_FLASH_SECTOR, sizeof(buf), buf);
}

/* Called from ramp_marker_entry (marker_entry.s) with the marker byte in
 * r0, for EVERY packet that reaches the marker-dispatch point -- not just
 * ones this file cares about. Markers 0xA5/0xAF/0x66 (and anything else)
 * are left completely alone; the untouched stock chain immediately after
 * the call site handles those exactly as it always did, since this
 * function's caller restores r3 = marker byte before returning there
 * regardless of what happens here. This function only acts on the five
 * new ramp-waypoint markers, which the stock firmware never uses. */
void ramp_marker_dispatch(u8 marker)
{
    if (marker >= 0xb1 && marker <= 0xb5) {
        u8 mode = STRUCT_BASE[MODE_FLAG_OFF];
        u16 temp_c = (mode == 0)
            ? *(volatile u16 *)(STRUCT_BASE + CUSTOM_FLOWER_OFF)
            : *(volatile u16 *)(STRUCT_BASE + CUSTOM_CONC_OFF);
        u16 duration = (mode == 0)
            ? *(volatile u16 *)(STRUCT_BASE + DUR_FLOWER_OFF)
            : *(volatile u16 *)(STRUCT_BASE + DUR_CONC_OFF);
        save_ramp_waypoint((u8)(marker - 0xb1), temp_c, duration);
    }
}
