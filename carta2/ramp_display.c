/* ramp_display.c -- Carta 2 ramp screen.
 *
 * LAYOUT during a ramp (240 x 240):
 *
 *   y 20   LEFT  battery % + battery icon       RIGHT  current stage target + unit
 *   y 38                                         RIGHT  stage-coloured underline
 *   y 44   LEFT  session time left (M:SS, large) RIGHT  live temperature (large)
 *   y 78-191     the ramp: one column per stage (filled = done, part-filled =
 *                in progress, outlined = next), dashed line at the stage target,
 *                white line at the measured temperature, underline under the
 *                current stage
 *   y 193+ STOCK dab counter + mode icon, or the READY banner once a stage's
 *                temperature is reached
 *
 * Colour language: white = where you are; stage colour = where you're going.
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
#define GX0      6
#define GW       228
#define GAP      3
#define GY_TOP   78
#define GY_BASE  186
#define MID_Y1   192

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

/* ---- ramp geometry (F; unit-independent) --------------------------------- */
typedef struct { int lo, hi, min_f, span; } scale_t;

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
    s->lo = mn - (s->span ? (s->span >> 1) + 15 : 40);
    s->hi = mx + 5;
}
static int temp_y(scale_t *s, int f)
{
    int y;
    if (f <= s->lo) return GY_BASE;
    if (f >= s->hi) return GY_TOP;
    y = GY_BASE - udiv((f - s->lo) * (GY_BASE - GY_TOP), s->hi - s->lo);
    return y < GY_TOP ? GY_TOP : y;
}
static u16 stage_colour(volatile ramp_state_t *st, scale_t *s, int i)
{
    return s->span ? heat(udiv((WP_F(st, i) - s->min_f) * 255, s->span)) : heat(255);
}
static void col_x(volatile ramp_state_t *st, int i, int *x0, int *w)
{
    int a = GX0 + udiv(GW * stage_start(st, i + 1), st->total_s);
    int b = (i + 1 == st->n_stages) ? GX0 + GW : GX0 + udiv(GW * stage_start(st, i + 2), st->total_s);
    *x0 = a;
    *w = b - a - GAP;
    /* a stage that's a tiny share of the ramp still gets a visible column, and
     * never a width < 1 (which would make every rect / fill below misbehave);
     * the furthest it can then reach is GX0 + GW - 1 + 2 = 235 < 240 */
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
    if (v > 99) { small(GX0, ROW_A, h, C_INK); small(GX0 + 12, ROW_A, t, C_INK); small(GX0 + 24, ROW_A, o, C_INK); }
    else {
        /* clears the 100% layout's third digit AND its '%' (x 42-55), which
         * the 2-digit '%' at x 30-43 only partly covers */
        rect(GX0 + 24, ROW_A, 30, 17, C_BLACK);
        if (v > 9) small(GX0, ROW_A, t, C_INK); else rect(GX0, ROW_A, 10, 17, C_BLACK);
        small(GX0 + 12, ROW_A, o, C_INK);
    }
    blit(GX0 + (v > 99 ? 36 : 24), ROW_A, 13, 16, 0xff, 0xff, SMALL + G_PCT * 0x22);
    if (CHARGING)
        batt_anim(GX0 + 54, ROW_A, 0x1d, 0x10, 6, 7, 0xff, 0xff, 0x36, 0xa9);
    else {
        u16 c = (BATTERY_LOW == 1 || v < 21) ? C_LOW : C_INK;
        int icon = v > 3 ? udiv(v - 1, 20) + 1 : 0;
        blit(GX0 + 54, ROW_A, 0x1d, 0x10, (u8)(c >> 8), (u8)c, BATT_ICON + icon * 0x44);
    }
    st->drawn_batt = v;
}

