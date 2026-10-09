/* ramp.h -- shared core of the on-device ramp patch (Carta 2, Aeris, Carta Sport).
 *
 * The three devices run the same logic; they differ only in addresses, field
 * offsets and flag polarities, which each device supplies in its own device.h.
 * Every value in a device.h was confirmed by reading the stock code that
 * CONSUMES it (the PID step, the orchestrator, the session timer), not inferred
 * from where values come from -- the earlier per-device copies of this logic got
 * field meanings wrong on two devices exactly that way.
 *
 * HOW A RAMP RUNS -- by reusing stock mechanisms rather than fighting them:
 *
 *  Arming. Terpline sends the stock session start (marker 0xA5) with every
 *  current value echoed and a ramp request in byte 14 (stock never reads it);
 *  the session that start begins arms on its first tick, on whatever preset
 *  is active, and that slot is put back afterwards (saved_ok). A request with
 *  nothing to run stops the session. A preset slot holding the sentinel
 *  (150 F / 65 C) arms the same way, so a ramp also starts from the device's
 *  own buttons.
 *
 *  Target. Each stock orchestrator reloads the PID target pair (C and F) from
 *  the active preset slot every tick while the "reached" flag is 0, before the
 *  PID step runs -- so the target fields themselves can't be written. Each stage
 *  instead writes the active slot (both units) and clears "reached", the same
 *  thing the stock firmware does to change temperature mid-session. The stock
 *  heat-up, ready cue and PID then run per stage. The slot's original contents
 *  are restored when the session ends.
 *
 *  Time. The stock session countdown (real seconds) is set to the ramp's total
 *  length, and the current stage is derived from it. Timing is in the device's
 *  own seconds, and the stock session-length limit never cuts a ramp short:
 *  between ticks the countdown may only stay put or drop by one second, and any
 *  other change (a stock reload from the slot's hold time) is undone. The slot's
 *  hold time itself is never touched. Every hold is time AT temperature: on
 *  all three devices the stock clock only runs once the target is reached
 *  (Carta 2 0xb5b8, Aeris 0x8980, Sport 0x8448). A device whose clock ran
 *  through heat-up would get each heating second given back instead, up to
 *  RAMP_MAX_HEAT_S per stage (kept for that case; the host tests exercise it).
 *
 *  Dab counting. Once a ramp is on stage 2 or later and has run 20 seconds at
 *  temperature (the stock "reached" flag, seconds counted in at_temp_s), the
 *  stock completion counter increments run once, so the official app's counters
 *  report it at once; the
 *  stock save arming that persists them runs when the ramp ends, after the slot
 *  is restored, so no save can capture the ramp's temporary stage temperature.
 *  A counted ramp ends one second early through the stock stop and end cue
 *  WITHOUT the completion bookkeeping, so it is never counted twice. Ramps with
 *  single stage are counted by the stock completion as usual. A ramp stopped
 *  before that point is not counted, exactly as a stopped stock session isn't.
 *
 *  Safeguards. Waypoints are copied to RAM through the stock SPI read when a
 *  ramp arms and must all pass a range / consistency check (official-app
 *  limits) or the session runs as a plain stock session. The running state is
 *  checked every tick, and nothing restores a preset slot from state this code
 *  didn't provably write. The stock PID, heat-up and every stock stop path
 *  stay in charge of the heater throughout.
 */
#ifndef RAMP_H
#define RAMP_H

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;

#include "device.h"

/* ---- addresses ------------------------------------------------------------
 * Every code address in a device.h is a DISASSEMBLY address (the image with its
 * 40-byte header stripped, as Ghidra and objdump show it). On the device the
 * header occupies flash 0x00-0x27 and the body follows, so code actually runs
 * at disassembly address + 0x28. Proof: the header's reset branch (56 80) lands
 * on physical 0xb0 = disassembly 0x88 + 0x28, and every callback pointer the
 * stock firmware stores (0x14dd, 0x165cd, 0x17589 on the Carta 2) points 0x28
 * past a real function entry, with bit 0 set.
 *
 * Calls into stock code compile to `tjex rN`, which on TC32 behaves like Arm
 * `bx`: bit 0 of the target is an instruction-set flag and must be set (the
 * toolchain emits addr|1 for its own functions, as the stock pointers above do).
 *
 * So every call into stock code goes through STOCK_FN: + 0x28, then | 1.
 * Data addresses copied from stock literal pools (glyph tables, icons) are
 * already runtime addresses and are used as-is; RAM addresses need neither.
 * The patch's own code is linked at its runtime address (see tools/build.py). */
