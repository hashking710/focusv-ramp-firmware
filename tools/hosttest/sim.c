/* sim.c -- host tests for the shared ramp code (the common/ sources).
 *
 * Models the stock behaviour the patch relies on, as traced in each device.h:
 *   - while "reached" is 0 the orchestrator reloads its target from the active
 *     preset slot; the heater moves toward it (1 F per tick here);
 *   - "reached" is set once the measured temperature is at the target;
 *   - the session countdown ticks once a second -- only while reached on Aeris
 *     and Sport and Carta 2, always in the free-running model (sim_carta_timing,
 *     which no device has -- it exercises the give-back path);
 *   - at zero the stock timer counts the session and stops it.
 * Then drives whole ramps, the picker, the store, the offset marker and the
 * announcement through the real patch code, and checks the results.
 *
 * Run from the repo root: sh tools/hosttest/run.sh
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "ramp.h"

/* entry points normally reached only from the assembly hooks */
void ramp_trampoline(void);
void ramp_marker_dispatch(u8 marker, u8 byte14);
int  ramp_announce_entry(int handle, const u8 *data, int len);
void ramp_event_entry(void);   /* aeris/ramp_event.c */

unsigned char sim_struct[256];
unsigned char sim_dab[64];
unsigned char sim_cue[16];
unsigned char sim_state[1024] __attribute__((aligned(8)));
unsigned char sim_ui[64];
unsigned char sim_mb[2];
static int consumed = -1;   /* the event the stock consumer last got */
void sim_consumer(void) { if (sim_mb[1]) { consumed = sim_mb[0]; sim_mb[1] = 0; } }

unsigned int sim_systick;
int sim_picker_closed;
static unsigned char flash[0x2000];   /* both store copies: DEV_RAMP_FLASH, RAMP_STORE_ALT */
static int power_ops = -1;           /* >= 0: flash operations left before a power cut */
static int erases, never_reach;
static int carta_timing;     /* 1: free-running clock (no device; give-back path) */
int sim_clock_waits;         /* !carta_timing, as the device.h macro sees it */
static int tick_no;
static int notify_busy;      /* sim_notify refuses this many packets first */
static unsigned char last_pkt[32];
static int last_len, notify_ok;
static int fails;

#define TPS 10               /* main-loop ticks per second */

#define CHECK(c, ...) do { if (!(c)) { fails++; printf("  FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static unsigned short u16at(unsigned char *b, int off) { return (unsigned short)(b[off] | b[off + 1] << 8); }
static void set16(unsigned char *b, int off, int v) { b[off] = (unsigned char)v; b[off + 1] = (unsigned char)(v >> 8); }

/* ---- stock stand-ins ------------------------------------------------------ */
short sim_div(int a, int b) { return (short)(a / b); }

void sim_flash_read(int addr, int len, void *buf) { memcpy(buf, flash + (addr - DEV_RAMP_FLASH), len); }
static int powered(void) { if (power_ops < 0) return 1; if (power_ops == 0) return 0; power_ops--; return 1; }
void sim_flash_erase(int addr) { if (!powered()) return; erases++; memset(flash + (addr - DEV_RAMP_FLASH), 0xff, 0x1000); }
static int write_too_long;
void sim_flash_write(int addr, int len, void *buf)
{
    int i;
    if (!powered())
        return;
    if (len > DEV_FLASH_WRITE_MAX)
        write_too_long++;
    for (i = 0; i < len; i++)   /* NOR: a write can only clear bits */
        flash[addr - DEV_RAMP_FLASH + i] &= ((unsigned char *)buf)[i];
}

int sim_notify(int handle, const unsigned char *data, int len)
{
    if (notify_busy > 0) { notify_busy--; return 0x81; }
    if (handle == 27 && len <= 32) { memcpy(last_pkt, data, len); last_len = len; notify_ok++; }
    return 0;
}

static int conc(void) { return sim_struct[0x06] != 1; }
static int rank(void) { return sim_struct[conc() ? 0x08 : 0x07]; }
static int slot_f(void) { return u16at(sim_struct, ((rank() + (conc() ? TBL_CO_F : TBL_FL_F)) * 2)); }

void sim_stop(void) { sim_struct[OFF_SESSION] = 0; }

static int target_f;
static int save_runs, saves, bad_saves, last_saved_f;   /* the stock settings save */
void sim_pid_tick(void)
{
    int meas;
    if (save_runs && sim_dab[32] && --sim_dab[32] == 0) {   /* stock: save the slots */
        saves++;
        last_saved_f = slot_f();
        if (last_saved_f != 150) bad_saves++;
    }
    if (!sim_struct[OFF_SESSION])
        return;
    if (!sim_struct[OFF_REACHED])
        target_f = slot_f();
    meas = u16at(sim_struct, OFF_MEAS_F);
    if (meas < target_f) meas++;
    else if (meas > target_f) meas--;
    set16(sim_struct, OFF_MEAS_F, meas);
    if (!sim_struct[OFF_REACHED] && meas == target_f && !never_reach)
        sim_struct[OFF_REACHED] = 1;
    if (tick_no % TPS == 0 && (carta_timing || sim_struct[OFF_REACHED])) {
        int left = u16at(sim_struct, OFF_COUNTDOWN);
        if (left > 0) set16(sim_struct, OFF_COUNTDOWN, --left);
        if (left == 0) {    /* stock completion: count, arm the save, stop */
            int base = conc() ? 2 : 0;
            set16(sim_dab, base, u16at(sim_dab, base) + 1);
            sim_dab[31] = 200; sim_dab[32] = 250;
            sim_stop();
        }
    }
}

