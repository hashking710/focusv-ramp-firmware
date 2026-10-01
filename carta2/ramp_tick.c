/* ramp_tick.c
 *
 * Injected ramp-sequencer for the Quantum/Carta-2 firmware.
 * Confirmed facts this relies on (see firmware-analysis-notes.md):
 *   - central session/PID struct lives at fixed RAM address 0x843028
 *   - struct+0x1   = "session active/initialized" flag -- re-confirmed this
 *                    session via direct decompile of FUN_0000af2c (the real
 *                    orchestrator, see below), the first thing it checks
 *   - struct+0x7   = flower/concentrate mode flag (0=flower, 1=concentrate).
 *                    Struct field offsets are far more stable across
 *                    firmware builds than function addresses (the data
 *                    layout doesn't get reorganized just because the code
 *                    around it got recompiled), and this offset shows
 *                    heavy, consistent [r4,#7]-style usage throughout the
 *                    real PROD-111224 disassembly (confirmed directly, not
 *                    just carried over) -- but unlike struct+0x1 above, it
 *                    wasn't re-confirmed against one specific named function
 *                    this session (the function that originally anchored
 *                    this claim, FUN_0000a614, turned out to be another
 *                    stale address -- not a function in this build either).
 *                    Treat this one specific offset as well-corroborated
 *                    but not independently re-proven the same rigorous way
 *                    as everything else in this file.
 *   - struct+0x20/0x21 = live PID target temperature, Celsius, flower
 *   - struct+0x22/0x23 = live PID target temperature, Celsius, concentrate
 *   - struct+0x36/0x37 = flower "custom value" field the stock 0xCC BLE
 *     handler writes (Celsius) for a non-preset temperature entry
 *   - struct+0x4e/0x4f = same, concentrate side
 *   - FUN_000001ac(value, divisor) at 0x1ac = ROM integer-division helper;
 *     F->C conversion is FUN_000001ac(F*5-160, 9)
 *   - FUN_0000af2c at 0xaf2c = the existing per-tick PID orchestrator,
 *     called at call site 0x6e2e (the containing caller function is outside
 *     Ghidra's auto-analysis function boundaries in this build -- a known
 *     TC32-module quirk, see methodology.md -- so it has no FUN_ name, but
 *     the call site itself is independently, exhaustively confirmed: an
 *     authoritative toolchain scan checked every possible 4-byte-aligned
 *     position in the entire 143,564-byte firmware for a `tjl 0xaf2c`
 *     encoding and found exactly one match, at 0x6e2e -- not inferred or
 *     guessed, the literal instruction bytes there. (An earlier version of
 *     this file had this same call site correctly, but called a different,
 *     stale address at it -- 0xad4c, carried over from an older firmware
 *     build where the orchestrator lived at that address. 0xad4c is not a
 *     function at all in this build; see ramp_save.c's header for the
 *     parallel story with the marker-dispatch patch.)
 *
 * Scratch RAM: 0x848000, TLSR8258 non-retention 32KB SRAM bank -- fine
 * since ramp state only needs to persist during an active session, but
 * NOT guaranteed zero at cold boot; see the magic-value init check.
 *
 * Waypoints: read directly from the dedicated flash sector at 0x31000
 * (see ramp_save.c), written via the marker-byte extension to the real
 * 0xCC handler (markers 0xB1-0xB5) -- real, dedicated, BLE-controlled
 * storage, kept completely separate from Session Presets. Flash on this
 * chip is memory-mapped for reads (the whole firmware itself executes
 * directly from flash addresses), so waypoints are read as plain
 * pointer dereferences here, same as any other confirmed field --
 * only writing flash needs the dedicated primitives (see ramp_save.c).
 *
 * Trigger: entering one of two sentinel temperatures (150F/160F, both
 * well under any real preset range) as a custom temperature and hitting
 * start arms the ramp -- unchanged from the earlier version.
 *
 * Display graph (new): while a ramp is running, the live-heating screen's
 * normal number+gauge is replaced with a graph of target-vs-measured
 * temperature over the ramp's run so far -- see ramp_display.c. This file
 * owns the trace buffer this needs:
 *   - struct+0x1c/0x1d = live MEASURED temperature, Fahrenheit -- confirmed
 *     via FUN_00008b08 (the RTD/Callendar-Van Dusen conversion routine),
 *     which writes its result straight into this field on the same
 *     central struct. (struct+0x1e/0x1f is the Celsius twin, unused here.)
 *   - the graph's X axis needs the ramp's total planned duration, known
 *     once armed (sum of the saved waypoints' hold durations) -- computed
 *     once when the ramp starts, stored as trace_stride (ticks per graph
 *     column) so the trace buffer always exactly spans the ramp's real
 *     length regardless of how long that turns out to be.
 */

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;

