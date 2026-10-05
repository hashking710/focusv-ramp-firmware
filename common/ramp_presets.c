/* ramp_presets.c -- Terpline's built-in concentrate ramps, shared by every
 * device. Values mirror terpline-web's lib/protocol/hardwareRampPresets.ts:
 * every profile starts at 440-480 F, peaks at 490-520 F, and has 4-5 stages,
 * so each one can count a dab (stage 2 and 20 s at temperature; see ramp.h).
 *
 * Used only when a concentrate session starts from the sentinel with no
 * uploaded waypoints saved -- an upload always takes precedence. The setup
 * offset (-10..+15 F, stored by the app) shifts every stage and is clamped back
 * into 440-520 F; stages the clamp makes equal are merged, as the app does. */
#include "ramp.h"

#define PRESET_MIN_F  440
#define PRESET_MAX_F  520
#define PRESET_COUNT  6
#define PRESET_STAGES 5

static const u16 PRESET_F[PRESET_COUNT][PRESET_STAGES] = {
    { 440, 455, 470, 490, 0 },        /* flavor first */
    { 450, 465, 480, 495, 0 },        /* rosin / solventless */
    { 455, 470, 485, 505, 0 },        /* balanced */
    { 460, 475, 495, 510, 0 },        /* sauce / badder */
    { 445, 460, 475, 490, 505 },      /* long session */
    { 470, 490, 505, 520, 0 },        /* clouds */
};
static const u8 PRESET_HOLD[PRESET_COUNT][PRESET_STAGES] = {
    { 35, 25, 25, 20, 0 },
    { 35, 30, 25, 20, 0 },
    { 30, 25, 25, 20, 0 },
    { 25, 25, 20, 15, 0 },
    { 30, 25, 25, 20, 15 },
    { 20, 20, 15, 15, 0 },
};
static const u8 PRESET_N[PRESET_COUNT] = { 4, 4, 4, 4, 5, 4 };

static int clamp_int(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

u8 ramp_default_stages(volatile ramp_state_t *st, u8 sel)
{
    int off = ramp_offset();
    u8 n = 0, i;

    if (sel >= PRESET_COUNT)
        sel = RAMP_DEFAULT_PRESET;

    for (i = 0; i < PRESET_N[sel]; i++) {
        int f = clamp_int((int)PRESET_F[sel][i] + off, PRESET_MIN_F, PRESET_MAX_F);

        if (n != 0 && f == (int)st->wp[n - 1][0]) {
            st->wp[n - 1][2] = (u16)(st->wp[n - 1][2] + PRESET_HOLD[sel][i]);
            continue;
        }
        st->wp[n][0] = (u16)f;
        st->wp[n][1] = (u16)rom_div(5 * (f - 32) + 4, 9);   /* F -> C, rounded */
        st->wp[n][2] = PRESET_HOLD[sel][i];
        n++;
    }
    return n;
}
