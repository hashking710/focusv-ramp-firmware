/* ramp_tick.c -- Aeris
 *
 * Injected ramp-sequencer for the Aeris firmware. A genuinely different
 * compiled binary from the Carta 2 patch in ../ramp_tick.c -- same chip
 * family and wire protocol, different code layout, different struct
 * offsets, no screen (so no display/graph component here; see
 * ramp_led.c instead).
 *
 * Confirmed facts this relies on, all independently re-derived this
 * session directly against the real Aeris PROD-111224 binary (not
 * carried over from Carta 2, and not assumed from early research-pass
 * notes without re-checking):
 *
 *   - Central session/PID struct at fixed RAM address 0x8430e4 --
 *     confirmed as the struct both the 0xCC BLE handler (FUN_0000b3bc)
 *     and the per-tick orchestrator (FUN_00008154) share, via direct
 *     literal-pool resolution against the real binary (both functions
 *     load the identical pointer value).
 *   - struct+0x0  = session-active flag -- confirmed via FUN_00008154's
 *     own first check ("if (*pcVar6 == 0) { ...reset session state... }").
 *   - struct+0x6  = mode flag. Unlike Carta 2 (0=flower, 1=concentrate),
 *     Aeris uses 1=flower, 2=concentrate -- confirmed via both the 0xCC
 *     handler and the tick function branching identically on this field
 *     with literal values 1 and 2 (never 0).
 *   - struct+0x2a/0x2b = live MEASURED temperature. Confirmed: written by
 *     FUN_00008154 every tick from a live ADC-derived computation
 *     (FUN_000001b4(ntc_raw*1000, calibration_divisor)), both operands
 *     read fresh from hardware-backed pointers each call, not constants.
 *   - struct+0x2c/0x2d = live PID target, flower. struct+0x2e/0x2f = same,
 *     concentrate. Confirmed via FUN_00007f14 (the "temperature reached"
 *     detector, independently already identified in prior research as
 *     ±5°F/±2°C tolerance logic) comparing this directly against
 *     +0x2a/0x2b -- and confirmed to be in the SAME real-Fahrenheit unit
 *     as the factory default preset table (FUN_00004146 seeds literal
 *     defaults of 300/350/370/390/410 -- unmistakably real °F, not a
 *     scaled/calibration unit), which FUN_00008154's own one-time init
 *     block copies byte-for-byte (no conversion math) into this same
 *     field for a preset-selected session. The exact code path that
 *     populates it for a *custom-value* (non-preset) session was not
 *     pinned down to the single instruction despite extensive tracing
 *     (see firmware-analysis-notes.md for the full trail: calibration
 *     glide functions, rank-dispatch jump tables, and several dead ends
 *     were ruled out along the way) -- but the field identity and unit
 *     are confirmed through multiple converging, independent facts, not
 *     a single fragile assumption. This patch writes it directly, the
 *     same way it writes the custom-value field below, rather than
 *     trying to replicate whatever stock trigger path normally reaches it.
 *   - struct+0x30/0x31 = custom-value temperature, flower, real °F
 *     (confirmed: the 0xCC handler converts the incoming packet's raw
 *     value via `raw*9/5+32`, i.e. C->F, and stores the result here).
 *     struct+0x48/0x49 = same, concentrate.
 *   - struct+0x60/0x61 = session duration, flower, raw packet value, no
 *     conversion. struct+0x6c/0x6d = same, concentrate.
 *   - The wire packet format (the 0xCC/SET_TEMP command) is BYTE-FOR-BYTE
 *     IDENTICAL to the Carta 2 packet -- confirmed by matching every
 *     field's byte offset (flower temp at packet+2, duration at
 *     packet+7/8, marker at packet+13, etc.) against the already-
 *     confirmed Carta 2 layout. This means the existing app-side
 *     buildTempCommand/buildRampWaypointSaveCommand functions work for
 *     Aeris completely unchanged -- only the firmware side differs.
 *   - FUN_000001ac(value, divisor) at 0x1ac = the same confirmed ROM
 *     integer-division helper used throughout this whole project.
 *   - Per-tick orchestrator: FUN_00008154. Its real call site is 0x6464,
 *     inside the main scheduler superloop (confirmed: the loop calls
 *     the already-known button/event dispatcher at 0x4ee8 immediately
 *     before it) -- found via the same exhaustive single-match scan used
 *     for the Carta 2 patch (every possible 4-byte-aligned position in
 *     the entire 80,684-byte firmware checked for the exact `tjl 0x8154`
 *     encoding; exactly one match, at 0x6464).
 *
 * What's NOT independently flash-verified, and can't be from this dump
 * alone: the free flash region this patch's own code and waypoint data
 * live in. The firmware image is 80,684 bytes (0x13B2C); the next
 * confirmed structure is the OTA staging bank at 0x20000 (same "clear
 * space ahead of the OTA bank" pattern already established for Carta 2's
 * 0x30000, and literal references to 0x20000 do exist in this binary,
 * consistent with an OTA-erase routine) -- but the dumped firmware file
 * genuinely does not contain any bytes beyond its own 80,684-byte length,
 * so there's nothing in this dump to check the ~0x14000-0x1FFFF gap
 * against. Confirming it's truly unused flash needs a real device.
 */

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;

