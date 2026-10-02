/* ramp_display.c -- Carta 2 ramp screen.
 *
 * LAYOUT during a ramp (240 x 240):
 *
 *   y 20   LEFT  battery % + battery icon       RIGHT  end temp (last stage) + unit
 *   y 38                                         RIGHT  end-temp underline, end-stage colour
 *   y 44   LEFT  session time left (M:SS, large) RIGHT  live temperature (large)
 *   y 78-191     the equalizer, x 142-233: one bar per stage, height + colour both
 *                from that stage's temperature relative to this ramp's own
 *                coolest-to-hottest span (never pinned to an absolute 0 deg
 *                scale, so it reads consistently on a small screen regardless
 *                of the ramp's real temperatures) -- done = solid, in progress
 *                = rises from the baseline as the stage does, with a bright cap
 *                at its target; next = outline only. A white tick on the active
 *                bar marks the live measured temperature on that same scale.
 *                x 0-141 is deliberately empty: the equalizer reads as a small
 *                live instrument, not a full chart, so it doesn't need the rest
 *                of the row to stay legible.
 *   y 193+ STOCK dab counter + mode icon, or the READY banner once a stage's
 *                temperature is reached
 *
 * Bars normally grow stage over stage, since a ramp normally heats up; the
 * exception is the Carta 2's own - button, which steps back to a cooler
 * stage (ramp_input.c) and so is the one input that visibly makes the active
 * bar shorter than the one before it.
 *
 * HOOKS. Stock elements whose space this layout uses are hidden only while a
 * ramp is active, at every one of their call sites:
 *   d3c0 (live temp + gauge)  fa38 (in fa1c) / f546 (view 13)  -> draws this UI
 *   dcac (session countdown)  fa3c (in fa1c) / f39c (views 8, 15, every second)
 *                                                               -> draws this UI
 *   ce70 (target), cf58 (slot hold), db40 (battery)  -- every caller -> hidden
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
#define C_AXIS   0x2967

#define ROW_A    20
#define ROW_B    44
#define MID_Y1   192

/* the equalizer: x 142-233 (right-aligned to the same x = 233 as the target /
 * live digits above it), y 78-186 -- the exact pixel box the original full-
 * width chart proved safe (clear region 0,74,240,118 stays above the stock
 * dab-counter / READY-banner row at y >= 192, confirmed by decompile; see the
 * git history for how that boundary was found). Only the box is reused: the
 * bars inside it are new. */
#define EQ_X0    142
#define EQ_X1    233
#define GY_TOP   78
#define GY_BASE  186
#define EQ_GAP   4
#define MIN_H    14                      /* every bar's visible floor, even at 0 */
#define MAX_H    (GY_BASE - GY_TOP)       /* height of the hottest stage in this ramp */

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
static u16 dim(u16 c, int k)
{
    return (u16)(((((c >> 11) & 31) * k >> 8) << 11) | ((((c >> 5) & 63) * k >> 8) << 5) | ((c & 31) * k >> 8));
}

/* ---- ramp geometry (F; unit-independent) -----------------------------------
 * min_f/span are this ramp's OWN coolest-to-hottest stage, never an absolute
 * temperature scale -- every height and colour below is a 0-255 fraction of
 * that span, so two ramps with very different real temperatures still fill
 * the same pixel box the same way. */
typedef struct { int min_f, span; } scale_t;

static void ramp_scale(volatile ramp_state_t *st, scale_t *s)
{
    int mx = 0, mn = 0x7fff, i;
    for (i = 0; i < st->n_stages; i++) {
        int f = WP_F(st, i);
        if (f > mx) mx = f;
        if (f < mn) mn = f;
    }
    s->min_f = mn;
    s->span = mx - mn;
}
static u16 stage_frac(volatile ramp_state_t *st, scale_t *s, int i)
{
    return s->span ? udiv((WP_F(st, i) - s->min_f) * 255, s->span) : 255;
}
static int eq_bar_h(u16 frac255)
{
    return MIN_H + udiv((MAX_H - MIN_H) * frac255, 255);
}
/* n equal-width columns across EQ_X0..EQ_X1, same min-width guard as the
 * previous time-proportional layout (a column must never come out < 1 px
 * wide, which would make the rects below misbehave). */