#define STRUCT_BASE          ((volatile u8 *)0x843028)
#define SESSION_ACTIVE_OFF   0x1
#define MODE_FLAG_OFF        0x7
#define TARGET_FLOWER_OFF    0x20
#define TARGET_CONC_OFF      0x22
#define CUSTOM_FLOWER_OFF    0x36
#define CUSTOM_CONC_OFF      0x4e
#define MEASURED_F_OFF       0x1c   /* confirmed via FUN_00008b08, this session */

#define RAMP_FLASH_SECTOR ((volatile u8 *)0x31000)
#define RAMP_NUM_SLOTS    5
#define RAMP_SLOT_SIZE    4

/* ROM integer-division helper, confirmed at firmware address 0x1ac. */
typedef short (*rom_div_fn)(int, int);
#define rom_div ((rom_div_fn)0x1ac)

/* Existing per-tick PID orchestrator we're wrapping, confirmed at 0xaf2c
 * (exhaustive single-match toolchain scan, see file header). */
typedef void (*orig_fn)(void);
#define orig_pid_tick ((orig_fn)0xaf2c)

#define RAMP_MAGIC 0xA5C5   /* bumped from 0xA5C4: struct grew again (trace[]
                             * widened 150->220, see TRACE_LEN below) -- same
                             * reasoning as the previous bump: forces a clean
                             * re-init instead of reading stale garbage through
                             * the resized array on first boot after this
                             * patch replaces an older ramp build. */

/* Graph trace buffer: one byte per graph column, holding a quantized
 * measured-temperature sample. TRACE_LEN matches the plotted-area width
 * ramp_display.c actually draws (see its own header) -- keep the two in
 * sync if either changes. Widened from 150 to 220 once a display-side audit
 * found two screen elements (FUN_0000dcac, FUN_0000e300) that the original
 * 3-call-site patch never suppressed, silently drawing over part of the
 * graph during a ramp -- see ramp_display.c's header for the full story.
 * Fixing that freed enough extra screen width to make a wider trace buffer
 * worthwhile. Still nothing against the 32KB non-retention bank this whole
 * struct lives in. */
#define TRACE_LEN 220

typedef struct {
    u16 magic;
    u16 tick_counter;
    u8  waypoint_index;    /* 0 = not started yet this session */
    u8  trace_stride;      /* ticks per graph column; computed once on arm */
    u8  trace_tick_count;  /* ticks since the last column was sampled */
    u8  trace_col;         /* next column to write, 0..TRACE_LEN (clamped) */
    u8  graph_frame_drawn; /* ramp_display.c: 0 until axes+target line drawn
                             * this run -- mirrored here only so the arm
                             * logic below can reset it; never read in this
                             * file otherwise */
    u8  graph_drawn_col;   /* ramp_display.c: how much of the trace is
                             * already on screen */
    u8  graph_drawn_stage; /* ramp_display.c: last-painted stage digit */
    u8  trace[TRACE_LEN];  /* measured temp (raw °F, clamped to fit a byte
                             * via TRACE_TEMP_BIAS) per column, oldest-first */
} ramp_state_t;
#define RAMP_STATE ((volatile ramp_state_t *)0x848000)

/* Measured °F values this device ever produces comfortably fit a byte once
 * a fixed bias is subtracted (every real target/limit across all three
 * devices is under 640°F -- see DEVICE_LIMITS in the app-side code -- and
 * nothing here needs sub-degree precision). Stored as (tempF - BIAS),
 * clamped to 0-255; ramp_display.c reverses this to plot real degrees. */
#define TRACE_TEMP_BIAS 200

#define SENTINEL_A_F  150
#define SENTINEL_B_F  160

static u8 sentinel_matched(u16 live_c)
{
    u16 a_c = (u16)rom_div((int)SENTINEL_A_F * 5 - 160, 9);
    u16 b_c = (u16)rom_div((int)SENTINEL_B_F * 5 - 160, 9);
    return (live_c == a_c) || (live_c == b_c);
}

static void apply_target(u8 mode, u16 target_c)
{
    if (mode == 0) {
        *(volatile u16 *)(STRUCT_BASE + TARGET_FLOWER_OFF) = target_c;
    } else {
        *(volatile u16 *)(STRUCT_BASE + TARGET_CONC_OFF) = target_c;
    }
}

/* Waypoints are stored already-converted to Celsius (see ramp_save.c,
 * which pulls them from the same struct fields this file's own
 * sentinel-trigger check reads). 0xFFFF (erased/never-saved) and 0
 * (explicit end marker) both naturally stop the ramp -- no need to
 * distinguish them. */
