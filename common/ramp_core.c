/* ramp_core.c -- ramp sequencer, shared by every device. See ramp.h for the
 * design; device.h supplies each device's confirmed addresses and offsets.
 *
 * Installed by a same-length call-site swap of the stock per-tick orchestrator
 * call (DEV_PID_TICK) -> ramp_trampoline, which runs the stock tick unchanged
 * and then the ramp.
 */
#include "ramp.h"

/* ---- the active preset slot ------------------------------------------- */

static void write_slot(volatile ramp_state_t *st, u16 f, u16 c)
{
    *PRESET(st->bank ? TBL_CO_F : TBL_FL_F, st->rank) = f;
    *PRESET(st->bank ? TBL_CO_C : TBL_FL_C, st->rank) = c;
}

static void apply_stage(volatile ramp_state_t *st, u8 stage)
{
    write_slot(st, WP_F(st, stage - 1), WP_C(st, stage - 1));
    STRUCT_BASE[OFF_REACHED] = 0;   /* stock re-targets from the slot */
    st->stage = stage;
    st->heat_s = 0;
}

/* ---- dab counting --------------------------------------------------------
 * Exactly the stock completion bookkeeping (the counter increments the stock
 * session timer performs when a session finishes), minus the stop and end cue.
 * Flower +0,+4,+6,+10,+14; concentrate +2,+4,+8,+12,+16 -- the same on every
 * device, read from each one's own timer routine. The counters change in RAM
 * at once (what the app reads); the stock save arming that persists them is
 * deferred to disarm, after the preset slot holds its own values again, so a
 * save can never write the ramp's temporary stage temperature to flash. */
static void bump(volatile u8 *base, u8 off)
{
    volatile u16 *p = (volatile u16 *)(base + off);
    *p = (u16)(*p + 1);
}

static void count_dab(volatile ramp_state_t *st)
{
    volatile u8 *d = (volatile u8 *)DEV_DAB_BASE;
    if (st->bank == 0) {
        bump(d, 0); bump(d, 4); bump(d, 6); bump(d, 10); bump(d, 14);
    } else {
        bump(d, 2); bump(d, 4); bump(d, 8); bump(d, 12); bump(d, 16);
    }
    st->counted = 1;   /* the save is armed in disarm, once the slot is restored */
}

/* A multi-stage ramp ends here, one second before the stock timer would end it
 * (which would run the completion bookkeeping a second time). */
static void end_ramp(void)
{
    volatile u8 *cue = (volatile u8 *)DEV_END_CUE;
    stock_stop();
    cue[3] = 5;
    cue[4] = 2;
}

/* ---- arming --------------------------------------------------------------- */

/* A waypoint read back from flash is only used if it is a temperature the
 * stock handler could have stored: in range, and its F and C agreeing (to about
 * 2 C) the way the firmware's own conversion leaves them. Corrupted store bytes
 * essentially never pass both, and an unusable store never arms -- the session
 * just runs as an ordinary stock session. */
static u8 wp_sane(u8 bank, u16 f, u16 c, u16 hold)
{
    int d = 9 * (int)c - 5 * ((int)f - 32);
    u16 lo = bank ? RAMP_CO_MIN_F : RAMP_FL_MIN_F;
    u16 hi = bank ? RAMP_CO_MAX_F : RAMP_FL_MAX_F;
    return f >= lo && f <= hi && d >= -18 && d <= 18 && hold <= RAMP_MAX_HOLD;
}

#define STORE_UNUSABLE 0xff

/* Loads the bank's waypoints from the store into st->wp (via the stock SPI
 * read) and returns how many stages it holds: the prefix up to the first empty
 * waypoint. 0 means nothing is saved for this bank (the defaults apply); STORE_
 * UNUSABLE means something is saved but a stage fails the check, and the
 * session must not arm at all. */