/* ---- helpers ----------------------------------------------------------------- */
static void reset_device(int carta)
{
    memset(sim_struct, 0, sizeof sim_struct);
    memset(sim_dab, 0, sizeof sim_dab);
    memset(sim_cue, 0, sizeof sim_cue);
    memset(sim_state, 0xa7, sizeof sim_state);      /* power-on garbage */
    memset(flash, 0xff, sizeof flash);
    power_ops = -1;
    memset(sim_ui, 0, sizeof sim_ui);
    memset(sim_mb, 0, sizeof sim_mb);
    consumed = -1;
    carta_timing = carta;
    sim_clock_waits = !carta;
    sim_struct[0x04] = 0;   /* F scale */
    tick_no = 0;
    notify_busy = 0; notify_ok = 0; last_len = 0;
    erases = 0; never_reach = 0; save_runs = 0; saves = 0; bad_saves = 0; last_saved_f = 0; sim_picker_closed = 0; sim_systick = 0x12345678u;
}

static volatile ramp_state_t *ST(void) { return RAMP_STATE; }

/* the current store copy, as the patch reads it */
static unsigned char *cur(void) { u32 a = ramp_store_addr(); return flash + (a ? a - DEV_RAMP_FLASH : 0); }

static void tick(void) { tick_no++; sim_systick += 16000000u / TPS; ramp_trampoline(); }

/* a session on custom slot 0 at temperature f with hold h seconds */
static void start_session(int is_conc, int f, int c, int hold)
{
    int b = is_conc ? TBL_CO_F : TBL_FL_F, bc = is_conc ? TBL_CO_C : TBL_FL_C, bh = is_conc ? TBL_CO_HOLD : TBL_FL_HOLD;
    sim_struct[0x06] = is_conc ? 2 : 1;
    sim_struct[is_conc ? 0x08 : 0x07] = 0;
    set16(sim_struct, (0 + b) * 2, f);
    set16(sim_struct, (0 + bc) * 2, c);
    set16(sim_struct, (0 + bh) * 2, hold);
    set16(sim_struct, OFF_COUNTDOWN, hold);
    set16(sim_struct, OFF_MEAS_F, 77);
    sim_struct[OFF_REACHED] = 0;
    sim_struct[OFF_SESSION] = 1;
}

/* the app's stage save: the stock handler writes the custom slot, then the marker */
static void upload(int is_conc, int stage, int f, int c, int hold)
{
    int b = is_conc ? TBL_CO_F : TBL_FL_F, bc = is_conc ? TBL_CO_C : TBL_FL_C, bh = is_conc ? TBL_CO_HOLD : TBL_FL_HOLD;
    set16(sim_struct, b * 2, f);
    set16(sim_struct, bc * 2, c);
    set16(sim_struct, bh * 2, hold);
    ramp_marker_dispatch((u8)((is_conc ? 0xb6 : 0xb1) + stage - 1), 0);
}

struct run { int stage_at[6]; int stage_temp[6]; int stages_seen; int counted_at; int ended_at; int at_temp_at_count; };

/* runs until the session ends (or a cap), recording when each stage begins */
static struct run run_to_end(int cap_s)
{
    struct run r;
    int last_stage = 0, i;
    memset(&r, 0, sizeof r);
    r.counted_at = -1; r.ended_at = -1;
    for (i = 0; i < cap_s * TPS; i++) {
        tick();
        if (ST()->magic == RAMP_MAGIC && ST()->stage != last_stage && ST()->stage) {
            last_stage = ST()->stage;
            if (last_stage <= 5) { r.stage_at[last_stage] = tick_no / TPS; r.stage_temp[last_stage] = slot_f(); }
            r.stages_seen++;
        }
        if (r.counted_at < 0 && ST()->counted) { r.counted_at = tick_no / TPS; r.at_temp_at_count = ST()->at_temp_s; }
        if (!sim_struct[OFF_SESSION]) { r.ended_at = tick_no / TPS; break; }
    }
    for (i = 0; i < 20; i++) tick();    /* let disarm run */
    return r;
}

/* ---- tests ------------------------------------------------------------------- */
static void t_default_preset(int carta)
{
    struct run r;
    printf("default preset (Balanced) in concentrate, %s timing\n", carta ? "free-running" : "waits-for-reached (all three devices)");
    reset_device(carta);
    tick();
    start_session(1, 150, 65, 30);
    r = run_to_end(400);
    CHECK(r.stages_seen == 4, "stages seen %d", r.stages_seen);
    CHECK(r.stage_temp[1] == 455 && r.stage_temp[2] == 470 && r.stage_temp[3] == 485 && r.stage_temp[4] == 505,
          "stage temps %d %d %d %d", r.stage_temp[1], r.stage_temp[2], r.stage_temp[3], r.stage_temp[4]);
    CHECK(r.counted_at >= 0, "dab never counted");
    CHECK(r.at_temp_at_count >= 20, "counted with only %d s at temperature", r.at_temp_at_count);
    CHECK(u16at(sim_dab, 2) == 1 && u16at(sim_dab, 0) == 0, "conc dab counter %d flower %d", u16at(sim_dab, 2), u16at(sim_dab, 0));
    CHECK(sim_dab[31] == 200 && sim_dab[32] == 250, "save not armed");
    CHECK(u16at(sim_struct, TBL_CO_F * 2) == 150, "slot not restored: %d", u16at(sim_struct, TBL_CO_F * 2));
    CHECK(r.ended_at > 0, "ramp never ended");
    {   /* every hold is time at temperature, on both clock rules: stage 1 adds the
         * heat-up from room temperature, later stages a few seconds of heating */
        int k;
        static const int hold[4] = { 0, 30, 25, 25 };
        for (k = 1; k < 4; k++) {
            int span = r.stage_at[k + 1] - r.stage_at[k];
            CHECK(span >= hold[k] && span <= hold[k] + (k == 1 ? 45 : 4), "stage %d lasted %d s for a %d s hold", k, span, hold[k]);
        }
    }
    printf("  stages start at %ds %ds %ds %ds, dab at %ds (%d s at temp), ended %ds\n",
           r.stage_at[1], r.stage_at[2], r.stage_at[3], r.stage_at[4], r.counted_at, r.at_temp_at_count, r.ended_at);
}