#ifndef STOCK_FN   /* tools/hosttest supplies its own, to run this code on a PC */
#define IMAGE_BASE            0x28u
#define STOCK_FN(type, addr)  ((type)(((u32)(addr) + IMAGE_BASE) | 1u))
#endif

typedef void  (*void_fn)(void);
typedef short (*rom_div_fn)(int, int);
#define rom_div         STOCK_FN(rom_div_fn, DEV_ROM_DIV)
#define orig_pid_tick   STOCK_FN(void_fn, DEV_PID_TICK)
#define stock_stop      STOCK_FN(void_fn, DEV_STOP)

/* ---- the central struct ------------------------------------------------- */
#define STRUCT_BASE    ((volatile u8 *)DEV_STRUCT)
#define FIELD16(off)   (*(volatile u16 *)(STRUCT_BASE + (off)))
#define PRESET(base, rank)  ((volatile u16 *)(STRUCT_BASE + DEV_PRESET_OFF(base, rank)))

/* ---- waypoint store (flash) ------------------------------------------------
 * [u16 magic][bank 0: 5 x {u16 F, u16 C, u16 hold_s}][bank 1: same]
 * bank 0 = flower (markers 0xB1-0xB5), bank 1 = concentrate (0xB6-0xBA).
 *
 * Always accessed through the stock SPI flash routines, never memory-mapped:
 * mapped reads go through the flash cache and could return stale data just
 * after a save. A ramp copies its bank into RAM (ramp_state_t.wp) once, when it
 * arms, validates it there, and runs from that copy -- so a save during a
 * running ramp can't change it either. */
#define RAMP_STORE_MAGIC   0xA52B
#define RAMP_NUM_BANKS     2
#define RAMP_NUM_SLOTS     5
#define RAMP_SLOT_SIZE     6
#define RAMP_STORE_SIZE    (2 + RAMP_NUM_BANKS * RAMP_NUM_SLOTS * RAMP_SLOT_SIZE)

typedef void (*flash_read_fn)(int addr, int len, void *buf);
typedef void (*flash_erase_fn)(int addr);
typedef void (*flash_write_fn)(int addr, int len, void *buf);
/* The most one flash_write call may take (a device.h can lower it: the
 * Carta 2's write verifies into a 64-byte stack buffer). */
#ifndef DEV_FLASH_WRITE_MAX
#define DEV_FLASH_WRITE_MAX  RAMP_STORE_TOTAL
#endif
#define flash_read   STOCK_FN(flash_read_fn, DEV_FLASH_READ)
#define flash_erase  STOCK_FN(flash_erase_fn, DEV_FLASH_ERASE)
#define flash_write  STOCK_FN(flash_write_fn, DEV_FLASH_WRITE)

/* ---- the ramp system's own on/off switch -----------------------------------
 * One more byte in the same flash sector as the waypoints (see
 * ramp_toggle_enabled in ramp_store.c), toggled from inside the preset picker
 * (ramp_picker.c): Carta 2 double click, Aeris and Sport triple click. Erased flash (0xFF) or
 * anything non-zero means enabled, so a store from before this existed, or
 * one that's never been touched, behaves exactly as it always has. */
#define RAMP_ENABLED_OFFSET  RAMP_STORE_SIZE
#define RAMP_SEL_OFFSET      (RAMP_STORE_SIZE + 1)   /* the chosen built-in preset */
#define RAMP_OFS_OFFSET      (RAMP_STORE_SIZE + 2)   /* setup offset, signed F */
#define RAMP_VER_OFFSET      (RAMP_STORE_SIZE + 3)   /* store layout version */
#define RAMP_STORE_TOTAL     (RAMP_STORE_SIZE + 4)
#define RAMP_STORE_VERSION   1                       /* 0xFF (erased) = written before versions */
/* The Carta 2 (0x964) and Aeris (0xa5c) page program doesn't split at 256-byte
 * page boundaries, so the whole store must stay inside the sector's first page. */
typedef char ramp_store_fits_one_page[(RAMP_STORE_TOTAL <= 256) ? 1 : -1];

static inline u8 ramp_enabled(void)
{
    u8 b;
    /* flash_read, not memory-mapped: the toggle that writes this byte can
     * fire on the tick right before this is checked (click -> this tick's
     * ramp_tick), which is exactly the stale-mapped-read window described
     * above -- so this follows the same rule as the rest of the store. */
    flash_read(DEV_RAMP_FLASH + RAMP_ENABLED_OFFSET, 1, &b);
    return b != 0;
}

/* Built-in presets (ramp_presets.c). Each device exposes DEV_PICK_COUNT of
 * them through its picker; an erased selection means the Balanced preset. */