static u8 load_stages(volatile ramp_state_t *st, u8 bank)
{
    u16 buf[RAMP_STORE_SIZE / 2];
    u16 *w = buf + 1 + bank * RAMP_NUM_SLOTS * 3;
    u8 n = 0, i, k;

    flash_read(DEV_RAMP_FLASH, RAMP_STORE_SIZE, buf);
    if (buf[0] != RAMP_STORE_MAGIC)
        return 0;
    while (n < RAMP_NUM_SLOTS) {
        u16 h = w[n * 3 + 2];
        if (h == 0 || h == 0xffff)
            break;
        if (!wp_sane(bank, w[n * 3], w[n * 3 + 1], h))
            return STORE_UNUSABLE;
        n++;
    }
    for (i = 0; i < n; i++)
        for (k = 0; k < 3; k++)
            st->wp[i][k] = w[i * 3 + k];
    return n;
}

/* A running state must be internally consistent before it's acted on. */
static u8 state_sane(volatile ramp_state_t *st)
{
    return st->n_stages >= 1 && st->n_stages <= RAMP_NUM_SLOTS &&
           st->stage >= 1 && st->stage <= st->n_stages &&
           st->bank <= 1 && st->rank <= RAMP_MAX_RANK && st->total_s != 0;
}

static void try_arm(volatile ramp_state_t *st)
{
    u8 bank = DEV_MODE_IS_CONC() ? 1 : 0;
    u8 rank = DEV_RANK(bank);
    u16 f, c;
    u16 total = 0;
    u8 i, n;

    if (rank > RAMP_MAX_RANK)
        return;
    f = *PRESET(bank ? TBL_CO_F : TBL_FL_F, rank);
    c = *PRESET(bank ? TBL_CO_C : TBL_FL_C, rank);
    if (!IS_SENTINEL(f, c) || st->arm_failed || !ramp_enabled())
        return;
    n = load_stages(st, bank);
    if (n == 0 && bank == 1)
        n = ramp_default_stages(st, ramp_selected());
    if (n == 0 || n == STORE_UNUSABLE) {
        st->arm_failed = 1;   /* nothing usable for this mode: an ordinary
                               * session; don't re-read flash every tick */
        return;
    }

    for (i = 0; i < n; i++)
        total += WP_HOLD(st, i);   /* <= 5 x RAMP_MAX_HOLD */

    st->bank = bank;
    st->rank = rank;
    st->n_stages = n;
    st->total_s = total;
    st->saved_f = f;
    st->saved_c = c;
    st->counted = 0;
    st->frame_drawn = 0;
    st->trace_n = 0;
    st->at_temp_s = 0;
    FIELD16(OFF_COUNTDOWN) = total;
    st->last_left = total;
    apply_stage(st, 1);
}

/* Restores the slot only when the state is provably one try_arm wrote: the
 * RAM it lives in isn't cleared at boot, so after a reset mid-ramp (or random
 * power-on contents that happen to match the magic) it must never write a
 * preset slot from unchecked fields. A ramp is only ever armed from a
 * sentinel slot, so a valid saved pair is always a sentinel. */
static void disarm(volatile ramp_state_t *st)
{
    if (st->bank <= 1 && st->rank <= RAMP_MAX_RANK && IS_SENTINEL(st->saved_f, st->saved_c)) {
        write_slot(st, st->saved_f, st->saved_c);
        if (st->counted) {
            volatile u8 *d = (volatile u8 *)DEV_DAB_BASE;
            DEV_SAVE_ARM(d);
        }
    }
    st->stage = 0;
    st->counted = 0;
}

/* ---- per tick ----------------------------------------------------------- */