static void t_flower_no_default(void)
{
    struct run r;
    printf("flower sentinel with nothing saved: plain session\n");
    reset_device(0);
    tick();
    start_session(0, 150, 65, 5);
    r = run_to_end(200);
    CHECK(r.stages_seen == 0, "a ramp ran in flower (%d stages)", r.stages_seen);
    CHECK(u16at(sim_dab, 0) == 1, "stock completion should count it once: %d", u16at(sim_dab, 0));
}

static void t_upload_precedence(void)
{
    struct run r;
    printf("uploaded stages win over the default; stage 1 clears 2-5\n");
    reset_device(0);
    tick();
    upload(1, 1, 400, 204, 10); upload(1, 2, 420, 216, 10); upload(1, 3, 440, 227, 10);
    upload(1, 1, 430, 221, 12); upload(1, 2, 450, 232, 12);   /* shorter second upload */
    start_session(1, 150, 65, 30);
    r = run_to_end(300);
    CHECK(r.stages_seen == 2, "stages %d (stale stage 3 survived?)", r.stages_seen);
    CHECK(r.stage_temp[1] == 430 && r.stage_temp[2] == 450, "temps %d %d", r.stage_temp[1], r.stage_temp[2]);
    CHECK(u16at(sim_dab, 2) == 1, "dab counted %d times", u16at(sim_dab, 2));
}

static void t_unusable_store(void)
{
    struct run r;
    printf("an unusable upload is refused, and doesn't fall back to the default\n");
    reset_device(0);
    tick();
    upload(1, 1, 700, 371, 10);        /* above the concentrate ceiling */
    start_session(1, 150, 65, 5);
    r = run_to_end(200);
    CHECK(r.stages_seen == 0, "armed on a bad store");
}

static void t_offset(void)
{
    struct run r;
    printf("offset marker: +15 on Clouds merges the two 520 F stages\n");
    reset_device(0);
    tick();
    ramp_marker_dispatch(RAMP_OFFSET_MARKER, 15);
    ramp_marker_dispatch(RAMP_OFFSET_MARKER, (u8)(signed char)40);   /* out of range: ignored */
    CHECK(ramp_offset() == 15, "offset %d", ramp_offset());
    ramp_store_set(RAMP_SEL_OFFSET, 5);                              /* Clouds */
    start_session(1, 150, 65, 30);
    r = run_to_end(300);
    CHECK(r.stages_seen == 3, "stages %d", r.stages_seen);
    CHECK(r.stage_temp[1] == 485 && r.stage_temp[2] == 505 && r.stage_temp[3] == 520,
          "temps %d %d %d", r.stage_temp[1], r.stage_temp[2], r.stage_temp[3]);
    ramp_marker_dispatch(RAMP_OFFSET_MARKER, (u8)(signed char)-10);
    CHECK(ramp_offset() == -10, "negative offset %d", ramp_offset());
}

static void t_picker(void)
{
    struct run r;
    volatile ramp_state_t *st;
    printf("picker: open, step, toggle, leave; closes when not idle\n");
    reset_device(0);
    tick();
    st = ST();
    CHECK(ramp_picker_event(st, 3, 1, 3, 2, 1, 7, 8) == 1 && st->picker_on, "didn't open");
    CHECK(st->picker_sel == RAMP_DEFAULT_PRESET, "starts at %d", st->picker_sel);
    ramp_picker_event(st, 2, 1, 3, 2, 1, 7, 8);
    ramp_picker_event(st, 2, 1, 3, 2, 1, 7, 8);
    ramp_picker_event(st, 2, 1, 3, 2, 1, 7, 8);
    ramp_picker_event(st, 2, 1, 3, 2, 1, 7, 8);
    CHECK(st->picker_sel == 0, "wrap: %d", st->picker_sel);
    ramp_picker_event(st, 1, 1, 3, 2, 1, 7, 8);
    CHECK(st->picker_sel == 5, "back-wrap: %d", st->picker_sel);
    CHECK(ramp_picker_event(st, 4, 1, 3, 2, 1, 7, 8) == 1, "other events must be swallowed");
    ramp_picker_event(st, 8, 1, 3, 2, 1, 7, 8);                  /* toggle off */
    CHECK(!ramp_enabled() && !st->picker_enabled, "toggle off failed");
    ramp_picker_event(st, 7, 1, 3, 2, 1, 7, 8);                  /* leave */
    CHECK(!st->picker_on && ramp_selected() == 5, "selection %d not saved", ramp_selected());
    start_session(1, 150, 65, 3);
    r = run_to_end(100);
    CHECK(r.stages_seen == 0, "armed while the system is off");
    CHECK(ramp_picker_event(st, 3, 1, 3, 2, 1, 7, 8) == 1, "picker must open while off");
    ramp_picker_event(st, 8, 1, 3, 2, 1, 7, 8);                  /* toggle on */
    CHECK(ramp_enabled(), "toggle on failed");
    CHECK(ramp_picker_event(st, 2, 0, 3, 2, 1, 7, 8) == 0 && !st->picker_on, "must close and pass through when not idle");
    CHECK(ramp_picker_event(st, 3, 0, 3, 2, 1, 7, 8) == 0, "must not open when not idle");
    start_session(1, 150, 65, 30);
    ramp_picker_event(st, 3, 1, 3, 2, 1, 7, 8);
    tick(); tick();
    CHECK(!st->picker_on || ramp_active(st), "picker still open in a session");
    r = run_to_end(300);
    CHECK(r.stage_temp[1] == 470 + 0 && r.stages_seen == 4, "didn't run Clouds: %d stages, first %d", r.stages_seen, r.stage_temp[1]);
}