#define RAMP_DEFAULT_PRESET  2
#define RAMP_OFS_MIN_F       (-10)
#define RAMP_OFS_MAX_F       15
/* Sent as a SET_TEMP marker with the offset in packet byte 14 (unread by stock
 * firmware; see ramp_marker_entry.s). Byte 14 is zero in every other packet. */
#define RAMP_OFFSET_MARKER   0xbb
/* App-started ramp: the stock session-start marker (A5) with this in packet
 * byte 14 -- unread by stock firmware -- asks for the session it starts to run
 * as a ramp, on whatever preset is active, with every preset value left as it
 * was (Terpline echoes them). It holds for RAMP_REQUEST_TICKS; see try_arm. */
#define RAMP_START_MARKER    0xa5
#define RAMP_START_REQUEST   0x52   /* 'R' */
#define RAMP_REQUEST_TICKS   (3u * 16u * 1000u * 1000u)

/* The chip's free-running system timer (SDK reg_system_tick, 16 ticks per us;
 * read throughout the stock image). Wraps every ~268 s, so only differences
 * are used. */
#ifndef DEV_SYS_TICK
#define DEV_SYS_TICK         (*(volatile u32 *)0x800740)
#endif
#define RAMP_PICKER_TIMEOUT  (30u * 16u * 1000u * 1000u)   /* 30 s idle in the picker */

/* A stage's hold only counts seconds at temperature (the stock "reached"
 * flag), on every device. In case a stage never reports reached -- a flag
 * misread, an atomizer that can't get there -- the hold starts counting anyway
 * after this many seconds of heating, so a ramp can't stall. */
#define RAMP_MAX_HEAT_S      120
/* On Aeris and Sport (device.h sets this) the stock session clock only runs
 * while "reached" is set, so a stage that never gets there never ticks at
 * all: the ramp times heat-up itself, on the system tick, and runs the clock
 * past the cap. */
#ifndef DEV_CLOCK_WAITS_FOR_REACHED
#define DEV_CLOCK_WAITS_FOR_REACHED  0
#endif
#define RAMP_SYS_TICKS_PER_S (16u * 1000u * 1000u)
u8  ramp_selected(void);
int ramp_offset(void);
void ramp_store_set(u8 off, u8 v);

/* the armed ramp's stages, from its RAM copy (0-based stage index) */
#define WP_F(st, s)     ((st)->wp[s][0])
#define WP_C(st, s)     ((st)->wp[s][1])
#define WP_HOLD(st, s)  ((st)->wp[s][2])

/* ---- arming sentinel: 150 F, or the stock C conversion of it (65; 66 if a
 * build rounds). Real presets never go below 275 F / 135 C. --------------- */
#define SENTINEL_F     150
#define IS_SENTINEL(f, c)  ((f) == SENTINEL_F || (c) == 65 || (c) == 66)

/* ---- preset slots: custom (rank 0) + 5, on every device ---------------- */
#define RAMP_MAX_RANK  5

/* ---- waypoint sanity (see count_stages): the official app's own limits --
 * flower 275-500 F, concentrate 365 F to the device's ceiling (DEV_MAX_F in
 * device.h), at most 300 s per stage (stock maximum 240 s). A store holding
 * anything outside these never arms. ---------------------------------------- */
#define RAMP_FL_MIN_F  275
#define RAMP_FL_MAX_F  500
#define RAMP_CO_MIN_F  365
#define RAMP_CO_MAX_F  DEV_MAX_F
#define RAMP_MAX_HOLD  300

/* ---- dab counting threshold ---------------------------------------------- */
#define COUNT_MIN_STAGE   2    /* a dab needs at least this stage ... */
#define COUNT_AT_TEMP_S   20   /* ... and this many seconds at temperature */

/* ---- runtime state (non-retention SRAM; magic-checked, never trusted at
 * power-on) ------------------------------------------------------------------ */
