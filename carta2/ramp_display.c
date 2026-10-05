/* ramp_display.c -- Carta 2 ramp screen.
 *
 * LAYOUT during a ramp (240 x 240):
 *
 *   y 20   LEFT  battery % + battery icon       RIGHT  stage goal / ramp max + unit
 *   y 44   LEFT  session time left (M:SS, large) RIGHT  live temperature (large)
 *   y 72-196     LEFT  the chart, x 6-219, framed by a left axis, a baseline and a
 *                      right axis:
 *                        arc   -- the goal: the ramp's planned temperature over
 *                                 time, a smooth line through each stage's start,
 *                                 coloured by heat
 *                        trace -- the measured temperature since the ramp began,
 *                                 one sample per column, white; its newest point
 *                                 bounces with the live reading, and stepping
 *                                 back a stage (-) rewinds it
 *                RIGHT the heat meter, x 227-233: 10 circles, blue at the bottom
 *                      to red at the top, lit up to the live temperature
 *   y 202-224    LEFT  the Terpline flame mark + wordmark (logo_strip.h)
 *                RIGHT "DABS" + the dab count, right-aligned: the same per-mode
 *                      counter the stock screen shows (flower +0 / concentrate
 *                      +2 at 0x8430e0, chosen by mode the way d048 does)
 * The stock bottom row (y 193-225: dab counter, mode / status icons, READY
 * banner) is hidden during a ramp, so the chart and logo can use it.
 *
 * Both the chart's height and the meter use this ramp's own range (its
 * coolest stage minus a margin, up to its hottest), never an absolute degree
 * scale, so every ramp fills the box the same way.
 *
 * HOOKS. Stock elements whose space this layout uses are hidden only while a
 * ramp is active, at every one of their call sites:
 *   d3c0 (live temp + gauge)  fa38 (in fa1c) / f546 (view 13)  -> draws this UI
 *   dcac (session countdown)  fa3c (in fa1c) / f39c (views 8, 15, every second)
 *                                                               -> draws this UI
 *   ce70 (target), cf58 (slot hold), db40 (battery),
 *   d048 / e42c / e300 / e2b4 (the bottom row)        -- every caller -> hidden
 * The fa1c sites always follow a full screen clear, so they redraw everything;
 * the view sites update incrementally. When no ramp is active every hook calls
 * the original, unchanged.
 *
 * PRIMITIVES (decompiled): fill_rect FUN_000074d0 and blit FUN_00007e2c both
 * draw (w+1) x (h+1) px, opaquely; blit writes set bits black and clear bits in
 * the colour, unless BOTH colour bytes are exactly 0xA5 (then a gradient table),
 * which no colour here is. Glyph pointers are runtime flash addresses.
 */
#include "ramp.h"
#include "logo_strip.h"

typedef void (*fill_rect_fn)(u8 x, u8 y, u8 w, u8 h, u8 hi, u8 lo);
typedef void (*blit_fn)(u8 x, u8 y, u8 w, u8 h, u8 hi, u8 lo, const void *glyph);
typedef void (*batt_anim_fn)(int x, int y, int w, int h, int a, int b, int c, int d, int e, int f);
#define fill_rect   STOCK_FN(fill_rect_fn, 0x74d0)
#define blit        STOCK_FN(blit_fn, 0x7e2c)
#define batt_anim   STOCK_FN(batt_anim_fn, 0x7a24)   /* db40's charging animation */
#define orig_d3c0   STOCK_FN(void_fn, 0xd3c0)
#define orig_dcac   STOCK_FN(void_fn, 0xdcac)
#define orig_ce70   STOCK_FN(void_fn, 0xce70)
#define orig_cf58   STOCK_FN(void_fn, 0xcf58)
#define orig_db40   STOCK_FN(void_fn, 0xdb40)
#define orig_d048   STOCK_FN(void_fn, 0xd048)   /* bottom row: dab counter */
#define orig_e42c   STOCK_FN(void_fn, 0xe42c)   /*   mode icon */
#define orig_e300   STOCK_FN(void_fn, 0xe300)   /*   status icon */
#define orig_e2b4   STOCK_FN(void_fn, 0xe2b4)   /*   READY banner */