static void t_stop_early(void)
{
    int i;
    printf("stopped before the dab point: not counted, slot restored\n");
    reset_device(0);
    tick();
    start_session(1, 150, 65, 30);
    for (i = 0; i < 200; i++) tick();
    sim_stop();
    run_to_end(5);
    CHECK(u16at(sim_dab, 2) == 0, "counted a stopped ramp");
    CHECK(u16at(sim_struct, TBL_CO_F * 2) == 150, "slot %d", u16at(sim_struct, TBL_CO_F * 2));
}

static void t_step(void)
{
    int i;
    printf("+/- stage jumps\n");
    reset_device(1);
    tick();
    start_session(1, 150, 65, 30);
    for (i = 0; i < 5; i++) tick();
    ramp_step(ST(), 1); ramp_step(ST(), 1); ramp_step(ST(), 1); ramp_step(ST(), 1);
    CHECK(ST()->stage == 4, "stage %d", ST()->stage);
    ramp_step(ST(), -1);
    CHECK(ST()->stage == 3 && slot_f() == 485, "stage %d temp %d", ST()->stage, slot_f());
    CHECK(!ST()->counted, "counted on a jump with no time at temperature");
}

static void t_announce(void)
{
    static const unsigned char aa[19] = { 0xaa, 19 };
    int i;
    printf("announcement after the 0xAA reply, through a busy queue\n");
    reset_device(0);
    tick();
    ramp_marker_dispatch(RAMP_OFFSET_MARKER, (u8)(signed char)-5);
    ramp_store_set(RAMP_SEL_OFFSET, 3);
    notify_busy = 0;
    CHECK(ramp_announce_entry(27, aa, 19) == 0, "0xAA send failed");
    CHECK(last_len == 19 && last_pkt[0] == 0xaa, "0xAA not passed through unchanged");
    notify_busy = 3000;                 /* the sync burst filling the queue */
    for (i = 0; i < 3100; i++) tick();
    CHECK(last_len == 12, "announcement not sent (len %d)", last_len);
    CHECK(last_pkt[0] == 0xbc && last_pkt[1] == 12 && !memcmp(last_pkt + 2, "TRMP", 4) && last_pkt[11] == 0xbc, "framing");
    CHECK(last_pkt[7] == DEV_ID && last_pkt[8] == 1 && last_pkt[9] == 3 && (signed char)last_pkt[10] == -5,
          "fields dev %d en %d preset %d ofs %d", last_pkt[7], last_pkt[8], last_pkt[9], (signed char)last_pkt[10]);
    i = notify_ok;
    tick(); tick();
    CHECK(notify_ok == i, "kept sending after success");
}

static void t_garbage_ram(void)
{
    printf("power-on RAM that happens to hold the magic\n");
    reset_device(0);
    memset(sim_state, 0, sizeof sim_state);
    ((volatile ramp_state_t *)sim_state)->magic = RAMP_MAGIC;
    ((volatile ramp_state_t *)sim_state)->stage = 3;
    ((volatile ramp_state_t *)sim_state)->n_stages = 2;     /* inconsistent */
    sim_struct[OFF_SESSION] = 1;
    tick(); tick();
    CHECK(ST()->stage == 0, "acted on an inconsistent state");
    CHECK(u16at(sim_struct, TBL_CO_F * 2) == 0 && u16at(sim_struct, TBL_FL_F * 2) == 0, "wrote a slot from garbage");
}

static void t_heat_cap(void)
{
    struct run r;
    printf("free-running clock, a stage that never reports reached: the heat cap keeps it moving\n");
    reset_device(1);
    never_reach = 1;
    tick();
    start_session(1, 150, 65, 30);
    r = run_to_end(1200);
    CHECK(r.stages_seen == 4, "stalled at %d stages", r.stages_seen);
    CHECK(r.ended_at > 0, "never ended");
    CHECK(r.stage_at[2] - r.stage_at[1] >= RAMP_MAX_HEAT_S + 30 - 1, "stage 1 lasted only %d s", r.stage_at[2] - r.stage_at[1]);
    CHECK(u16at(sim_dab, 2) == 0, "counted a dab with no time at temperature");
    printf("  ended at %ds\n", r.ended_at);
}

static void t_heat_cap_waits(void)
{
    struct run r;
    printf("waits-for-reached clock (all three devices), a stage that never reports reached: the ramp runs the clock past the cap\n");
    reset_device(0);
    never_reach = 1;
    tick();
    start_session(1, 150, 65, 30);
    r = run_to_end(1200);
    CHECK(r.stages_seen == 4, "stalled at %d stages", r.stages_seen);
    CHECK(r.ended_at > 0, "never ended");
    CHECK(r.stage_at[2] - r.stage_at[1] >= RAMP_MAX_HEAT_S + 30 - 1, "stage 1 lasted only %d s", r.stage_at[2] - r.stage_at[1]);
    CHECK(u16at(sim_dab, 2) == 0, "counted a dab with no time at temperature");
    printf("  ended at %ds\n", r.ended_at);
}

