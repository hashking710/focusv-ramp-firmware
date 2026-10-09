/* ramp_store.c -- waypoint storage, shared by every device.
 *
 * Reached from the device's marker hook (ramp_marker_entry.s), installed at the
 * 0xCC handler's marker-byte load, after the stock handler has already parsed
 * the packet into the custom (rank 0) preset slots in BOTH units -- the unit the
 * packet was sent in exactly, the other via the firmware's own conversion -- and
 * the hold time into the duration tables. Those values are stored as-is, so a
 * stage plays back with no rounding of this patch's own.
 *
 * Markers (all fall through the stock A5 / AF / 66 compare chain untouched):
 *   0xB1-0xB5  flower waypoints 1-5        (bank 0)
 *   0xB6-0xBA  concentrate waypoints 1-5   (bank 1)
 *   0xBB       setup offset (byte 14, signed F)
 *   0xBD       stock / ramp mode (byte 14 'S' / 'R')
 *   0xBE       built-in preset choice (byte 14, 0 .. DEV_PICK_COUNT - 1)
 *   0xA5 + 'R' in byte 14: run the session it starts as a ramp
 * Waypoint 1 of a bank clears that bank's waypoints 2-5; a ramp is the saved
 * prefix up to the first empty (hold 0 / erased) waypoint.
 * The mode comes from the marker, not the device: the attached atomizer decides
 * the device's mode, and flower waypoints saved with a concentrate atomizer
 * attached must still be stored as flower.
 */
#include "ramp.h"

/* ---- two copies -------------------------------------------------------------
 * The store lives in two sectors (DEV_RAMP_FLASH, RAMP_STORE_ALT). A copy
 * counts only if its magic is right and its commit byte reads 0x00; the
 * commit byte is written last, after the rest of the copy, so a save cut
 * short by a power loss -- erased sector, half the bytes, everything but the
 * commit byte -- is simply not a copy, and the other one is still there. Of
 * two complete copies the one with the newer sequence byte (wrapping) is
 * current. A save always writes the copy that isn't current. */
static u8 copy_ok(u32 a, u8 *seq)
{
    u8 b[2];
    flash_read(a, 2, b);
    if (b[0] != (u8)RAMP_STORE_MAGIC || b[1] != (u8)(RAMP_STORE_MAGIC >> 8))
        return 0;
    flash_read(a + RAMP_SEQ_OFFSET, 2, b);   /* seq, commit */
    if (b[1] != 0)
        return 0;
    *seq = b[0];
    return 1;
}

u32 ramp_store_addr(void)
{
    u8 s0 = 0, s1 = 0;
    u8 ok0 = copy_ok(DEV_RAMP_FLASH, &s0), ok1 = copy_ok(RAMP_STORE_ALT, &s1);
    if (ok0 && ok1)
        return (u8)(s1 - s0) < 0x80 ? RAMP_STORE_ALT : DEV_RAMP_FLASH;
    return ok1 ? RAMP_STORE_ALT : ok0 ? DEV_RAMP_FLASH : 0;
}

/* The whole current store into buf; a fresh one if there is none. */
static void store_load(u8 *buf)
{
    u32 a = ramp_store_addr();
    int i;
    if (a) {
        flash_read(a, RAMP_STORE_TOTAL, buf);
        return;
    }
    for (i = 0; i < RAMP_STORE_TOTAL; i++)
        buf[i] = 0xff;
    buf[0] = (u8)RAMP_STORE_MAGIC;
    buf[1] = (u8)(RAMP_STORE_MAGIC >> 8);
}

/* Writes buf as the new current copy -- unless the current copy already
 * holds exactly this, so repeated uploads, an unchanged picker choice or the
 * same offset cost no erase cycle. */
static void store_commit(u8 *buf)
{
    u8 cur[RAMP_STORE_TOTAL];
    u8 zero = 0;
    u32 a = ramp_store_addr(), dst = DEV_RAMP_FLASH;
    int i;

    buf[RAMP_VER_OFFSET] = RAMP_STORE_VERSION;
    buf[RAMP_SEQ_OFFSET] = 0;
    if (a) {
        flash_read(a, RAMP_STORE_TOTAL, cur);
        for (i = 0; i < RAMP_SEQ_OFFSET; i++)
            if (cur[i] != buf[i])
                break;
        if (i == RAMP_SEQ_OFFSET)
            return;
        buf[RAMP_SEQ_OFFSET] = (u8)(cur[RAMP_SEQ_OFFSET] + 1);
        dst = (a == DEV_RAMP_FLASH) ? RAMP_STORE_ALT : DEV_RAMP_FLASH;
    }
    buf[RAMP_COMMIT_OFFSET] = 0xff;
    flash_erase(dst);
    for (i = 0; i < RAMP_COMMIT_OFFSET; i += DEV_FLASH_WRITE_MAX)   /* one page */
        flash_write(dst + i,
                    RAMP_COMMIT_OFFSET - i < DEV_FLASH_WRITE_MAX ? RAMP_COMMIT_OFFSET - i : DEV_FLASH_WRITE_MAX,
                    buf + i);
    flash_write(dst + RAMP_COMMIT_OFFSET, 1, &zero);   /* the copy is complete only now */
}

static void save_waypoint(u8 bank, u8 slot, u16 f, u16 c, u16 hold)
{
    u8 buf[RAMP_STORE_TOTAL];
    u8 *p;
    int i;

    store_load(buf);

    /* Waypoint 1 starts a new upload: clear the bank's later waypoints, so a
     * shorter ramp never inherits stages from a longer one saved before it.
     * (The app always uploads 1..n in order.) */
    if (slot == 0)
        for (i = 2 + bank * RAMP_NUM_SLOTS * RAMP_SLOT_SIZE + RAMP_SLOT_SIZE;
             i < 2 + (bank + 1) * RAMP_NUM_SLOTS * RAMP_SLOT_SIZE; i++)
            buf[i] = 0xff;

    p = buf + 2 + (bank * RAMP_NUM_SLOTS + slot) * RAMP_SLOT_SIZE;
    p[0] = (u8)f;    p[1] = (u8)(f >> 8);
    p[2] = (u8)c;    p[3] = (u8)(c >> 8);
    p[4] = (u8)hold; p[5] = (u8)(hold >> 8);

    store_commit(buf);
}

