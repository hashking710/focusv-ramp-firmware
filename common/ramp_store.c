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

static void save_waypoint(u8 bank, u8 slot, u16 f, u16 c, u16 hold)
{
    u8 buf[RAMP_STORE_SIZE];
    u8 *p;
    int i;

    /* NOR flash: read the whole store, erase the sector, write it back */
    flash_read(DEV_RAMP_FLASH, RAMP_STORE_SIZE, buf);
    if (buf[0] != (u8)RAMP_STORE_MAGIC || buf[1] != (u8)(RAMP_STORE_MAGIC >> 8)) {
        for (i = 0; i < RAMP_STORE_SIZE; i++)   /* first save, or an older layout */
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
    flash_write(DEV_RAMP_FLASH, RAMP_STORE_SIZE, buf);
}

void ramp_marker_dispatch(u8 marker)
{
    if (ramp_active(RAMP_STATE))
        return;   /* never rewrite the store under a running ramp */

    if (marker >= 0xb1 && marker <= 0xb5)
        save_waypoint(0, marker - 0xb1, *PRESET(TBL_FL_F, 0), *PRESET(TBL_FL_C, 0),
                      *PRESET(TBL_FL_HOLD, 0));
    else if (marker >= 0xb6 && marker <= 0xba)
        save_waypoint(1, marker - 0xb6, *PRESET(TBL_CO_F, 0), *PRESET(TBL_CO_C, 0),
                      *PRESET(TBL_CO_HOLD, 0));
}