static void t_save_held(void)
{
    struct run r;
    int i;
    printf("a stock settings save pending at the start never captures a stage temperature\n");
    reset_device(0);
    save_runs = 1;
    tick();
    sim_dab[32] = 30;                 /* armed by a button change a moment ago */
    start_session(1, 150, 65, 30);
    r = run_to_end(400);
    CHECK(r.ended_at > 0, "never ended");
    CHECK(bad_saves == 0, "%d saves wrote a stage temperature", bad_saves);
    for (i = 0; i < 300 && !saves; i++) tick();
    CHECK(saves > 0 && last_saved_f == 150, "held save not re-armed (saves %d, slot %d)", saves, last_saved_f);
}

/* a session on preset slot `rank` as it is (no slot writes), the way stock
 * starts one after an app start marker */
static void start_on_rank(int is_conc, int rank, int f, int c, int hold)
{
    int b = is_conc ? TBL_CO_F : TBL_FL_F, bc = is_conc ? TBL_CO_C : TBL_FL_C, bh = is_conc ? TBL_CO_HOLD : TBL_FL_HOLD;
    sim_struct[0x06] = is_conc ? 2 : 1;
    sim_struct[is_conc ? 0x08 : 0x07] = (unsigned char)rank;
    set16(sim_struct, (rank + b) * 2, f);
    set16(sim_struct, (rank + bc) * 2, c);
    set16(sim_struct, (rank + bh) * 2, hold);
    set16(sim_struct, OFF_COUNTDOWN, hold);
    set16(sim_struct, OFF_MEAS_F, 77);
    sim_struct[OFF_REACHED] = 0;
    sim_struct[OFF_SESSION] = 1;
}

static void t_app_request(void)
{
    struct run r;
    int i;
    printf("app-requested ramp on a real preset: runs, then the slot and the custom preset are as they were\n");
    reset_device(0);
    tick();
    set16(sim_struct, (0 + TBL_CO_F) * 2, 400);   /* the user's custom preset */
    set16(sim_struct, (0 + TBL_CO_C) * 2, 204);
    ramp_marker_dispatch(RAMP_START_MARKER, RAMP_START_REQUEST);
    start_on_rank(1, 2, 480, 248, 40);
    r = run_to_end(400);
    CHECK(r.stages_seen == 4 && r.stage_temp[1] == 455, "stages %d first %d", r.stages_seen, r.stage_temp[1]);
    CHECK(u16at(sim_struct, (2 + TBL_CO_F) * 2) == 480 && u16at(sim_struct, (2 + TBL_CO_C) * 2) == 248,
          "slot 2 left at %d / %d", u16at(sim_struct, (2 + TBL_CO_F) * 2), u16at(sim_struct, (2 + TBL_CO_C) * 2));
    CHECK(u16at(sim_struct, TBL_CO_F * 2) == 400 && u16at(sim_struct, TBL_CO_C * 2) == 204, "custom preset changed");

    printf("app-requested ramp on a slot whose F and C disagree (stock's C table write): still put back\n");
    reset_device(0);
    tick();
    ramp_marker_dispatch(RAMP_START_MARKER, RAMP_START_REQUEST);
    start_on_rank(1, 3, (9 * 249 + 288) / 5, 249, 40);
    r = run_to_end(400);
    CHECK(r.stages_seen == 4, "stages %d", r.stages_seen);
    CHECK(u16at(sim_struct, (3 + TBL_CO_F) * 2) == (9 * 249 + 288) / 5 && u16at(sim_struct, (3 + TBL_CO_C) * 2) == 249,
          "slot 3 left at %d / %d", u16at(sim_struct, (3 + TBL_CO_F) * 2), u16at(sim_struct, (3 + TBL_CO_C) * 2));

    printf("app-requested ramp with nothing to run (flower, no stages): the session is stopped\n");
    reset_device(0);
    tick();
    ramp_marker_dispatch(RAMP_START_MARKER, RAMP_START_REQUEST);
    start_on_rank(0, 1, 380, 193, 120);
    for (i = 0; i < 3; i++) tick();
    CHECK(sim_struct[OFF_SESSION] == 0, "session still running");
    CHECK(u16at(sim_struct, (1 + TBL_FL_F) * 2) == 380, "slot changed");

    printf("a stale request (over 3 s old) doesn't turn a later session into a ramp\n");
    reset_device(0);
    tick();
    ramp_marker_dispatch(RAMP_START_MARKER, RAMP_START_REQUEST);
    for (i = 0; i < 4 * TPS; i++) tick();
    start_on_rank(1, 2, 480, 248, 40);
    for (i = 0; i < 5; i++) tick();
    CHECK(ST()->stage == 0 && sim_struct[OFF_SESSION] == 1, "stage %d session %d", ST()->stage, sim_struct[OFF_SESSION]);

    printf("a start marker without the request code: a plain stock session\n");
    reset_device(0);
    tick();
    ramp_marker_dispatch(RAMP_START_MARKER, 0);
    start_on_rank(1, 2, 480, 248, 40);
    for (i = 0; i < 5; i++) tick();
    CHECK(ST()->stage == 0, "armed without a request");
}

static void announce_now(void)
{
    static const unsigned char aa[19] = { 0xaa };
    ramp_announce_entry(27, aa, 19);
    tick();
}