static u16 slot_temp_c(u8 slot)
{
    return *(volatile u16 *)(RAMP_FLASH_SECTOR + slot * RAMP_SLOT_SIZE);
}

static u16 slot_duration(u8 slot)
{
    return *(volatile u16 *)(RAMP_FLASH_SECTOR + slot * RAMP_SLOT_SIZE + 2);
}

/* Sum of every valid waypoint's hold duration, stopping at the first
 * end-of-ramp marker (0 or 0xFFFF) -- same convention ramp_tick() itself
 * already uses to detect the end. Used once, at arm time, to size the
 * trace's tick-per-column stride so the graph always exactly spans
 * whatever this particular ramp's real planned length turns out to be. */
static u16 total_ramp_ticks(void)
{
    u8  i;
    u32 total = 0;

    for (i = 0; i < RAMP_NUM_SLOTS; i++) {
        u16 d = slot_duration(i);
        if (d == 0 || d == 0xffff)
            break;
        total += d;
    }
    if (total > 0xffff)
        total = 0xffff;
    return (u16)total;
}

/* Called once per tick for every tick the ramp is actively running
 * (including "still holding this waypoint" ticks, the common case) --
 * see the call site in ramp_tick() below for exactly when. Appends one
 * sample every trace_stride ticks; stops silently once the buffer fills,
 * which only happens if the ramp runs longer than planned (session
 * re-armed, device-side session-duration ceiling interactions, etc.) --
 * better to just stop recording than overrun the buffer. */
static void sample_trace(volatile ramp_state_t *st)
{
    u16 measured_f;

    if (st->trace_col >= TRACE_LEN)
        return;

    st->trace_tick_count++;
    if (st->trace_tick_count < st->trace_stride)
        return;
    st->trace_tick_count = 0;

    measured_f = *(volatile u16 *)(STRUCT_BASE + MEASURED_F_OFF);
    if (measured_f < TRACE_TEMP_BIAS) {
        st->trace[st->trace_col] = 0;
    } else if (measured_f - TRACE_TEMP_BIAS > 255) {
        st->trace[st->trace_col] = 255;
    } else {
        st->trace[st->trace_col] = (u8)(measured_f - TRACE_TEMP_BIAS);
    }
    st->trace_col++;
}

static void ramp_tick(void)
{
    volatile ramp_state_t *st = RAMP_STATE;
    u8 mode;
    u16 live_c;

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

    /* Sample every tick the ramp is actively running, regardless of which
     * branch below ends up returning -- "still holding this waypoint" is
     * the common case and must still record a point, not just the ticks
     * where something else also happens to change. */
    if (st->waypoint_index != 0 && st->waypoint_index <= RAMP_NUM_SLOTS) {
        sample_trace(st);
    }

    if (st->waypoint_index == 0 && st->tick_counter == 0) {
        live_c = (mode == 0)
            ? *(volatile u16 *)(STRUCT_BASE + CUSTOM_FLOWER_OFF)
            : *(volatile u16 *)(STRUCT_BASE + CUSTOM_CONC_OFF);
        if (!sentinel_matched(live_c))
            return;
        {
            u16 total_ticks = total_ramp_ticks();
            /* no hardware divide on this chip -- must go through the
             * confirmed ROM helper, not native '/' (the linker will
             * otherwise pull in a libgcc __udivsi3 that doesn't exist
             * in this freestanding build). */
            u16 stride = (u16)rom_div((int)total_ticks, TRACE_LEN);
            if (stride < 1)   stride = 1;
            if (stride > 255) stride = 255;
            st->trace_stride = (u8)stride;
        }
        st->trace_col = 0;
        st->trace_tick_count = 0;
        st->graph_frame_drawn = 0;
        st->graph_drawn_col = 0;
        st->graph_drawn_stage = 0;
        st->waypoint_index = 1;
        apply_target(mode, slot_temp_c(0));
        return;
    }

    if (st->waypoint_index > RAMP_NUM_SLOTS)
        return;

    {
        u16 hold_ticks = slot_duration(st->waypoint_index - 1);
        if (hold_ticks == 0 || hold_ticks == 0xffff)
            return;   /* end of ramp: explicit marker, or slot never saved */

        st->tick_counter++;
        if (st->tick_counter < hold_ticks)
            return;

        st->tick_counter = 0;
        st->waypoint_index++;
        if (st->waypoint_index > RAMP_NUM_SLOTS)
            return;

        apply_target(mode, slot_temp_c(st->waypoint_index - 1));
    }
}

/* Installed at the patched call site in place of the direct call to
 * FUN_0000ad4c: runs the original, unmodified PID tick first, then the
 * ramp logic. */
void ramp_trampoline(void)
{
    orig_pid_tick();
    ramp_tick();
}