/* stock data, as db40 reads it */
#define BATTERY_PCT   (*(volatile u8 *)(0x84309c + 0x14))
#define BATTERY_LOW   (*(volatile u8 *)(0x84309c + 0x11))   /* low-power mode */
#define CHARGING      (*(volatile u8 *)0x843194)

/* fonts / bitmaps */
#define SMALL      ((const u8 *)0x1e6cc)   /* 0x22/glyph, 10 x 17 */
#define LARGE      ((const u8 *)0x21600)   /* 0x30/glyph, 16 x 24 */
#define BATT_ICON  ((const u8 *)0x21870)   /* 0x44/icon, 30 x 17 */
#define G_PCT      11
#define G_DEG_C    12                      /* + 1 = degrees F */

#define C_BLACK  0x0000
#define C_INK    0xFFFF
#define C_TIME   0xAD97   /* rgb(173,178,189) */
#define C_LOW    0xFE40   /* db40's own low-battery colour */
#define C_AXIS   0x4A69   /* rgb(72,76,72) */
#define C_DOT_OFF 0x18E3  /* unlit meter circle, rgb(24,28,24) */

#define ROW_A    20
#define ROW_B    44
#define SCREEN_Y1 225                       /* last row the ramp screen owns */

/* Below the header the ramp screen owns y 70-225: the original chart box plus
 * the stock bottom row (y 193-225, hidden during a ramp). 225 is the lowest row
 * the stock screen itself draws, so nothing here goes below it. */
#define CLEAR_Y  70
#define AX_L     6                          /* left axis */
#define CX0      8                          /* first chart column */
#define AX_R     (CX0 + RAMP_TRACE_LEN + 1) /* right axis, x 219 */
#define PY_TOP   74                         /* hottest plotted temperature */
#define PY_BOT   193                        /* coolest plotted temperature */
#define BASE_Y   196                        /* baseline */
#define AX_TOP   72                         /* top of both axes = top circle */
#define MTR_X    226                        /* heat meter: right edge x 233, as the text */
#define MTR_N    10
#define MTR_PITCH 13
#define MTR_Y0   (BASE_Y - 7)               /* 8 px circles: bottom y 189-196, top 72-79,
                                             * so the meter spans exactly the chart frame */

static void rect(int x, int y, int w, int h, u16 c)
{
    if (w >= 1 && h >= 1)
        fill_rect((u8)x, (u8)y, (u8)(w - 1), (u8)(h - 1), (u8)(c >> 8), (u8)c);
}
static void small(int x, int y, int d, u16 c) { blit((u8)x, (u8)y, 9, 16, (u8)(c >> 8), (u8)c, SMALL + d * 0x22); }
static void large(int x, int y, int d, u16 c) { blit((u8)x, (u8)y, 15, 23, (u8)(c >> 8), (u8)c, LARGE + d * 0x30); }
static u16 udiv(int a, int b) { return (u16)rom_div(a, b); }
/* hundreds / tens / ones without a hardware divide */
static void split3(u16 v, int *h, int *t, int *o)
{
    *h = udiv(v, 100); v -= *h * 100;
    *t = udiv(v, 10);  *o = v - *t * 10;
}

/* ---- colour --------------------------------------------------------------- */
static const u8 STOP_R[5] = { 7, 17, 27, 31, 31 };
static const u8 STOP_G[5] = { 22, 17, 15, 30, 53 };
static const u8 STOP_B[5] = { 31, 28, 20,  7,  7 };

static u16 heat(int frac)
{
    int x = frac * 4, seg = x >> 8, t = x & 0xff, r, g, b;
    if (seg > 3) { seg = 3; t = 255; }
    r = STOP_R[seg] + (((STOP_R[seg + 1] - STOP_R[seg]) * t) >> 8);
    g = STOP_G[seg] + (((STOP_G[seg + 1] - STOP_G[seg]) * t) >> 8);
    b = STOP_B[seg] + (((STOP_B[seg + 1] - STOP_B[seg]) * t) >> 8);
    return (u16)((r << 11) | (g << 5) | b);
}