static void eq_col_x(u8 n, int i, int *x0, int *w)
{
    int a = EQ_X0 + udiv((EQ_X1 - EQ_X0) * i, n);
    int b = EQ_X0 + udiv((EQ_X1 - EQ_X0) * (i + 1), n);
    *x0 = a;
    *w = b - a - EQ_GAP;
    if (*w < 2)
        *w = 2;
}

/* ---- left column ---------------------------------------------------------- */

static void draw_battery(volatile ramp_state_t *st, u8 force)
{
    u8 v = BATTERY_PCT;
    int h, t, o;
    if (!force && v == st->drawn_batt)
        return;
    split3(v, &h, &t, &o);
    if (v > 99) { small(6, ROW_A, h, C_INK); small(18, ROW_A, t, C_INK); small(30, ROW_A, o, C_INK); }
    else {
        /* clears the 100% layout's third digit AND its '%' (x 42-55), which
         * the 2-digit '%' at x 30-43 only partly covers */
        rect(30, ROW_A, 30, 17, C_BLACK);
        if (v > 9) small(6, ROW_A, t, C_INK); else rect(6, ROW_A, 10, 17, C_BLACK);
        small(18, ROW_A, o, C_INK);
    }
    blit(6 + (v > 99 ? 36 : 24), ROW_A, 13, 16, 0xff, 0xff, SMALL + G_PCT * 0x22);
    if (CHARGING)
        batt_anim(6 + 54, ROW_A, 0x1d, 0x10, 6, 7, 0xff, 0xff, 0x36, 0xa9);
    else {
        u16 c = (BATTERY_LOW == 1 || v < 21) ? C_LOW : C_INK;
        int icon = v > 3 ? udiv(v - 1, 20) + 1 : 0;
        blit(6 + 54, ROW_A, 0x1d, 0x10, (u8)(c >> 8), (u8)c, BATT_ICON + icon * 0x44);
    }
    st->drawn_batt = v;
}

/* M:SS in the large font; cleared only when the minute width changes */
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

/* The ramp's END temperature -- the last stage's target, constant for the
 * whole ramp -- not the current stage's (the equalizer shows that instead,
 * as the bar you're currently climbing). Lets you see at a glance where the
 * whole ramp finishes, next to the live reading right below it. */