static void t_stock_mode(void)
{
    struct run r;
    int e;
    printf("stock mode: nothing arms, no marker but the switch acts; switching back restores ramps\n");
    reset_device(0);
    tick();
    upload(1, 1, 430, 221, 12);
    announce_now();
    CHECK(last_pkt[6] == 3 && last_pkt[8] == 0x01, "ramp mode announce: protocol %d flags %#x", last_pkt[6], last_pkt[8]);

    ramp_marker_dispatch(RAMP_MODE_MARKER, RAMP_MODE_STOCK);
    CHECK(ramp_stock_mode() && ST()->stock_mode == 1, "not in stock mode");
    CHECK(cur()[RAMP_MODE_OFFSET] == RAMP_MODE_STOCK, "mode byte %#x", cur()[RAMP_MODE_OFFSET]);
    tick();
    CHECK(last_pkt[0] == 0xbc && last_pkt[8] == 0x03, "the switch should announce stock mode at once: flags %#x", last_pkt[8]);

    e = erases;
    upload(1, 1, 480, 249, 30);                                   /* ignored */
    ramp_marker_dispatch(RAMP_OFFSET_MARKER, 10);                  /* ignored */
    ramp_marker_dispatch(RAMP_START_MARKER, RAMP_START_REQUEST);   /* ignored */
    CHECK(erases == e && ramp_offset() == 0 && ST()->start_req == 0, "a marker acted in stock mode");
    start_session(1, 150, 65, 5);                                  /* the sentinel: a plain session */
    r = run_to_end(200);
    CHECK(r.stages_seen == 0 && u16at(sim_struct, TBL_CO_F * 2) == 150, "armed in stock mode (%d stages)", r.stages_seen);
    CHECK(u16at(sim_dab, 2) == 1, "stock completion should count it once: %d", u16at(sim_dab, 2));

    memset(sim_state, 0xa7, sizeof sim_state);                     /* power cycle: RAM lost */
    CHECK(ramp_stock_mode(), "stock mode lost before the first tick");
    tick();
    CHECK(ST()->stock_mode == 1, "stock mode lost after a power cycle");

    ramp_marker_dispatch(RAMP_MODE_MARKER, 0x00);                  /* not a mode code: ignored */
    CHECK(ramp_stock_mode(), "an unknown code changed the mode");
    ramp_marker_dispatch(RAMP_MODE_MARKER, RAMP_MODE_RAMP);
    CHECK(!ramp_stock_mode() && cur()[RAMP_MODE_OFFSET] == 0xff, "not back in ramp mode");
    start_session(1, 150, 65, 30);
    r = run_to_end(300);
    CHECK(r.stages_seen == 1 && r.stage_temp[1] == 430, "the saved ramp didn't survive stock mode: %d stages", r.stages_seen);

    printf("switching back to ramp mode mid-session leaves that session stock\n");
    ramp_marker_dispatch(RAMP_MODE_MARKER, RAMP_MODE_STOCK);
    start_session(1, 150, 65, 5);
    tick();
    ramp_marker_dispatch(RAMP_MODE_MARKER, RAMP_MODE_RAMP);
    r = run_to_end(200);
    CHECK(r.stages_seen == 0, "armed halfway through a stock session (%d stages)", r.stages_seen);
    start_session(1, 150, 65, 30);
    r = run_to_end(300);
    CHECK(r.stages_seen == 1, "the next session didn't arm: %d stages", r.stages_seen);

    printf("the mode switch is refused while a ramp runs\n");
    start_session(1, 150, 65, 30);
    tick();
    CHECK(ramp_active(ST()), "ramp didn't arm");
    ramp_marker_dispatch(RAMP_MODE_MARKER, RAMP_MODE_STOCK);
    CHECK(!ramp_stock_mode() && cur()[RAMP_MODE_OFFSET] == 0xff, "switched mid-ramp");
}

static void t_power_cut(void)
{
    struct run r;
    int n, done = 0;
    printf("a power cut at any point of a save leaves the old store or the new one, never none\n");
    for (n = 0; n < 12 && !done; n++) {
        int ofs;
        reset_device(0);
        tick();
        upload(1, 1, 430, 221, 12);
        ramp_marker_dispatch(RAMP_OFFSET_MARKER, 5);
        ramp_marker_dispatch(RAMP_OFFSET_MARKER, 6);   /* both copies in use */
        power_ops = n;                                  /* the cut */
        ramp_marker_dispatch(RAMP_OFFSET_MARKER, 9);
        done = power_ops > 0;                           /* the save finished before the cut */
        power_ops = -1;                                 /* power back */
        memset(sim_state, 0xa7, sizeof sim_state);
        tick();
        ofs = ramp_offset();
        CHECK(ofs == 6 || ofs == 9, "after a cut at operation %d the offset reads %d", n, ofs);
        CHECK(n < 5 || done || ofs == 9, "cut at %d: a complete save was lost", n);
        start_session(1, 150, 65, 30);
        r = run_to_end(300);
        CHECK(r.stages_seen == 1 && r.stage_temp[1] == 430, "cut at %d: the saved ramp is gone (%d stages)", n, r.stages_seen);
    }
    CHECK(done, "the save never completed");
}

/* the Aeris button hook, as the consumer's caller: one event through it */
static void press_event(int ev, int clicks)
{
    sim_ui[0x1d] = (unsigned char)clicks;
    sim_mb[0] = (unsigned char)ev;
    sim_mb[1] = 1;
    consumed = -1;
    ramp_event_entry();
}

