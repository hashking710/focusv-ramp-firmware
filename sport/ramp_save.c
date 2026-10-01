/* ramp_save.c -- Carta Sport
 *
 * Custom-ramp waypoint storage. Same approach as the Carta 2 and Aeris
 * patches, adapted to Sport's confirmed addresses.
 *
 * Confirmed facts this relies on (see ramp_tick.c's header for the
 * struct offsets, which this file shares):
 *
 *   - The marker-dispatch chain lives at 0xb002-0xb018 (the real
 *     0xCC handler itself has no Ghidra-recognized function boundary --
 *     same known gap as the other two devices -- but the instructions
 *     are confirmed real via direct disassembly): loads the marker byte
 *     from packet+0x35 (r7+0x35, same register role as both other
 *     devices), then three compare/branch pairs against 0xA5 (start),
 *     0xAF (stop), 0x66 (test-fire). None of 0xB1-0xB5 match any of the
 *     three, so those markers fall straight through on stock firmware --
 *     same harmless-no-op property as the other two patches.
 *   - Same architecture problem as the other two devices: patching a
 *     single leaf of that chain would mean the new markers are never
 *     seen. Fix is the same: intercept the marker *load*
 *     (0xb002-0xb006, a 4-byte "tmovs r3,#0x35 / tloadrb r3,[r7,r3]"
 *     pair), not any leaf -- see ramp_marker_entry.s.
 *   - Flash primitives, independently confirmed for this specific build
 *     (checked directly rather than assumed from either other device,
 *     both of which use different addresses again):
 *       - Write: entry at 0xc0e8, signature (addr, len, buf) -- a
 *         page-boundary-aware chunking wrapper around a lower-level SPI
 *         command primitive (opcode 0x02 = Page Program), confirmed via
 *         direct decompile.
 *       - Erase: FUN_0000c178(addr) -- confirmed via direct decompile,
 *         issues SPI opcode 0x20 (Sector Erase).
 *       - Read: no dedicated primitive needed or used -- flash is
 *         memory-mapped for reads on this chip family (same confirmed
 *         project-wide fact already established for Carta 2 and Aeris);
 *         waypoints are read back as plain pointer dereferences in
 *         ramp_tick.c, same as the other two patches.
 */

typedef unsigned char  u8;
typedef unsigned short u16;

#define STRUCT_BASE          ((volatile u8 *)0x8426ec)
#define MODE_FLAG_OFF        0x6
#define CUSTOM_FLOWER_OFF    0x30
#define CUSTOM_CONC_OFF      0x48
#define DUR_FLOWER_OFF       0x60
#define DUR_CONC_OFF         0x6c

#define RAMP_FLASH_SECTOR 0x18000
#define RAMP_NUM_SLOTS    5
#define RAMP_SLOT_SIZE    4

/* Confirmed directly against this build -- see header. Note the
 * argument order (addr, len, buf) matches Carta 2/Aeris's convention,
 * but the ADDRESSES do not -- checked fresh, not reused. */
typedef void (*flash_erase_fn)(int addr);
#define flash_erase ((flash_erase_fn)0xc178)

typedef void (*flash_write_fn)(int addr, int len, void *buf);
#define flash_write ((flash_write_fn)0xc0e8)

/* Flash is memory-mapped for reads -- a plain volatile read, same as
 * ramp_tick.c's waypoint reads. No function call needed. */
static void flash_read(int addr, int len, void *buf)
{
    u8 *src = (u8 *)(volatile u8 *)addr;
    u8 *dst = (u8 *)buf;
    int i;
    for (i = 0; i < len; i++) dst[i] = src[i];
}

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