/* One byte of the current store, or 0xFF (erased) if there is none --
 * whatever else might be in the sectors is never read as a setting. */
static u8 store_byte(u8 off)
{
    u32 a = ramp_store_addr();
    u8 b;
    if (!a)
        return 0xff;
    flash_read(a + off, 1, &b);
    return b;
}

u8 ramp_enabled(void)
{
    return store_byte(RAMP_ENABLED_OFFSET) != 0;
}

u8 ramp_store_stock_mode(void)
{
    return store_byte(RAMP_MODE_OFFSET) == RAMP_MODE_STOCK;
}

u8 ramp_stock_mode(void)
{
    volatile ramp_state_t *st = RAMP_STATE;
    if (st->magic == RAMP_MAGIC)
        return st->stock_mode;
    return ramp_store_stock_mode();   /* before the first tick sets the cache */
}

/* Sets the mode (from the app, or the Carta 2's boot gesture). Callers make
 * sure no ramp is running. */
void ramp_set_stock_mode(u8 stock)
{
    volatile ramp_state_t *st = RAMP_STATE;
    ramp_store_set(RAMP_MODE_OFFSET, stock ? RAMP_MODE_STOCK : 0xff);
    if (st->magic == RAMP_MAGIC) {
        st->stock_mode = ramp_store_stock_mode();   /* what flash now holds */
        /* A session already running belongs to stock until it ends: it
         * never turns into a ramp halfway (cleared at its end). */
        if (STRUCT_BASE[OFF_SESSION] != 0)
            st->arm_failed = 1;
        ramp_announce_queue(st);                    /* tell the app at once */
    }
}

void ramp_marker_dispatch(u8 marker, u8 byte14)
{
    volatile ramp_state_t *st = RAMP_STATE;

    if (ramp_active(st))
        return;   /* never rewrite the store under a running ramp */

    /* The mode switch works in either mode, ramps on or off. */
    if (marker == RAMP_MODE_MARKER) {
        if (byte14 == RAMP_MODE_STOCK || byte14 == RAMP_MODE_RAMP)
            ramp_set_stock_mode(byte14 == RAMP_MODE_STOCK);
        return;
    }
    if (ramp_stock_mode())
        return;   /* stock mode: nothing but the switch above */
    if (!ramp_enabled())
        return;   /* disabled: no flash write of any kind, full stop */

    if (marker == RAMP_START_MARKER) {
        if (byte14 == RAMP_START_REQUEST) {   /* see ramp.h: no flash involved */
            st->start_req = 1;
            st->start_req_t0 = DEV_SYS_TICK;
        }
        return;
    }

    if (marker == RAMP_OFFSET_MARKER) {
        int v = (signed char)byte14;
        if (v >= RAMP_OFS_MIN_F && v <= RAMP_OFS_MAX_F) {
            ramp_store_set(RAMP_OFS_OFFSET, (u8)(byte14 ^ 0x80));   /* see ramp_offset */
            if (st->magic == RAMP_MAGIC)
                ramp_announce_queue(st);   /* tell the app what's stored now */
        }
        return;
    }

    if (marker == RAMP_SELECT_MARKER) {
        if (byte14 < DEV_PICK_COUNT) {
            ramp_store_set(RAMP_SEL_OFFSET, byte14);
            if (st->magic == RAMP_MAGIC) {
                st->picker_sel = byte14;   /* an open picker (Aeris / Sport) follows it */
                st->picker_dirty = 0;
                ramp_announce_queue(st);
            }
        }
        return;
    }

    if (marker >= 0xb1 && marker <= 0xb5)
        save_waypoint(0, marker - 0xb1, *PRESET(TBL_FL_F, 0), *PRESET(TBL_FL_C, 0),
                      *PRESET(TBL_FL_HOLD, 0));
    else if (marker >= 0xb6 && marker <= 0xba)
        save_waypoint(1, marker - 0xb6, *PRESET(TBL_CO_F, 0), *PRESET(TBL_CO_C, 0),
                      *PRESET(TBL_CO_HOLD, 0));
}

/* Sets one byte of the store via the same read-whole-sector / erase /
 * write-back cycle save_waypoint() uses, so it can never land on a half state. */
void ramp_store_set(u8 off, u8 v)
{
    u8 buf[RAMP_STORE_TOTAL];
    store_load(buf);
    buf[off] = v;
    store_commit(buf);
}

void ramp_toggle_enabled(void)
{
    ramp_store_set(RAMP_ENABLED_OFFSET, ramp_enabled() ? 0 : 0xff);
}

u8 ramp_selected(void)
{
    u8 b = store_byte(RAMP_SEL_OFFSET);
    return (b < DEV_PICK_COUNT) ? b : RAMP_DEFAULT_PRESET;
}

/* Stored with the top bit flipped: -1 is 0xFF as a raw byte, the same as
 * erased flash. Flipped, every offset in range has its own byte, and erased
 * (0xFF) decodes to 127 -- out of range, so "never set" reads as 0. */
int ramp_offset(void)
{
    int v = (signed char)(store_byte(RAMP_OFS_OFFSET) ^ 0x80);
    return (v < RAMP_OFS_MIN_F || v > RAMP_OFS_MAX_F) ? 0 : v;
}