/* M:SS in the large font; cleared only when the minute width changes */
static void draw_time(volatile ramp_state_t *st, u8 force)
{
    u16 left = st->last_left, m, s;
    int x = GX0;
    if (!force && left == st->drawn_left)
        return;
    m = udiv(left, 60);
    s = left - m * 60;
    if (m > 99) m = 99;
    if (force || (m >= 10) != (udiv(st->drawn_left, 60) >= 10))
        rect(GX0, ROW_B, 84, 24, C_BLACK);
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

static void draw_target(volatile ramp_state_t *st, scale_t *s, u8 force)
{
    u16 v = stage_target_display(st);
    if (!force && v == st->drawn_target)
        return;
    int h, t, o;
    if (v > 999) v = 999;
    split3(v, &h, &t, &o);
    if (v > 99) small(183, ROW_A, h, C_INK); else rect(183, ROW_A, 10, 17, C_BLACK);
    small(195, ROW_A, t, C_INK);
    small(207, ROW_A, o, C_INK);
    blit(219, ROW_A, 14, 16, 0xff, 0xff, SMALL + (G_DEG_C + (DEV_SCALE_IS_F() ? 1 : 0)) * 0x22);
    rect(183, 38, 51, 2, stage_colour(st, s, st->stage - 1));
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

/* ---- the ramp ------------------------------------------------------------- */

static void draw_current(volatile ramp_state_t *st, scale_t *s, u8 force)
{
    int i = st->stage - 1, x0, w, ty, fill, my;
    u16 c = stage_colour(st, s, i);
    u16 hold = WP_HOLD(st, i), el = ramp_elapsed(st), start = stage_start(st, st->stage);
    u16 into = el > start ? el - start : 0;

    col_x(st, i, &x0, &w);
    ty = temp_y(s, WP_F(st, i));
    fill = hold ? udiv(w * (into > hold ? hold : into), hold) : w;
    my = temp_y(s, FIELD16(OFF_MEAS_F));
    if (!force && fill == st->drawn_fill && my == st->drawn_meas_y)
        return;

    rect(x0, GY_TOP, w, GY_BASE - GY_TOP + 1, C_BLACK);
    rect(x0, ty, fill, GY_BASE - ty + 1, c);
    rect(x0 + fill, ty, w - fill, GY_BASE - ty + 1, dim(c, 80));
    rect(x0, my - 1 < GY_TOP ? GY_TOP : my - 1, w, 3, C_INK);
    st->drawn_fill = (u8)fill;
    st->drawn_meas_y = (u8)my;
}

static void draw_chart(volatile ramp_state_t *st, scale_t *s)
{
    int i, x0, w, ty, x;
    u16 cur = stage_colour(st, s, st->stage - 1);
    int target_y = temp_y(s, WP_F(st, st->stage - 1));

    rect(0, GY_TOP - 4, 240, MID_Y1 - (GY_TOP - 4) + 1, C_BLACK);
    for (x = GX0; x < GX0 + GW; x += 6)
        rect(x, target_y, 3, 1, dim(cur, 150));
    for (i = 0; i < st->n_stages; i++) {
        u16 c = stage_colour(st, s, i);
        col_x(st, i, &x0, &w);
        ty = temp_y(s, WP_F(st, i));
        if (i + 1 < st->stage) {
            rect(x0, ty, w, GY_BASE - ty + 1, c);
        } else if (i + 1 > st->stage) {
            rect(x0, ty, w, 2, c);
            rect(x0, ty, 2, GY_BASE - ty + 1, c);
            rect(x0 + w - 2, ty, 2, GY_BASE - ty + 1, c);
        }
    }
    rect(GX0, GY_BASE + 1, GW, 1, C_AXIS);
    col_x(st, st->stage - 1, &x0, &w);
    rect(x0, GY_BASE + 3, w, 3, cur);
    draw_current(st, s, 1);
    st->drawn_stage = st->stage;
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
    draw_target(st, &s, full || st->stage != st->drawn_stage);
    draw_live(st, full);
    if (full || st->stage != st->drawn_stage)
        draw_chart(st, &s);
    else
        draw_current(st, &s, 0);
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