/* heat meter, bottom to top: blue, blue, green, green, yellow, yellow,
 * orange, orange, red, red */
static const u16 MTR_C[MTR_N] = {
    0x051C, 0x051C, 0x25A8, 0x25A8, 0xFF20, 0xFF20, 0xFC03, 0xFC03, 0xE0E3, 0xE0E3
};

/* ---- ramp geometry (F; unit-independent) ----------------------------------
 * The plotted range is this ramp's own: from its coolest stage minus a margin
 * (so the climb from the start is visible) to its hottest plus a little
 * headroom -- never pinned to an absolute degree scale. */
typedef struct { int lo, hi; } scale_t;

static void ramp_scale(volatile ramp_state_t *st, scale_t *s)
{
    int mx = 0, mn = 0x7fff, i, span;
    for (i = 0; i < st->n_stages; i++) {
        int f = WP_F(st, i);
        if (f > mx) mx = f;
        if (f < mn) mn = f;
    }
    span = mx - mn;
    s->lo = mn - (span ? (span >> 1) + 15 : 40);
    s->hi = mx + 5;
}
/* 0-255 position of a temperature within the plotted range, clamped */
static u16 temp_frac(scale_t *s, int f)
{
    if (f <= s->lo) return 0;
    if (f >= s->hi) return 255;
    return udiv((f - s->lo) * 255, s->hi - s->lo);
}
static int temp_y(scale_t *s, int f)
{
    return PY_BOT - udiv(temp_frac(s, f) * (PY_BOT - PY_TOP), 255);
}

/* The goal at chart column c: a smooth line through (start of stage i, its
 * target) for each stage, flat at the last target to the end of the ramp. */
static int goal_f(volatile ramp_state_t *st, int c)
{
    int t = udiv(c * st->total_s, RAMP_TRACE_LEN - 1);
    int a = 0, b, i, fa, fb;
    for (i = 0; i < st->n_stages; i++) {
        fa = WP_F(st, i);
        if (i + 1 == st->n_stages)
            return fa;
        b = a + WP_HOLD(st, i);
        if (t <= b) {
            fb = WP_F(st, i + 1);
            if (b <= a)
                return fa;
            /* the divider is unsigned: a cooler next stage is handled by sign */
            return fb >= fa ? fa + udiv((fb - fa) * (t - a), b - a)
                            : fa - udiv((fa - fb) * (t - a), b - a);
        }
        a = b;
    }
    return WP_F(st, 0);
}

/* the chart column for "now" */
static int now_col(volatile ramp_state_t *st)
{
    int c = udiv(ramp_elapsed(st) * (RAMP_TRACE_LEN - 1), st->total_s);
    return c > RAMP_TRACE_LEN - 1 ? RAMP_TRACE_LEN - 1 : c;
}

/* ---- left column ---------------------------------------------------------- */

/* "68%" then the icon 5 px after the '%' (x 49, or x 61 at 100%). The row is
 * cleared whenever the value changes, since 100% shifts everything right. */
static void draw_battery(volatile ramp_state_t *st, u8 force)
{
    u8 v = BATTERY_PCT;
    int h, t, o, x = 6, icon_x;
    if (!force && v == st->drawn_batt)
        return;
    split3(v, &h, &t, &o);
    rect(6, ROW_A, 86, 17, C_BLACK);
    if (v > 99) { small(x, ROW_A, h, C_INK); x += 12; }
    if (v > 9)  { small(x, ROW_A, t, C_INK); x += 12; }
    small(x, ROW_A, o, C_INK); x += 12;
    blit(x, ROW_A, 13, 16, 0xff, 0xff, SMALL + G_PCT * 0x22);
    icon_x = x + 14 + 5;
    if (CHARGING)
        batt_anim(icon_x, ROW_A, 0x1d, 0x10, 6, 7, 0xff, 0xff, 0x36, 0xa9);
    else {
        u16 c = (BATTERY_LOW == 1 || v < 21) ? C_LOW : C_INK;
        int icon = v > 3 ? udiv(v - 1, 20) + 1 : 0;
        blit(icon_x, ROW_A, 0x1d, 0x10, (u8)(c >> 8), (u8)c, BATT_ICON + icon * 0x44);
    }
    st->drawn_batt = v;
}