static void t_mode_gesture(void)
{
    int k;
    printf("five presses with the fifth held switch Terpline / Focus V on the Aeris and Sport\n");
    reset_device(0);
    tick();
    sim_ui[2] = 1;                                   /* on */
    sim_ui[0x0e] = 1;                                /* LEDs on */
    for (k = 1; k <= 5; k++) press_event(16, k);     /* the five presses reach stock */
    CHECK(consumed == 16 && !ramp_stock_mode(), "a press was taken");
    press_event(15, 5);
    CHECK(ramp_stock_mode() && ST()->mode_cue == RAMP_CUE_FOCUSV, "didn't switch to Focus V");
    CHECK(consumed == 16, "the hold reached stock as %d", consumed);
    press_event(15, 5);                              /* again, from stock mode */
    CHECK(!ramp_stock_mode() && ST()->mode_cue == RAMP_CUE_TERPLINE, "didn't switch back to Terpline");

    press_event(15, 4);                              /* 4 + hold: stock's own (dim mode), untouched */
    CHECK(!ramp_stock_mode() && consumed == 15, "4 presses + hold was taken (%d)", consumed);
    press_event(16, 1);                              /* a single press + hold: the picker, not the mode */
    press_event(15, 1);
    CHECK(ST()->picker_on && !ramp_stock_mode(), "single hold should open the picker");
    press_event(15, 5);                              /* from inside the picker: switches, picker closes */
    CHECK(ramp_stock_mode() && !ST()->picker_on, "picker open: no switch or picker left open");
    press_event(15, 5);

    start_session(1, 150, 65, 30);                   /* during a session: stock's hold (stop) */
    tick();
    press_event(15, 5);
    CHECK(!ramp_stock_mode() && consumed == 15, "switched during a session (%d)", consumed);
    sim_struct[OFF_SESSION] = 0;
    tick();

    sim_ui[2] = 8;                                   /* standby: not on */
    press_event(15, 5);
    CHECK(!ramp_stock_mode(), "switched in standby");
}

static void t_announce_ramp(void)
{
    int i, seen_on = 0;
    printf("the announcement reports a ramp starting and ending (bit 2)\n");
    reset_device(0);
    tick();
    start_session(1, 150, 65, 30);
    for (i = 0; i < 3; i++) tick();
    CHECK(ramp_active(ST()) && last_pkt[0] == 0xbc && (last_pkt[8] & 0x04), "no 'ramp running' announcement at arm (flags %#x)", last_pkt[8]);
    seen_on = (last_pkt[8] & 0x04) != 0;
    for (i = 0; i < 400 * TPS && sim_struct[OFF_SESSION]; i++) tick();
    for (i = 0; i < 5; i++) tick();
    CHECK(seen_on && !ramp_active(ST()) && last_pkt[0] == 0xbc && !(last_pkt[8] & 0x04), "no 'ramp ended' announcement (flags %#x)", last_pkt[8]);
}

static void t_select_preset(void)
{
    struct run r;
    printf("an app chooses the built-in preset (0xBE): stored, announced, run; refused out of range / stock mode / off\n");
    reset_device(0);
    tick();
    ramp_marker_dispatch(RAMP_SELECT_MARKER, 5);                 /* Clouds */
    tick();
    CHECK(ramp_selected() == 5, "selected %d", ramp_selected());
    CHECK(last_pkt[0] == 0xbc && last_pkt[9] == 5, "not announced (preset byte %d)", last_pkt[9]);
    start_session(1, 150, 65, 30);
    r = run_to_end(300);
    CHECK(r.stages_seen == 4 && r.stage_temp[1] == 470 && r.stage_temp[4] == 520,
          "didn't run Clouds: %d stages, %d .. %d", r.stages_seen, r.stage_temp[1], r.stage_temp[4]);

    ramp_marker_dispatch(RAMP_SELECT_MARKER, DEV_PICK_COUNT);    /* out of range */
    CHECK(ramp_selected() == 5, "an out-of-range choice was stored (%d)", ramp_selected());

    ramp_picker_event(ST(), 15, 1, 15, 7, -1, 15, 9);             /* an open picker follows */
    ramp_marker_dispatch(RAMP_SELECT_MARKER, 1);
    CHECK(ST()->picker_sel == 1 && !ST()->picker_dirty && ramp_selected() == 1, "the open picker didn't follow");
    ramp_picker_event(ST(), 15, 1, 15, 7, -1, 15, 9);             /* close: no write */
    CHECK(ramp_selected() == 1, "closing the picker changed the choice");

    ramp_marker_dispatch(RAMP_MODE_MARKER, RAMP_MODE_STOCK);
    ramp_marker_dispatch(RAMP_SELECT_MARKER, 3);
    CHECK(ramp_selected() == 1, "chosen in stock mode");
    ramp_marker_dispatch(RAMP_MODE_MARKER, RAMP_MODE_RAMP);
    ramp_toggle_enabled();                                        /* ramps off */
    ramp_marker_dispatch(RAMP_SELECT_MARKER, 3);
    CHECK(ramp_selected() == 1, "chosen with ramps off");
    ramp_toggle_enabled();

    last_pkt[0] = 0;
    ramp_marker_dispatch(RAMP_OFFSET_MARKER, 7);                  /* the offset announces too */
    tick();
    CHECK(last_pkt[0] == 0xbc && (signed char)last_pkt[10] == 7, "offset change not announced");
}