typedef struct {
    u16 magic;
    u8  stage;        /* 0 = idle; 1..n_stages = active */
    u8  n_stages;
    u8  bank;         /* 0 flower / 1 concentrate, locked at arm time */
    u8  rank;         /* preset slot the session was started from */
    u16 total_s;      /* sum of stage holds, seconds */
    u16 saved_f;      /* that slot's original contents (the sentinel), */
    u16 saved_c;      /*   restored when the session ends */
    u16 last_left;    /* the ramp's own view of the countdown -- see ramp_core.c */
    u16 at_temp_s;    /* seconds this ramp has run with the stock "reached" flag set */
    u8  counted;      /* stock completion bookkeeping already run this session */
    u8  arm_failed;   /* this session's sentinel found no usable store */
    u8  picker_on;    /* preset select mode (ramp_picker.c) is showing */
    u8  picker_sel;   /* the preset being shown while picker_on */
    u8  picker_dirty; /* picker_sel changed and not yet written to flash */
    u8  picker_enabled; /* the ramp system's on/off, cached while the picker shows */
    u32 picker_t0;    /* system tick of the picker's last event (timeout) */
    u8  save_held;    /* a stock settings save was pending when / while the ramp ran */
    u8  start_req;    /* the app asked for the next session to run as a ramp */
    u32 start_req_t0; /* system tick of that request */
    u8  requested;    /* this ramp was armed by that request, on a real preset */
    u16 saved_chk;    /* proof that saved_f / saved_c / rank / bank are try_arm's */
    u8  press_awake;  /* Aeris/Sport: the device was fully on when the button
                       * went down (not woken from standby by that press) */
    u16 heat_s;       /* seconds this stage has spent heating, not at temperature */
    u32 heat_t0;      /* Aeris/Sport: system tick heat_s was last advanced at */
    u16 ann_tries;    /* announcement send attempts left (ramp_announce.c) */
    u8  ann_enabled;  /* the announcement's fields, captured when it's queued */
    u8  ann_preset;
    u8  ann_offset;
    u8  frame_drawn;  /* display bookkeeping (Carta 2 screen) */
    u8  drawn_fill;
    u8  drawn_meas_y;
    u8  drawn_batt;
    u16 drawn_hero;
    u16 drawn_left;
    u16 drawn_target;
    u16 drawn_dabs;   /* Carta 2: the dab count last drawn */
    u8  trace_n;      /* Carta 2 chart: measured-temperature samples recorded */
    u8  trace[RAMP_TRACE_LEN];   /* one y per chart column, from ramp start */
    u16 wp[RAMP_NUM_SLOTS][3];   /* this ramp's stages {F, C, hold_s}, copied at arm */
} ramp_state_t;

#define RAMP_MAGIC     0xA5C9
#define RAMP_STATE     ((volatile ramp_state_t *)DEV_RAMP_STATE)

static inline u8 ramp_active(volatile ramp_state_t *st)
{
    return st->magic == RAMP_MAGIC && st->stage != 0 && st->stage <= st->n_stages;
}

static inline u16 ramp_elapsed(volatile ramp_state_t *st)
{
    u16 left = st->last_left;
    return left >= st->total_s ? 0 : (u16)(st->total_s - left);
}

/* Seconds from ramp start to the start of a 1-based stage. */
static inline u16 stage_start(volatile ramp_state_t *st, u8 stage)
{
    u16 t = 0;
    u8 i;
    for (i = 1; i < stage; i++)
        t += WP_HOLD(st, i - 1);
    return t;
}

/* Current target in the device's display unit. */
static inline u16 stage_target_display(volatile ramp_state_t *st)
{
    return DEV_SCALE_IS_F() ? WP_F(st, st->stage - 1) : WP_C(st, st->stage - 1);
}

void ramp_step(volatile ramp_state_t *st, int dir);
void ramp_picker_draw(u8 sel, u8 enabled);   /* Carta 2 only */

/* Picker colours on the LEDs and the button light: one per preset (Flavor,
 * Rosin, Balanced, Sauce, Long session, Clouds), red when the system is off. */
extern const u8 RAMP_PRESET_RGB[6][3];   /* ramp_presets.c */
#define RAMP_OFF_R   255
#define RAMP_OFF_G   0
#define RAMP_OFF_B   0

void ramp_toggle_enabled(void);
u8   ramp_default_stages(volatile ramp_state_t *st, u8 sel);
void ramp_announce_tick(volatile ramp_state_t *st);
void ramp_picker_close(volatile ramp_state_t *st);
/* A device state in which a sentinel session must stay a stock session (Aeris
 * and Sport: quick heat, UI state 7). */
/* Each device saves its settings -- preset slots included -- when a countdown
 * (DEV_SAVE_TIMER) reaches zero; stock arms it with DEV_SAVE_DELAY after a
 * button or app change. Firing while a ramp runs would persist the ramp's
 * temporary stage temperature into the trigger slot, so a ramp holds any
 * pending save (zeroing the countdown cancels it) and re-arms it once the slot
 * holds its own value again. */
#ifndef DEV_ARM_BLOCKED
#define DEV_ARM_BLOCKED()    0
#endif
#ifndef DEV_PICKER_CLOSED
#define DEV_PICKER_CLOSED()  ((void)0)   /* Carta 2: redraw the stock idle screen */
#endif
u8   ramp_picker_event(volatile ramp_state_t *st, int ev, u8 idle,
                       int enter, int next, int prev, int exit, int toggle);

#endif