/* M:SS in the large font, counting down from the ramp's total; cleared only
 * when the minute width changes */
static void draw_time(volatile ramp_state_t *st, u8 force)
{
    u16 left = st->last_left, m, s;
    int x = 6;
    if (!force && left == st->drawn_left)
        return;
    m = udiv(left, 60);
    s = left - m * 60;
    if (m > 99) m = 99;
    if (force || (m >= 10) != (udiv(st->drawn_left, 60) >= 10))
        rect(6, ROW_B, 84, 24, C_BLACK);
    {
        int mh, mt, mo, sh, stn, so;
        split3(m, &mh, &mt, &mo);
        split3(s, &sh, &stn, &so);
        if (m >= 10) { large(x, ROW_B, mt, C_TIME); x += 18; }
        large(x, ROW_B, mo, C_TIME);       x += 18;
        rect(x, ROW_B + 6, 3, 3, C_TIME);  rect(x, ROW_B + 15, 3, 3, C_TIME); x += 6;
        large(x, ROW_B, stn, C_TIME);      x += 18;
        large(x, ROW_B, so, C_TIME);
    }
    st->drawn_left = left;
}

/* ---- right column (right-aligned to x = 233) -------------------------------- */

static void digits3(int x, u16 v, u16 c)
{
    int h, t, o;
    if (v > 999) v = 999;
    split3(v, &h, &t, &o);
    small(x, ROW_A, h, c);
    small(x + 12, ROW_A, t, c);
    small(x + 24, ROW_A, o, c);
}

/* "goal/max": the current stage's target, in its heat colour, then the
 * ramp's highest target. Every valid ramp temperature is 3 digits in either
 * unit (flower 135-260 C / 275-500 F, concentrate 185-335 C / 365-635 F), so
 * the positions are fixed. */
static void draw_goal(volatile ramp_state_t *st, scale_t *s, u8 force)
{
    u8 f = DEV_SCALE_IS_F();
    int i = st->stage - 1, k;
    u16 goal = f ? WP_F(st, i) : WP_C(st, i), mx = 0;
    if (!force && goal == st->drawn_target)
        return;
    for (k = 0; k < st->n_stages; k++) {
        u16 v = f ? WP_F(st, k) : WP_C(st, k);
        if (v > mx) mx = v;
    }
    rect(130, ROW_A, 104, 17, C_BLACK);
    digits3(136, goal, heat(temp_frac(s, WP_F(st, i))));
    for (k = 0; k < 6; k++)                       /* '/' */
        rect(173 + k, ROW_A + 14 - k * 3, 2, 3, C_TIME);
    digits3(183, mx, C_TIME);
    blit(219, ROW_A, 14, 16, 0xff, 0xff, SMALL + (G_DEG_C + (f ? 1 : 0)) * 0x22);
    st->drawn_target = goal;
}

static void draw_live(volatile ramp_state_t *st, u8 force)
{
    u16 v = FIELD16(DEV_SCALE_IS_F() ? OFF_MEAS_F : OFF_MEAS_C);
    if (!force && v == st->drawn_hero)
        return;
    int h, t, o;
    if (v > 999) v = 999;
    split3(v, &h, &t, &o);
    if (v > 99) large(164, ROW_B, h, C_INK); else rect(164, ROW_B, 16, 24, C_BLACK);
    if (v > 9)  large(182, ROW_B, t, C_INK); else rect(182, ROW_B, 16, 24, C_BLACK);
    large(200, ROW_B, o, C_INK);
    blit(219, ROW_B + 7, 14, 16, 0xff, 0xff, SMALL + (G_DEG_C + (DEV_SCALE_IS_F() ? 1 : 0)) * 0x22);
    st->drawn_hero = v;
}

/* ---- the chart -------------------------------------------------------------- */

/* a vertical run joining y0 and y1 in one column, 2 px thick */
static void join(int x, int y0, int y1, u16 c)
{
    int a = y0 < y1 ? y0 : y1, b = y0 < y1 ? y1 : y0;
    rect(x, a, 1, b - a + 2, c);
}