static void t_rank_change(void)
{
    int i;
    printf("another app selects a different preset mid-ramp: a plain stock session of that preset\n");
    reset_device(0);
    tick();
    set16(sim_struct, (2 + TBL_CO_F) * 2, 480);
    set16(sim_struct, (2 + TBL_CO_C) * 2, 248);
    set16(sim_struct, (2 + TBL_CO_HOLD) * 2, 40);
    start_session(1, 150, 65, 30);
    for (i = 0; i < 60 * TPS; i++) tick();
    CHECK(ramp_active(ST()), "ramp not running");
    sim_struct[0x08] = 2;                         /* the 0xCC packet's rank byte */
    tick();
    CHECK(ST()->stage == 0, "still ramping on the old slot");
    CHECK(u16at(sim_struct, TBL_CO_F * 2) == 150, "trigger slot not restored: %d", u16at(sim_struct, TBL_CO_F * 2));
    CHECK(u16at(sim_struct, OFF_COUNTDOWN) == 40, "countdown %d, not the new preset's hold", u16at(sim_struct, OFF_COUNTDOWN));
    for (i = 0; i < 5 * TPS; i++) tick();
    CHECK(ST()->stage == 0 && sim_struct[OFF_SESSION] == 1, "re-armed or stopped");
    for (i = 0; i < 200 * TPS && sim_struct[OFF_SESSION]; i++) tick();
    CHECK(!sim_struct[OFF_SESSION] && u16at(sim_dab, 2) == 1 && slot_f() == 480,
          "the session should end the stock way at preset 2: session %d dabs %d", sim_struct[OFF_SESSION], u16at(sim_dab, 2));
}

static void t_foreign_sector(void)
{
    struct run r;
    printf("a store sector holding something else reads as erased, and the first save replaces it\n");
    reset_device(0);
    memset(flash, 0x00, sizeof flash);   /* no magic; every byte a would-be setting */
    cur()[RAMP_MODE_OFFSET] = RAMP_MODE_STOCK;
    tick();
    CHECK(ramp_enabled() && ramp_selected() == RAMP_DEFAULT_PRESET && ramp_offset() == 0 && !ramp_stock_mode(),
          "read a setting from a foreign sector: en %d sel %d ofs %d stock %d",
          ramp_enabled(), ramp_selected(), ramp_offset(), ramp_stock_mode());
    start_session(1, 150, 65, 30);
    r = run_to_end(300);
    CHECK(r.stages_seen == 4, "default preset didn't run: %d stages", r.stages_seen);
    ramp_marker_dispatch(RAMP_OFFSET_MARKER, 5);
    CHECK(u16at(cur(), 0) == RAMP_STORE_MAGIC && ramp_offset() == 5 && ramp_enabled(), "first save didn't make a clean store");
}

static void t_picker_timeout(void)
{
    volatile ramp_state_t *st;
    int i;
    printf("picker closes after 30 s idle, keeping the choice\n");
    reset_device(0);
    tick();
    st = ST();
    ramp_picker_event(st, 3, 1, 3, 2, 1, 7, 8);
    ramp_picker_event(st, 2, 1, 3, 2, 1, 7, 8);
    for (i = 0; i < 29 * TPS; i++) tick();
    CHECK(st->picker_on, "closed too early");
    for (i = 0; i < 2 * TPS; i++) tick();
    CHECK(!st->picker_on && sim_picker_closed == 1, "didn't time out (%d)", sim_picker_closed);
    CHECK(ramp_selected() == 3, "choice %d not kept", ramp_selected());
}

static void t_flash_writes(void)
{
    volatile ramp_state_t *st;
    printf("unchanged data costs no flash erase; the store carries a version byte\n");
    reset_device(0);
    tick();
    st = ST();
    upload(1, 1, 430, 221, 12);
    CHECK(erases == 1 && cur()[RAMP_VER_OFFSET] == RAMP_STORE_VERSION, "erases %d version %d", erases, cur()[RAMP_VER_OFFSET]);
    upload(1, 1, 430, 221, 12);
    CHECK(erases == 1, "re-upload erased again (%d)", erases);
    ramp_marker_dispatch(RAMP_OFFSET_MARKER, 5);
    ramp_marker_dispatch(RAMP_OFFSET_MARKER, 5);
    CHECK(erases == 2, "same offset erased again (%d)", erases);
    ramp_picker_event(st, 3, 1, 3, 2, 1, 7, 8);
    ramp_picker_event(st, 7, 1, 3, 2, 1, 7, 8);
    CHECK(erases == 2, "unchanged picker exit erased (%d)", erases);
    ramp_picker_event(st, 3, 1, 3, 2, 1, 7, 8);
    ramp_picker_event(st, 2, 1, 3, 2, 1, 7, 8);
    ramp_picker_event(st, 1, 1, 3, 2, 1, 7, 8);
    ramp_picker_event(st, 7, 1, 3, 2, 1, 7, 8);
    CHECK(erases == 2, "net-unchanged choice erased (%d)", erases);
}

int main(void)
{
    CHECK(sizeof(ramp_state_t) < sizeof sim_state, "state too big");
    t_default_preset(0);
    t_default_preset(1);
    t_flower_no_default();
    t_upload_precedence();
    t_unusable_store();
    t_offset();
    t_picker();
    t_stop_early();
    t_step();
    t_announce();
    t_garbage_ram();
    t_heat_cap();
    t_heat_cap_waits();
    t_save_held();
    t_app_request();
    t_stock_mode();
    t_foreign_sector();
    t_rank_change();
    t_power_cut();
    t_mode_gesture();
    t_announce_ramp();
    t_select_preset();
    t_picker_timeout();
    t_flash_writes();
    CHECK(write_too_long == 0, "%d flash writes longer than DEV_FLASH_WRITE_MAX", write_too_long);
    printf(fails ? "\n%d FAILED\n" : "\nALL HOST TESTS PASSED\n", fails);
    return fails != 0;
}