static void ramp_tick(void)
{
    volatile ramp_state_t *st = RAMP_STATE;
    u16 t;
    u8 s;

    if (st->magic != RAMP_MAGIC) {
        st->magic = RAMP_MAGIC;
        st->stage = 0;
        st->arm_failed = 0;
        st->picker_on = 0;
        st->picker_dirty = 0;
        st->picker_enabled = 0;
        st->ann_tries = 0;
    }

    ramp_announce_tick(st);

    if (st->picker_on && DEV_SYS_TICK - st->picker_t0 > RAMP_PICKER_TIMEOUT) {
        ramp_picker_close(st);
        DEV_PICKER_CLOSED();
    }

    /* A session that starts while the picker is open belongs to the stock
     * code: close the picker, unsaved choice included. The button light is
     * handed back by the device's ramp_led.c on its next pass. */
    if (STRUCT_BASE[OFF_SESSION] != 0) {
        st->picker_on = 0;
        st->picker_dirty = 0;
    }

    if (STRUCT_BASE[OFF_SESSION] == 0) {
        if (st->stage != 0)
            disarm(st);
        st->arm_failed = 0;
        return;
    }

    if (st->stage == 0) {
        try_arm(st);
        return;
    }

    if (!state_sane(st)) {
        st->stage = 0;   /* not a state try_arm wrote: drop it, touch nothing */
        return;
    }

    /* atomizer swapped mid-ramp: hand the session back to stock */
    if ((DEV_MODE_IS_CONC() ? 1 : 0) != st->bank) {
        disarm(st);
        return;
    }

    /* The ramp owns the countdown while it runs. Between two ticks the stock
     * timer can only leave it alone or take one second off (it runs at 1 Hz;
     * this runs every main-loop pass). Any other change is stock code
     * reloading it from the slot's hold time (session start paths, BLE
     * handlers, first reach on Aeris/Sport, the Sport's +10 s) -- up or down.
     * Undo it, so the slot's hold never has to hold the ramp's length. */
    t = FIELD16(OFF_COUNTDOWN);
    if (t != st->last_left && t + 1 != st->last_left)
        FIELD16(OFF_COUNTDOWN) = t = st->last_left;
    /* One second went by on the stock clock. It counts toward the hold, and
     * toward the dab, only at temperature (the stock "reached" flag). On Aeris
     * and Sport the stock clock already waits for that; on the Carta 2 it runs
     * through heat-up, so the second is given back. After RAMP_MAX_HEAT_S of
     * heating a stage counts down anyway, so a stage that never reports
     * reached can't stall the ramp. */
    if (t + 1 == st->last_left) {
        if (STRUCT_BASE[OFF_REACHED]) {
            if (st->at_temp_s < 0xffff)
                st->at_temp_s++;
        } else if (st->heat_s < RAMP_MAX_HEAT_S) {
            st->heat_s++;
            FIELD16(OFF_COUNTDOWN) = t = st->last_left;
        }
    }
    st->last_left = t;

    if (t <= 1) {
        if (st->counted) {
            end_ramp();
            return;
        }
        /* A multi-stage ramp that never qualified for a dab (only possible if
         * it got no time at temperature) ends the same way, uncounted: the dab
         * rule is the patch's, not the stock completion's. */
        if (st->n_stages >= COUNT_MIN_STAGE) {
            end_ramp();
            return;
        }
        /* Single stage: the stock completion at 0 counts it and arms
         * the save. Give the slot its own values back first, so that save can
         * only ever see them. The heater target is unaffected -- stock reloads
         * it from the slot only while "reached" is 0, which, if it is, means
         * one second at the sentinel instead of the last stage's target. */
        write_slot(st, st->saved_f, st->saved_c);
        return;
    }

    t = ramp_elapsed(st);
    for (s = 1; s < st->n_stages; s++)
        if (t < stage_start(st, s + 1))
            break;
    if (s != st->stage)
        apply_stage(st, s);

    if (!st->counted && st->stage >= COUNT_MIN_STAGE && st->at_temp_s >= COUNT_AT_TEMP_S)
        count_dab(st);
}

void ramp_trampoline(void)
{
    orig_pid_tick();
    ramp_tick();
    DEV_AFTER_TICK(RAMP_STATE);
}

/* Jump to the next / previous stage (Carta 2 +/- buttons). */
void ramp_step(volatile ramp_state_t *st, int dir)
{
    int target = (int)st->stage + dir;
    if (!state_sane(st) || target < 1 || target > st->n_stages)
        return;
    st->last_left = (u16)(st->total_s - stage_start(st, (u8)target));
    FIELD16(OFF_COUNTDOWN) = st->last_left;
    apply_stage(st, (u8)target);
    if (!st->counted && st->stage >= COUNT_MIN_STAGE && st->at_temp_s >= COUNT_AT_TEMP_S)
        count_dab(st);
}