/* Redraws chart column c on its own: clear, the goal arc, then the trace if
 * it has reached this column. Any column can be redrawn alone, which is what
 * lets the trace's newest point bounce without disturbing the rest. */
static void draw_column(volatile ramp_state_t *st, scale_t *s, int c)
{
    int x = CX0 + c, f = goal_f(st, c), y = temp_y(s, f);
    int yp = c ? temp_y(s, goal_f(st, c - 1)) : y;
    rect(x, PY_TOP, 1, PY_BOT - PY_TOP + 2, C_BLACK);
    join(x, yp, y, heat(temp_frac(s, f)));
    if (c < st->trace_n) {
        int ty = st->trace[c], tp = c ? st->trace[c - 1] : ty;
        join(x, tp, ty, C_INK);
    }
}

static void draw_chart(volatile ramp_state_t *st, scale_t *s)
{
    int c;
    rect(0, CLEAR_Y, 240, SCREEN_Y1 - CLEAR_Y + 1, C_BLACK);
    rect(AX_L, AX_TOP, 1, BASE_Y - AX_TOP + 1, C_AXIS);
    rect(AX_R, AX_TOP, 1, BASE_Y - AX_TOP + 1, C_AXIS);
    rect(AX_L, BASE_Y, AX_R - AX_L + 1, 1, C_AXIS);
    for (c = 0; c < RAMP_TRACE_LEN; c++)
        draw_column(st, s, c);
    for (c = 0; c < LOGO_RUNS; c++)                /* the logo: one rect per run */
        rect(LOGO_RUN[c][0], LOGO_Y + (LOGO_RUN[c][2] >> 3), LOGO_RUN[c][1], 1,
             LOGO_PAL[LOGO_RUN[c][2] & 7]);
}

/* Records the trace up to "now" and redraws only what changed. Stepping back
 * a stage moves "now" left: the trace is cut back to it and those columns
 * redrawn, so the line visibly rewinds with the ramp. */
static void update_trace(volatile ramp_state_t *st, scale_t *s)
{
    int now = now_col(st), y = temp_y(s, FIELD16(OFF_MEAS_F)), c;

    if (st->trace_n > now + 1) {
        int old = st->trace_n;
        st->trace_n = (u8)(now + 1);
        for (c = now + 1; c < old; c++)
            draw_column(st, s, c);
    }
    while (st->trace_n <= now) {
        st->trace[st->trace_n] = (u8)y;
        st->trace_n++;
        draw_column(st, s, st->trace_n - 1);
    }
    if (st->trace[now] != y) {                     /* the newest point bounces */
        st->trace[now] = (u8)y;
        draw_column(st, s, now);
    }
}

/* ---- the dab counter --------------------------------------------------------- */

#define DAB_Y    (LOGO_Y + 3)               /* small digits, y 205-221 */
#define LABEL_Y  (DAB_Y + 10)               /* "DABS" bottom = the digits' bottom row, y 221 */

static void draw_dabs(volatile ramp_state_t *st, u8 force)
{
    volatile u8 *d = (volatile u8 *)DEV_DAB_BASE;
    u16 v = *(volatile u16 *)(d + (DEV_MODE_IS_CONC() ? 2 : 0)), q, rest;
    int digit[5], n = 0, x, i;
    if (!force && v == st->drawn_dabs)
        return;
    rest = v;
    do {                                    /* up to 5 digits, as the stock counter */
        q = udiv(rest, 10);
        digit[n++] = rest - q * 10;
        rest = q;
    } while (rest && n < 5);
    rect(LOGO_W + 4, LOGO_Y, 234 - (LOGO_W + 4), LOGO_H, C_BLACK);
    x = 234 - n * 12;                       /* last digit ends at x 232 */
    for (i = n - 1; i >= 0; i--, x += 12)
        small(x, DAB_Y, digit[i], C_INK);
    x = 234 - n * 12 - 6 - LABEL_W;
    for (i = 0; i < LABEL_RUNS; i++)
        rect(x + LABEL_RUN[i][0], LABEL_Y + LABEL_RUN[i][2], LABEL_RUN[i][1], 1, C_TIME);
    st->drawn_dabs = v;
}

