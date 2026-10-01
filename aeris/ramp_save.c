/* ramp_save.c -- Aeris
 *
 * Custom-ramp waypoint storage, saved via the real app's own custom-
 * temperature entry flow -- same approach as the Carta 2 patch, adapted
 * to Aeris's confirmed addresses.
 *
 * Confirmed facts this relies on (see ramp_tick.c's header for the struct
 * offsets, which this file shares):
 *
 *   - The real 0xCC handler is FUN_0000b3bc, and by the time it reaches
 *     its marker-dispatch chain, has already parsed and written the
 *     packet's temperature (real Fahrenheit, converted) into struct+0x30
 *     (flower) / struct+0x48 (concentrate), and its duration into
 *     struct+0x60 (flower) / struct+0x6c (concentrate) -- directly
 *     decompiled and confirmed this session, not assumed from the Carta 2
 *     patch's analogous fields.
 *   - The marker-dispatch chain lives at 0xb490-0xb4a9 (inside what
 *     Ghidra's current auto-analysis calls FUN_0000b3bc, continuing
 *     after it): loads the marker byte from packet+0x35 (r7+0x35, where
 *     r7 is the incoming packet buffer -- same register role as Carta 2's
 *     r5, confirmed via direct disassembly), then three back-to-back
 *     compare/branch pairs against 0xA5 (start), 0xAF (stop), 0x66
 *     (test-fire). None of 0xB1-0xB5 match any of the three, so those
 *     markers fall straight through this chain untouched on stock
 *     firmware -- same "harmless no-op" property as the Carta 2 patch's
 *     new markers.
 *   - Patching any single leaf of that chain (e.g. just the 0x66 case)
 *     would mean the new markers never get seen, for the identical reason
 *     the Carta 2 patch's first draft was wrong about this: a marker that
 *     doesn't match any of the three existing comparisons never reaches
 *     any individual leaf. The fix here is the same one used there:
 *     intercept the marker *load* (0xb490-0xb494, a 4-byte
 *     "tmovs r3,#0x35 / tloadrb r3,[r7,r3]" pair), not any leaf -- see
 *     ramp_marker_entry.s.
 *   - Flash primitives, independently confirmed for THIS build (the Carta
 *     2 patch's addresses for these do NOT apply here -- checked directly
 *     and confirmed absent, before writing this file, specifically to
 *     avoid repeating the exact mistake this project already caught and
 *     fixed once): FUN_00000ab8(addr,len,buf) = read, byte-by-byte from
 *     flash into buf with SPI-controller handshaking; FUN_00000a1c(addr)
 *     = erase; FUN_00000a5c(addr,len,buf) = write, byte-by-byte from buf
 *     into flash. All three decompiled directly and confirmed by
 *     behavior (handshake-loop + copy-loop shape) and call-site usage
 *     elsewhere in this same binary (the factory-defaults and BLE-sync
 *     routines use exactly these for the device's own flash records).
 *     No separate "write-verify-with-retry" wrapper was found the way
 *     Carta 2 has one (FUN_00019308) -- this file calls the plain write
 *     primitive directly.
 */

typedef unsigned char  u8;
typedef unsigned short u16;

#define STRUCT_BASE          ((volatile u8 *)0x8430e4)
#define MODE_FLAG_OFF        0x6
#define CUSTOM_FLOWER_OFF    0x30
#define CUSTOM_CONC_OFF      0x48
#define DUR_FLOWER_OFF       0x60
#define DUR_CONC_OFF         0x6c

#define RAMP_FLASH_SECTOR 0x15000
#define RAMP_NUM_SLOTS    5
#define RAMP_SLOT_SIZE    4

/* Confirmed directly against this build -- see header. */
typedef void (*flash_read_fn)(int addr, int len, void *buf);
#define flash_read ((flash_read_fn)0xab8)

typedef void (*flash_erase_fn)(int addr);
#define flash_erase ((flash_erase_fn)0xa1c)

typedef void (*flash_write_fn)(int addr, int len, void *buf);
#define flash_write ((flash_write_fn)0xa5c)

static void save_ramp_waypoint(u8 slot, u16 temp_f, u16 duration)
{
    u8 buf[RAMP_NUM_SLOTS * RAMP_SLOT_SIZE];

    if (slot >= RAMP_NUM_SLOTS)
        return;

    flash_read(RAMP_FLASH_SECTOR, sizeof(buf), buf);
    flash_erase(RAMP_FLASH_SECTOR);

    buf[slot * RAMP_SLOT_SIZE + 0] = (u8)temp_f;
    buf[slot * RAMP_SLOT_SIZE + 1] = (u8)(temp_f >> 8);
    buf[slot * RAMP_SLOT_SIZE + 2] = (u8)duration;
    buf[slot * RAMP_SLOT_SIZE + 3] = (u8)(duration >> 8);

    flash_write(RAMP_FLASH_SECTOR, sizeof(buf), buf);
}

/* Called from ramp_marker_entry (marker_entry.s) with the marker byte in
 * r0, for every packet that reaches the marker-dispatch point. Markers
 * 0xA5/0xAF/0x66 (and anything else) are left completely alone -- the
 * untouched stock chain immediately after the call site handles those
 * exactly as it always did, since the caller restores r3 = marker byte
 * before returning there regardless of what happens here. */
void ramp_marker_dispatch(u8 marker)
{
    if (marker >= 0xb1 && marker <= 0xb5) {
        u8 mode = STRUCT_BASE[MODE_FLAG_OFF];
        u16 temp_f = (mode == 1)
            ? *(volatile u16 *)(STRUCT_BASE + CUSTOM_FLOWER_OFF)
            : *(volatile u16 *)(STRUCT_BASE + CUSTOM_CONC_OFF);
        u16 duration = (mode == 1)
            ? *(volatile u16 *)(STRUCT_BASE + DUR_FLOWER_OFF)
            : *(volatile u16 *)(STRUCT_BASE + DUR_CONC_OFF);
        save_ramp_waypoint((u8)(marker - 0xb1), temp_f, duration);
    }
}