#define STRUCT_BASE           ((volatile u8 *)0x8430e4)
#define SESSION_ACTIVE_OFF    0x0
#define MODE_FLAG_OFF         0x6    /* 1 = flower, 2 = concentrate */
#define TARGET_FLOWER_OFF     0x2c
#define TARGET_CONC_OFF       0x2e
#define CUSTOM_FLOWER_OFF     0x30
#define CUSTOM_CONC_OFF       0x48
#define DUR_FLOWER_OFF        0x60
#define DUR_CONC_OFF          0x6c
#define MEASURED_F_OFF        0x2a

/* Dedicated flash region for this patch's own code + waypoint storage --
 * see the file header for why this specific address can't be verified
 * against the dump the way every other address in this file can be. */
#define RAMP_FLASH_SECTOR ((volatile u8 *)0x15000)
#define RAMP_NUM_SLOTS    5
#define RAMP_SLOT_SIZE    4

typedef short (*rom_div_fn)(int, int);
#define rom_div ((rom_div_fn)0x1ac)

/* Per-tick orchestrator, confirmed at 0x8154, called from the real
 * (Ghidra-auto-analysis-invisible, but exhaustively confirmed) call site
 * 0x6464. */
typedef void (*orig_fn)(void);
#define orig_pid_tick ((orig_fn)0x8154)

/* Defined in ramp_led.c, part of the same injected blob. Takes whether a
 * ramp is currently active -- must not touch the LEDs at all otherwise. */
void ramp_led_update(u8 ramp_active);

#define RAMP_MAGIC 0xA5D1   /* distinct from the Carta 2 patch's magic --
                             * different device, different RAM, no reason
                             * to share a value that would only cause
                             * confusion if the two were ever compared */

typedef struct {
    u16 magic;
    u16 tick_counter;
    u8  waypoint_index;    /* 0 = not started yet this session */
} ramp_state_t;
#define RAMP_STATE ((volatile ramp_state_t *)0x848000)

#define SENTINEL_A_F  150
#define SENTINEL_B_F  160

static u8 sentinel_matched(u16 live_f)
{
    /* Unlike Carta 2 (whose custom-value field is Celsius, needing an
     * F->C conversion before comparing), Aeris's custom-value field is
     * already real Fahrenheit -- compare directly, no rom_div needed. */
    return (live_f == SENTINEL_A_F) || (live_f == SENTINEL_B_F);
}

static u16 slot_temp_f(u8 slot)
{
    return *(volatile u16 *)(RAMP_FLASH_SECTOR + slot * RAMP_SLOT_SIZE);
}

static u16 slot_duration(u8 slot)
{
    return *(volatile u16 *)(RAMP_FLASH_SECTOR + slot * RAMP_SLOT_SIZE + 2);
}

static void apply_target(u8 mode, u16 target_f)
{
    if (mode == 1) {
        *(volatile u16 *)(STRUCT_BASE + TARGET_FLOWER_OFF) = target_f;
        *(volatile u16 *)(STRUCT_BASE + CUSTOM_FLOWER_OFF) = target_f;
    } else {
        *(volatile u16 *)(STRUCT_BASE + TARGET_CONC_OFF) = target_f;
        *(volatile u16 *)(STRUCT_BASE + CUSTOM_CONC_OFF) = target_f;
    }
}

static void ramp_tick(void)
{
    volatile ramp_state_t *st = RAMP_STATE;
    u8 mode;
    u16 live_f;

    if (st->magic != RAMP_MAGIC) {
        st->magic = RAMP_MAGIC;
        st->tick_counter = 0;
        st->waypoint_index = 0;
    }

    if (STRUCT_BASE[SESSION_ACTIVE_OFF] == 0) {
        st->waypoint_index = 0;
        st->tick_counter = 0;
        return;
    }

    mode = STRUCT_BASE[MODE_FLAG_OFF];

    if (st->waypoint_index == 0 && st->tick_counter == 0) {
        live_f = (mode == 1)
            ? *(volatile u16 *)(STRUCT_BASE + CUSTOM_FLOWER_OFF)
            : *(volatile u16 *)(STRUCT_BASE + CUSTOM_CONC_OFF);
        if (!sentinel_matched(live_f))
            return;
        st->waypoint_index = 1;
        apply_target(mode, slot_temp_f(0));
        return;
    }

    if (st->waypoint_index == 0 || st->waypoint_index > RAMP_NUM_SLOTS)
        return;

    {
        u16 hold_ticks = slot_duration(st->waypoint_index - 1);
        if (hold_ticks == 0 || hold_ticks == 0xffff)
            return;

        st->tick_counter++;
        if (st->tick_counter < hold_ticks)
            return;

        st->tick_counter = 0;
        st->waypoint_index++;
        if (st->waypoint_index > RAMP_NUM_SLOTS)
            return;

        apply_target(mode, slot_temp_f(st->waypoint_index - 1));
    }
}

/* Installed at the patched call site (0x6464) in place of the direct call
 * to FUN_00008154: runs the original tick unmodified, then the ramp
 * sequencer, then the LED progress indicator. */
void ramp_trampoline(void)
{
    orig_pid_tick();
    ramp_tick();
    ramp_led_update(RAMP_STATE->waypoint_index != 0 && RAMP_STATE->waypoint_index <= RAMP_NUM_SLOTS);
}