/* ---- the heat meter --------------------------------------------------------- */

/* an 8 px circle from three rects */
static void dot(int x, int y, u16 c)
{
    rect(x + 2, y, 4, 8, c);
    rect(x, y + 2, 8, 4, c);
    rect(x + 1, y + 1, 6, 6, c);
}

static void draw_meter(volatile ramp_state_t *st, scale_t *s, u8 force)
{
    int lit = 1 + udiv(temp_frac(s, FIELD16(OFF_MEAS_F)) * (MTR_N - 1) + 127, 255), i;
    if (!force && lit == st->drawn_fill)
        return;
    for (i = 0; i < MTR_N; i++)
        dot(MTR_X, MTR_Y0 - i * MTR_PITCH, i < lit ? MTR_C[i] : C_DOT_OFF);
    st->drawn_fill = (u8)lit;
}

static void ramp_draw(volatile ramp_state_t *st)
{
    scale_t s;
    u8 full = !st->frame_drawn;
    ramp_scale(st, &s);

    if (full)
        rect(0, ROW_A, 240, SCREEN_Y1 - ROW_A + 1, C_BLACK);
    draw_battery(st, full);
    draw_time(st, full);
    draw_goal(st, &s, full);
    draw_live(st, full);
    if (full)
        draw_chart(st, &s);
    update_trace(st, &s);
    draw_meter(st, &s, full);
    draw_dabs(st, full);
    st->frame_drawn = 1;
}

/* ---- hooks -------------------------------------------------------------------- */

void ramp_d3c0_full(void)
{
    volatile ramp_state_t *st = RAMP_STATE;
    if (!ramp_active(st)) { orig_d3c0(); return; }
    st->frame_drawn = 0;            /* fa1c cleared the screen first */
    ramp_draw(st);
}
void ramp_d3c0_view(void)
{
    volatile ramp_state_t *st = RAMP_STATE;
    if (!ramp_active(st)) { orig_d3c0(); return; }
    ramp_draw(st);
}
void ramp_dcac_full(void)
{
    if (!ramp_active(RAMP_STATE)) orig_dcac();
}
void ramp_dcac_view(void)
{
    volatile ramp_state_t *st = RAMP_STATE;
    if (!ramp_active(st)) { orig_dcac(); return; }
    ramp_draw(st);
}
/* Preset picker overlay (ramp_input.c): a black box over the stock target
 * line, showing the chosen preset's number and a row of six markers. Drawn from
 * the 0xce70 hook, which the idle screen calls every frame, so it's repainted
 * each frame while the picker is open. */
#define PICK_X  40
#define PICK_Y  98
#define PICK_W  160
#define PICK_H  36
void ramp_picker_draw(u8 sel)
{
    int i;
    rect(PICK_X, PICK_Y, PICK_W, PICK_H, C_BLACK);
    small(PICK_X + 14, PICK_Y + 10, sel + 1, C_INK);
    for (i = 0; i < DEV_PICK_COUNT; i++)
        rect(PICK_X + 40 + i * 18, PICK_Y + 14, 12, 8, i == sel ? C_INK : C_DOT_OFF);
}

void ramp_ce70_hide(void)
{
    volatile ramp_state_t *st = RAMP_STATE;
    if (ramp_active(st))
        return;
    orig_ce70();
    if (st->picker_on && STRUCT_BASE[OFF_SCREEN] == 0)
        ramp_picker_draw(st->picker_sel);
}
void ramp_cf58_hide(void) { if (!ramp_active(RAMP_STATE)) orig_cf58(); }
void ramp_db40_hide(void) { if (!ramp_active(RAMP_STATE)) orig_db40(); }
void ramp_d048_hide(void) { if (!ramp_active(RAMP_STATE)) orig_d048(); }
void ramp_e42c_hide(void) { if (!ramp_active(RAMP_STATE)) orig_e42c(); }
void ramp_e300_hide(void) { if (!ramp_active(RAMP_STATE)) orig_e300(); }
void ramp_e2b4_hide(void) { if (!ramp_active(RAMP_STATE)) orig_e2b4(); }
