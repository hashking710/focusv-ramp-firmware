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
 * Waypoint 1 of a bank clears that bank's waypoints 2-5; a ramp is the saved
 * prefix up to the first empty (hold 0 / erased) waypoint.
 * The mode comes from the marker, not the device: the attached atomizer decides
 * the device's mode, and flower waypoints saved with a concentrate atomizer
 * attached must still be stored as flower.
 */
#include "ramp.h"

/* RAMP_STORE_TOTAL throughout, not RAMP_STORE_SIZE: this reads/writes the
 * enabled-flag byte too (see ramp.h), so a save never clobbers it back to
 * erased. A store written by a version of this patch before that flag
 * existed reads it as 0xFF regardless -- NOR flash leaves anything past
 * what was actually written at its erased value -- so this needs no
 * migration: an old store is simply read as "enabled", the existing default. */
static void save_waypoint(u8 bank, u8 slot, u16 f, u16 c, u16 hold)
{
    u8 buf[RAMP_STORE_TOTAL];
    u8 *p;
    int i;

    /* NOR flash: read the whole store, erase the sector, write it back */
    flash_read(DEV_RAMP_FLASH, RAMP_STORE_TOTAL, buf);
    if (buf[0] != (u8)RAMP_STORE_MAGIC || buf[1] != (u8)(RAMP_STORE_MAGIC >> 8)) {
        for (i = 0; i < RAMP_STORE_TOTAL; i++)   /* first save, or an older layout */
            buf[i] = 0xff;
        buf[0] = (u8)RAMP_STORE_MAGIC;
        buf[1] = (u8)(RAMP_STORE_MAGIC >> 8);
    }

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

    flash_erase(DEV_RAMP_FLASH);
    flash_write(DEV_RAMP_FLASH, RAMP_STORE_TOTAL, buf);
}

void ramp_marker_dispatch(u8 marker, u8 byte14)
{
    if (ramp_active(RAMP_STATE))
        return;   /* never rewrite the store under a running ramp */
    if (!ramp_enabled())
        return;   /* disabled: no flash write of any kind, full stop */

    if (marker == RAMP_OFFSET_MARKER) {
        int v = (signed char)byte14;
        if (v >= RAMP_OFS_MIN_F && v <= RAMP_OFS_MAX_F)
            ramp_store_set(RAMP_OFS_OFFSET, byte14);
        return;
    }

    if (marker >= 0xb1 && marker <= 0xb5)
        save_waypoint(0, marker - 0xb1, *PRESET(TBL_FL_F, 0), *PRESET(TBL_FL_C, 0),
                      *PRESET(TBL_FL_HOLD, 0));
    else if (marker >= 0xb6 && marker <= 0xba)
        save_waypoint(1, marker - 0xb6, *PRESET(TBL_CO_F, 0), *PRESET(TBL_CO_C, 0),
                      *PRESET(TBL_CO_HOLD, 0));
}

/* Flips the enabled byte via the exact same read-whole-sector / erase /
 * write-back cycle save_waypoint() uses, so it can never land on a half
 * state. Called only from a device's ramp_click_entry.s, on a quadruple
 * click of its single button -- see ramp_enabled() in ramp.h. */
void ramp_store_set(u8 off, u8 v)
{
    u8 buf[RAMP_STORE_TOTAL];
    int i;

    flash_read(DEV_RAMP_FLASH, RAMP_STORE_TOTAL, buf);
    if (buf[0] != (u8)RAMP_STORE_MAGIC || buf[1] != (u8)(RAMP_STORE_MAGIC >> 8)) {
        for (i = 0; i < RAMP_STORE_TOTAL; i++)
            buf[i] = 0xff;
        buf[0] = (u8)RAMP_STORE_MAGIC;
        buf[1] = (u8)(RAMP_STORE_MAGIC >> 8);
    }
    buf[off] = v;

    flash_erase(DEV_RAMP_FLASH);
    flash_write(DEV_RAMP_FLASH, RAMP_STORE_TOTAL, buf);
}

void ramp_toggle_enabled(void)
{
    ramp_store_set(RAMP_ENABLED_OFFSET, ramp_enabled() ? 0 : 0xff);
}

u8 ramp_selected(void)
{
    u8 b;
    flash_read(DEV_RAMP_FLASH + RAMP_SEL_OFFSET, 1, &b);
    return (b < DEV_PICK_COUNT) ? b : RAMP_DEFAULT_PRESET;
}

int ramp_offset(void)
{
    u8 b;
    int v;
    flash_read(DEV_RAMP_FLASH + RAMP_OFS_OFFSET, 1, &b);
    if (b == 0xff)
        return 0;
    v = (signed char)b;
    if (v < RAMP_OFS_MIN_F) v = RAMP_OFS_MIN_F;
    if (v > RAMP_OFS_MAX_F) v = RAMP_OFS_MAX_F;
    return v;
}

/* Called from ramp_click_entry.s every click in place of the two stock
 * instructions that increment and store the LED-preset counter (0-5) --
 * base/off together are that field's address, so this replicates them
 * exactly first. 1-3 then return having done nothing else -- LED preset
 * cycling still works normally. Landing on 4 is repurposed: instead of
 * selecting whichever LED preset 4 happens to be (the user doesn't want
 * that kept), it toggles the ramp system and resets the counter to 0,
 * matching "0 = LEDs off" so preset 4 is never actually applied. */
void ramp_click_dispatch(volatile u8 *base, u8 off)
{
    u8 count = (u8)(base[off] + 1);
    base[off] = count;
    if (count != 4)
        return;
    base[off] = 0;
    ramp_toggle_enabled();
}