static void draw_end(volatile ramp_state_t *st, scale_t *s, u8 force)
{
    u16 v = DEV_SCALE_IS_F() ? WP_F(st, st->n_stages - 1) : WP_C(st, st->n_stages - 1);
    if (!force && v == st->drawn_target)
        return;
    int h, t, o;
    if (v > 999) v = 999;
    split3(v, &h, &t, &o);
    if (v > 99) small(183, ROW_A, h, C_INK); else rect(183, ROW_A, 10, 17, C_BLACK);
    small(195, ROW_A, t, C_INK);
    small(207, ROW_A, o, C_INK);
    blit(219, ROW_A, 14, 16, 0xff, 0xff, SMALL + (G_DEG_C + (DEV_SCALE_IS_F() ? 1 : 0)) * 0x22);
    rect(183, 38, 51, 2, heat(stage_frac(st, s, st->n_stages - 1)));
    st->drawn_target = v;
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

/* ---- the equalizer ---------------------------------------------------------
 * One bar per stage, equal width, x 142-233. Height and colour both come from
 * stage_frac() -- this ramp's own relative heat, 0-255 -- never from an
 * absolute degree scale, so the box fills the same way regardless of the
 * ramp's real temperatures. Done stages are solid blocks; the active stage
 * rises from the baseline as it progresses, capped with a bright tick at its
 * own target; later stages are outlines only. */

static void draw_eq_current(volatile ramp_state_t *st, scale_t *s, u8 force)
{
    int i = st->stage - 1, x0, w, h, y, fill, my;
    u16 frac = stage_frac(st, s, i);
    u16 c = heat(frac), mfrac;
    u16 hold = WP_HOLD(st, i), el = ramp_elapsed(st), start = stage_start(st, st->stage);
    u16 into = el > start ? el - start : 0;
    int mf = FIELD16(OFF_MEAS_F);

    eq_col_x(st->n_stages, i, &x0, &w);
    h = eq_bar_h(frac);
    y = GY_BASE - h;
    fill = hold ? udiv(h * (into > hold ? hold : into), hold) : h;

    /* measured temperature, clamped onto this ramp's own 0-255 span -- the
     * same scale the bars themselves use, so the tick lands where it visually
     * belongs even when the live reading briefly overshoots a stage's target */
    if (mf <= s->min_f) mfrac = 0;
    else if (!s->span || mf >= s->min_f + s->span) mfrac = 255;
    else mfrac = udiv((mf - s->min_f) * 255, s->span);
    my = GY_BASE - eq_bar_h(mfrac);

    if (!force && fill == st->drawn_fill && my == st->drawn_meas_y)
        return;

    rect(x0, GY_TOP, w, GY_BASE - GY_TOP, C_BLACK);       /* this column only */
    rect(x0, GY_BASE - fill, w, fill, c);                 /* risen so far: solid */
    rect(x0, y + 2, w, h - fill - 2, dim(c, 80));          /* still to climb */
    rect(x0, y, w, 2, C_INK);                              /* this stage's target, drawn last */
    rect(x0, my - 1 < GY_TOP ? GY_TOP : my - 1, w, 3, C_INK);
    st->drawn_fill = (u8)fill;
    st->drawn_meas_y = (u8)my;
}

static void draw_eq_full(volatile ramp_state_t *st, scale_t *s)
{
    int i, x0, w, h, y;
    u8 n = st->n_stages, stage = st->stage;

    rect(0, GY_TOP - 4, 240, MID_Y1 - (GY_TOP - 4) + 1, C_BLACK);
    for (i = 0; i < n; i++) {
        u16 c = heat(stage_frac(st, s, i));
        eq_col_x(n, i, &x0, &w);
        h = eq_bar_h(stage_frac(st, s, i));
        y = GY_BASE - h;
        if (i + 1 < stage) {
            rect(x0, y, w, h, c);                 /* done: solid */
        } else if (i + 1 > stage) {
            rect(x0, y, w, 2, c);                  /* not yet reached: outline */
            rect(x0, y, 2, h, c);
            rect(x0 + w - 2, y, 2, h, c);
        }
        /* i + 1 == stage: drawn by draw_eq_current, called right after */
    }
    rect(EQ_X0, GY_BASE + 1, EQ_X1 - EQ_X0, 1, C_AXIS);
    draw_eq_current(st, s, 1);
    st->drawn_stage = stage;
}

static void ramp_draw(volatile ramp_state_t *st)
{
    scale_t s;
    u8 full = !st->frame_drawn;
    ramp_scale(st, &s);

    if (full)
        rect(0, ROW_A, 240, MID_Y1 - ROW_A + 1, C_BLACK);
    draw_battery(st, full);
    draw_time(st, full);
    draw_end(st, &s, full);
    draw_live(st, full);
    if (full || st->stage != st->drawn_stage)
        draw_eq_full(st, &s);
    else
        draw_eq_current(st, &s, 0);
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
void ramp_ce70_hide(void) { if (!ramp_active(RAMP_STATE)) orig_ce70(); }
void ramp_cf58_hide(void) { if (!ramp_active(RAMP_STATE)) orig_cf58(); }
void ramp_db40_hide(void) { if (!ramp_active(RAMP_STATE)) orig_db40(); }
