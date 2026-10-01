/* ramp_tick.c -- Carta Sport
 *
 * Injected ramp-sequencer for the Carta Sport firmware. A genuinely
 * different compiled binary from both the Carta 2 and Aeris patches --
 * same chip family and wire protocol, different code layout, different
 * addresses (even though Sport's source is clearly near-identical to
 * Aeris's at the C level -- see below). No screen, LED progress only
 * (5 individually-addressable RGB LEDs here, vs Aeris's 4 -- see
 * ramp_led.c).
 *
 * Confirmed facts this relies on, all independently re-derived this
 * session directly against the real Sport binary -- NOT copy-pasted from
 * the Aeris patch despite the near-identical source structure, and with
 * one real mistake caught and avoided in the process: Sport's divide
 * helper lives at a completely different address than both other
 * devices' (0x1529c, not 0x1ac) -- confirmed absent at 0x1ac before
 * writing any code that might have assumed otherwise.
 *
 *   - Central struct at 0x8426ec -- confirmed as the struct shared by
 *     both the 0xCC BLE handler and the per-tick orchestrator, via
 *     direct literal-pool resolution against the real binary (same
 *     cross-check method used for Aeris).
 *   - struct+0x0 = session-active flag (confirmed via the tick
 *     function's own first check, same pattern as Aeris).
 *   - struct+0x6 = mode flag, 1=flower, 2=concentrate (same convention
 *     as Aeris, confirmed independently against this binary).
 *   - struct+0x2a/0x2b = live measured temperature (confirmed: written
 *     by the tick function every tick from a live ADC-derived
 *     computation).
 *   - struct+0x14/0x15 = live PID target, flower. struct+0x16/0x17 =
 *     same, concentrate. Confirmed via the exact same "temperature
 *     reached"-style comparison against the measured field as both
 *     other devices use -- same reasoning as Aeris's analogous fields
 *     applies here (see ../aeris/ramp_tick.c's header for the full
 *     "reasoned from converging facts, not one traced instruction"
 *     story; the same situation holds here, not independently re-proven
 *     to the same single-instruction standard but resting on the same
 *     kind of converging evidence).
 *   - struct+0x30/0x31 = custom-value temperature, flower, real
 *     Fahrenheit when the app sends Fahrenheit-scale packets (confirmed:
 *     the 0xCC handler's Fahrenheit-scale branch stores the packet's
 *     raw value here unconverted). struct+0x48/0x49 = same, concentrate.
 *     This patch relies on the app always sending Fahrenheit-scale
 *     packets for ramp traffic (confirmed true in terpline-web's
 *     buildRampStartCommand/buildRampWaypointSaveCommand, which both
 *     hardcode it) -- the same reasoning Aeris's patch already
 *     documents and relies on.
 *   - struct+0x60/0x61 = session duration, flower, raw packet value, no
 *     conversion. struct+0x6c/0x6d = same, concentrate.
 *   - The wire packet format is byte-for-byte identical to Carta 2 and
 *     Aeris -- confirmed by matching every field's byte offset. No
 *     app-side changes needed for Sport either.
 *   - The 0xCC handler has the SAME dual-path structure already found
 *     (and verified to reconverge correctly) in both other devices: a
 *     scale-byte check branches to a separate Fahrenheit-specific code
 *     block, which does its own field writes then tail-jumps back into
 *     the shared body -- confirmed via direct disassembly that this
 *     still flows into the same marker-dispatch chain this patch
 *     intercepts, regardless of which branch was taken.
 *   - FUN_0001529c(value, divisor) at 0x1529c = Sport's own divide
 *     helper (quotient) -- NOT the same address as Carta 2/Aeris's
 *     0x1ac, which is confirmed absent as a function in this binary.
 *     Checked directly before writing this file specifically to avoid
 *     repeating the exact class of mistake this whole project has
 *     already caught and fixed more than once.
 *   - Per-tick orchestrator: the tick function (body starting 0x7c00).
 *     Its real call site is 0x58b0, inside the main scheduler superloop
 *     -- found via the same exhaustive single-match scan used for both
 *     other devices (every possible 4-byte-aligned position in the
 *     entire 90,860-byte firmware checked for the exact `tjl 0x7c00`
 *     encoding; exactly one match).
 *
 * What's NOT independently flash-verified, same structural limitation
 * as Aeris: the free flash region this patch's own code and waypoint
 * data live in. Firmware image is 90,860 bytes (0x162EC); the OTA
 * staging bank at 0x20000 is confirmed directly for Sport (a real
 * erase-loop call targeting it was found and decompiled, unlike Aeris
 * where this was only inferred) -- but the dump still contains nothing
 * past the real image's own length, so the ~0x163000-0x1FFFF gap's
 * actual flash contents can't be checked from this dump alone.
 */

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;

#define STRUCT_BASE           ((volatile u8 *)0x8426ec)
#define SESSION_ACTIVE_OFF    0x0
#define MODE_FLAG_OFF         0x6    /* 1 = flower, 2 = concentrate */
#define TARGET_FLOWER_OFF     0x14
#define TARGET_CONC_OFF       0x16
#define CUSTOM_FLOWER_OFF     0x30
#define CUSTOM_CONC_OFF       0x48
#define DUR_FLOWER_OFF        0x60
#define DUR_CONC_OFF          0x6c
#define MEASURED_F_OFF        0x2a

/* Dedicated flash region for this patch's own code + waypoint storage --
 * see the file header for why this specific address can't be verified
 * against the dump the way every other address in this file can be. */
#define RAMP_FLASH_SECTOR ((volatile u8 *)0x18000)
#define RAMP_NUM_SLOTS    5
#define RAMP_SLOT_SIZE    4

typedef short (*rom_div_fn)(int, int);
#define rom_div ((rom_div_fn)0x1529c)   /* Sport-specific -- NOT 0x1ac */

/* Per-tick orchestrator, confirmed body starting at 0x7c00, called from
 * the real (Ghidra-auto-analysis-invisible, but exhaustively confirmed)
 * call site 0x58b0. */
typedef void (*orig_fn)(void);
#define orig_pid_tick ((orig_fn)0x7c00)

#define RAMP_MAGIC 0xA5E2   /* distinct from both other devices' magic
                             * values -- different device, different RAM,
                             * no reason to share a value */

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
    /* Custom-value field is already real Fahrenheit when the app sends
     * Fahrenheit-scale packets (always true for ramp traffic) -- compare
     * directly, no conversion needed. Same approach as the Aeris patch. */
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

/* Defined in ramp_led.c, part of the same injected blob. Takes whether a
 * ramp is currently active -- must not touch the LEDs at all otherwise. */
void ramp_led_update(u8 ramp_active);

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

/* Installed at the patched call site (0x58b0) in place of the direct call
 * to the stock tick function: runs the original tick unmodified, then the
 * ramp sequencer, then the LED progress indicator. */
void ramp_trampoline(void)
{
    orig_pid_tick();
    ramp_tick();
    ramp_led_update(RAMP_STATE->waypoint_index != 0 && RAMP_STATE->waypoint_index <= RAMP_NUM_SLOTS);
}
