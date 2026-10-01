/* ramp_display.c
 *
 * While a hardware ramp is running, replaces the Carta 2's normal
 * live-heating screen elements with a target-vs-measured temperature
 * graph, instead of the usual single-number dial. Three stock functions
 * get call-site-swapped for thin wrappers here (same technique as the
 * rest of this patch -- same-length replacement, no insertion, originals
 * left completely untouched and still called in the non-ramp case):
 *
 *   - FUN_0000d3c0 (main temp+gauge number)   -> ramp_temp_display
 *   - FUN_0000d048 (session countdown timer)  -> ramp_countdown_or_skip
 *   - FUN_0000e42c (active-preset-slot badge) -> ramp_badge_or_skip
 *
 * All three patch sites live inside FUN_0000fa1c, the confirmed
 * live-heating-screen refresh routine (see firmware-analysis-notes.md,
 * "the entire live-heating status bar decoded"). FUN_0000cf58/0xce70
 * (the small target-temperature readouts) and FUN_0000db40 (battery) are
 * left completely alone -- both stay meaningful during a ramp.
 *
 * Design choices worth recording (not just the "what", the "why"):
 *
 * - Colour: the measured-temperature trace is coloured by a cool->hot
 *   gradient based on each point's own temperature, rather than a single
 *   BLE-configurable colour. Reasoning: a fixed user-picked colour is
 *   purely cosmetic and needs real new firmware surface (a new marker
 *   opcode, flash storage, app UI) for something that conveys no
 *   information. A temperature gradient needs none of that -- it's pure
 *   computation from data already on-device -- and it's more legible at
 *   a glance (hotter visibly reads as "further along"/"more intense")
 *   without duplicating what the stage indicator already shows. Can
 *   still add BLE-configurable colour later as a pure personalization
 *   layer on top of this if wanted; this isn't mutually exclusive with
 *   it, just a better place to spend the first pass.
 * - The target line stays a fixed, cooler slate colour rather than also
 *   being heat-graded, and is drawn genuinely dashed and thinner than the
 *   live trace (draw_dashed_segment, TARGET_THICKNESS) -- three separate,
 *   reinforcing signals (colour, pattern, weight) that it's "the reference
 *   plan", not competing with the vivid, heavier "what's actually
 *   happening" trace. fill_rect gives pixel-exact control, so the dash
 *   pattern is literal, not simulated -- no reason to settle for
 *   colour-only once that was worked out.
 * - A bright white "now" marker caps the live trace at its newest sample
 *   (LIVE_MARKER_SIZE, oversized relative to the line), and the stage
 *   digit is drawn as a filled badge in the same colour rather than a bare
 *   glyph -- one shared accent colour used for both "look here" cues. The
 *   stage badge is also this display's one animation-adjacent touch: since
 *   it only gets drawn once per stage change anyway (incremental drawing,
 *   see below), each new stage necessarily debuts "lit up" rather than
 *   quietly appearing -- the closest thing to a transition flash available
 *   without a per-tick fade timer.
 * - Waypoint columns get a small tick mark on the X axis (draw_waypoint_tick)
 *   so the ramp's shape is readable directly off the chart -- where each
 *   stage starts and ends -- not just inferable from the dashed line's
 *   slope changes.
 * - Drawing is incremental, not a full redraw every call. This function
 *   runs on the same per-tick cadence as the rest of the live-heating
 *   screen, but new trace samples only land every trace_stride ticks
 *   (ramp_tick.c) -- most calls would otherwise be redrawing identical
 *   pixels for no reason. The static frame (axes + target line) draws
 *   once per ramp run; after that, only newly-sampled trace columns get
 *   painted, tracked via graph_drawn_col in the shared RAM state.
 * - No BLE/preset-rank badge during a ramp -- FUN_0000e42c is skipped
 *   entirely (not repurposed) rather than fed a substitute rank value.
 *   Freed that whole screen region for a bigger graph instead, and this
 *   file draws its own compact single-digit stage indicator in a
 *   deliberately out-of-the-way spot (bottom-left, below the graph),
 *   using the same confirmed digit-glyph table FUN_0000ce70/0xcf58 use --
 *   full control over placement instead of inheriting wherever 0xe42c's
 *   own jump-table handlers happened to draw.
 *
 * Confirmed facts this relies on, all from direct decompile this session:
 *   - FUN_0000d048 is unambiguously the countdown timer -- its digit
 *     decomposition with progressive leading-zero suppression is the
 *     classic MM:SS rendering pattern, not a fixed-width number.
 *   - struct+0x1c/0x1d = live MEASURED temperature, Fahrenheit --
 *     confirmed via FUN_00008b08 (the RTD/Callendar-Van Dusen conversion
 *     routine), which writes its result directly into this field.
 *   - FUN_000074d0(x, y, w, h, colorHi, colorLo) is a confirmed solid
 *     rectangle fill: sets the ST7789 drawing window (CASET/RASET via
 *     FUN_00007464), sends RAMWR (0x2C), then writes (w+1)*(h+1) pixels
 *     of the given RGB565 colour. No bitmap/glyph data needed.
 *   - FUN_00007e2c(x, y, w, h, colorHi, colorLo, glyphPtr) is a confirmed
 *     glyph/bitmap blit (always takes a source pointer, unlike 0x74d0).
 *     The small digit table at 0x1e6cc (resolved directly from the
 *     binary, "PTR_DAT_0000cf54" in Ghidra's naming) is what
 *     FUN_0000ce70/0xcf58 use, 0x22 (34) bytes per glyph, drawn as a
 *     9x16 box (confirmed from their own call sites' w/h arguments).
 *     Reused here as-is for the stage digit -- same table, same size.
 *
 * Screen geometry: GRAPH_* below assumes the area vacated by the
 * countdown/badge row (roughly y=190-238) is now free in addition to the
 * main dial's old area (y=52-190ish), inferred from FUN_0000d048's and
 * FUN_0000e42c's own draw coordinates, not independently measured
 * against a real device. Treat as a first guess to refine on hardware.
 *
 * Colours (COLOR_* below) are otherwise-placeholder RGB565 values --
 * there's been no way to see any of this on a real screen yet. The
 * *shape* of the colour scheme (graded trace, neutral target) is the
 * considered part; the exact hex endpoints are easy to retune later.
 */

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;

#define STRUCT_BASE         ((volatile u8 *)0x843028)
#define MODE_FLAG_OFF       0x7
#define MEASURED_F_OFF      0x1c

#define RAMP_FLASH_SECTOR   ((volatile u8 *)0x31000)  /* must match ramp_save.c
                             * and ramp_tick.c */
#define RAMP_NUM_SLOTS      5
#define RAMP_SLOT_SIZE      4

#define RAMP_MAGIC          0xA5C4
#define TRACE_LEN           150     /* must match ramp_tick.c's TRACE_LEN */
#define TRACE_TEMP_BIAS     200     /* must match ramp_tick.c's bias */

/* Mirrors ramp_tick.c's ramp_state_t exactly -- two translation units,
 * one struct layout, kept in sync by hand (no shared header in this
 * freestanding build setup). If one changes, the other must too. */
typedef struct {
    u16 magic;
    u16 tick_counter;
    u8  waypoint_index;
    u8  trace_stride;
    u8  trace_tick_count;
    u8  trace_col;
    u8  graph_frame_drawn;  /* 0 until axes+target line drawn this run */
    u8  graph_drawn_col;    /* how much of the trace is already on screen */
    u8  graph_drawn_stage;  /* last-painted stage digit, 0 = none yet */
    u8  trace[TRACE_LEN];
} ramp_state_t;
#define RAMP_STATE ((volatile ramp_state_t *)0x848000)

typedef short (*rom_div_fn)(int, int);
#define rom_div ((rom_div_fn)0x1ac)

typedef void (*orig_fn)(void);
#define orig_temp_display ((orig_fn)0xd3c0)
#define orig_countdown    ((orig_fn)0xd048)

typedef void (*fill_rect_fn)(u8 x, u8 y, u8 w, u8 h, u8 color_hi, u8 color_lo);
#define fill_rect ((fill_rect_fn)0x74d0)

typedef void (*blit_glyph_fn)(u8 x, u8 y, u8 w, u8 h, u8 color_hi, u8 color_lo,
                               const void *glyph);
#define blit_glyph ((blit_glyph_fn)0x7e2c)

#define DIGIT_TABLE       ((const u8 *)0x1e6cc)  /* resolved this session,
                           * the same table FUN_0000ce70/0xcf58 use */
#define DIGIT_GLYPH_SIZE  0x22
#define DIGIT_W           9
#define DIGIT_H           0x10

/* Graph area, expanded to use the space the countdown/badge row used to
 * occupy (both suppressed during a ramp -- see header). */
#define GRAPH_X0   20
#define GRAPH_Y0   50
#define GRAPH_W    TRACE_LEN   /* one pixel per trace column -- simplest
                                 * possible X mapping, no scaling needed */
#define GRAPH_H    140

#define STAGE_X    GRAPH_X0
#define STAGE_Y    (GRAPH_Y0 + GRAPH_H + 10)

#define LINE_THICKNESS   2
#define TARGET_THICKNESS 1   /* thinner than the live trace -- reinforces
                               * "reference" vs "live data" by weight too,
                               * not just colour and dash pattern */
#define LIVE_MARKER_SIZE 4   /* the bright cap drawn at the newest sample --
                               * bigger than LINE_THICKNESS so it pops as a
                               * distinct "you are here", not just another
                               * point on the line */

/* Dash pattern for the target line: DASH_ON lit columns, then
 * (DASH_PERIOD - DASH_ON) skipped, repeating. This is a real dashed line,
 * not a simulated one -- fill_rect gives pixel-exact control, so the
 * "matching the client app's dashed-vs-solid convention" idea from the
 * header can actually be literal here, not just colour-coded. */
#define DASH_PERIOD   6
#define DASH_ON       3

/* Placeholder RGB565 values -- there's been no way to see any of this on a
 * real screen yet, so these are considered-but-unverified choices, not
 * measured ones. The palette is deliberately a small, coherent set (cool
 * slate frame, graded blue->amber trace, one bright accent reused for both
 * the live marker and the stage badge) rather than one-off colours per
 * element, so it reads as a single designed scheme instead of a pile of
 * placeholders -- easy to retune the exact hex endpoints later without
 * having to rethink the relationships between them. */
/* Every value below was chosen as a real rgb(r,g,b) target and converted to
 * RGB565 programmatically (not hand-picked hex), specifically because a
 * hand-picked RGB565 literal is very easy to get subtly wrong -- the 5-6-5
 * bit packing doesn't read anywhere close to intuitively, and a value that
 * "looks about right" as hex can easily decode to a noticeably different
 * color than intended. Shown here as both forms so a future retune starts
 * from the same real-color intent, not from reverse-engineering old hex. */
#define COLOR_AXIS         0x3A4B  /* rgb(60,72,94)   -- dim cool slate, frame, recedes */
#define COLOR_TARGET       0x84F8  /* rgb(134,156,196) -- brighter slate-blue, dashed ref line */
#define COLOR_COOL         0x139F  /* rgb(20,112,255)  -- vivid blue, low end of trace */
#define COLOR_HOT          0xFD03  /* rgb(255,161,24)  -- vivid amber, high end of trace */
#define COLOR_LIVE_MARKER  0xFFFF  /* white -- the one deliberately-unmissable
                                     * accent, reused for both the "now" cap
                                     * on the trace and the stage-change badge
                                     * fill, so the two "look here" cues in
                                     * this UI share one visual language */
#define COLOR_STAGE_TEXT   0x0000  /* dark glyph against the bright badge
                                     * fill above -- stays legible */

static u8 ramp_is_active(volatile ramp_state_t *st)
{
    return st->magic == RAMP_MAGIC
        && st->waypoint_index != 0
        && st->waypoint_index <= RAMP_NUM_SLOTS;
}

static u16 slot_temp_c(u8 slot)
{
    return *(volatile u16 *)(RAMP_FLASH_SECTOR + slot * RAMP_SLOT_SIZE);
}

static u16 slot_duration(u8 slot)
{
    return *(volatile u16 *)(RAMP_FLASH_SECTOR + slot * RAMP_SLOT_SIZE + 2);
}

static u16 c_to_f(u16 c)
{
    return (u16)(rom_div((int)c * 9, 5) + 32);
}

/* Real confirmed per-mode limits (DEVICE_LIMITS, Quantum) -- used as the
 * graph's Y-axis range rather than deriving min/max from the waypoints
 * themselves, so the scale stays consistent across different saved ramps
 * instead of jumping around per-ramp. */
static void graph_range(u8 mode, u16 *lo, u16 *hi)
{
    if (mode == 0) { *lo = 300; *hi = 460; }
    else           { *lo = 365; *hi = 635; }
}

static u8 temp_to_y(u16 temp_f, u16 lo, u16 hi)
{
    u16 span = hi - lo;
    u16 clamped = temp_f;
    if (clamped < lo) clamped = lo;
    if (clamped > hi) clamped = hi;
    return (u8)(GRAPH_Y0 + GRAPH_H -
                rom_div((int)(clamped - lo) * GRAPH_H, (int)span));
}

/* Cool->hot linear gradient, per-channel, based on where temp_f falls in
 * the mode's real range. Picked over anything fancier (HSV, multi-stop)
 * since this chip has no hardware divide or float -- every extra stop or
 * channel-space conversion is another rom_div call per point, and this
 * already runs once per newly-sampled column, every ramp tick. */
static u16 heat_color(u16 temp_f, u16 lo, u16 hi)
{
    u16 span = hi - lo;
    u16 clamped = temp_f;
    int frac;   /* 0-255 */
    u8 r0, g0, b0, r1, g1, b1, r, g, b;

    if (clamped < lo) clamped = lo;
    if (clamped > hi) clamped = hi;
    frac = rom_div((int)(clamped - lo) * 255, (int)span);

    r0 = (COLOR_COOL >> 11) & 0x1f; g0 = (COLOR_COOL >> 5) & 0x3f; b0 = COLOR_COOL & 0x1f;
    r1 = (COLOR_HOT  >> 11) & 0x1f; g1 = (COLOR_HOT  >> 5) & 0x3f; b1 = COLOR_HOT  & 0x1f;

    r = (u8)(r0 + rom_div((r1 - r0) * frac, 255));
    g = (u8)(g0 + rom_div((g1 - g0) * frac, 255));
    b = (u8)(b0 + rom_div((b1 - b0) * frac, 255));

    return (u16)((r << 11) | (g << 5) | b);
}

static void draw_point(u8 col, u8 y, u16 color)
{
    fill_rect(GRAPH_X0 + col, y, LINE_THICKNESS, LINE_THICKNESS,
              (u8)(color >> 8), (u8)(color & 0xff));
}

/* Fills a stepped line between two (column, y) points, same colour for
 * the whole segment -- used, per-segment, for the measured trace (colour
 * recomputed per segment so the gradient still shows across a multi-column
 * jump). The target line uses draw_dashed_segment below instead. */
static void draw_segment(u8 col0, u8 y0, u8 col1, u8 y1, u16 color)
{
    if (col1 == col0) {
        draw_point(col0, y0, color);
        return;
    }
    {
        u8 x;
        for (x = col0; x <= col1; x++) {
            u8 yy = (u8)(y0 - rom_div((int)(y0 - y1) * (x - col0), col1 - col0));
            draw_point(x, yy, color);
        }
    }
}

/* Same stepped lerp as draw_segment, but only paints a column when it falls
 * within the "on" portion of the DASH_PERIOD/DASH_ON pattern -- a real
 * dashed line, pixel-exact, not a simulated one (see DASH_PERIOD comment).
 * Phase is measured from col0 every call rather than carried globally, so
 * the dash pattern restarts cleanly at each waypoint-to-waypoint segment
 * instead of drifting across the whole target line. */
static void draw_dashed_segment(u8 col0, u8 y0, u8 col1, u8 y1, u16 color)
{
    if (col1 == col0) {
        draw_point(col0, y0, color);
        return;
    }
    {
        /* Counted with a wrapping local counter rather than `%` -- this chip
         * has no hardware divide/modulo, and pulling in a libgcc software
         * modulo would fail to link in this freestanding build (same reason
         * every other division here goes through rom_div). */
        u8 x;
        u8 phase = 0;
        for (x = col0; x <= col1; x++) {
            if (phase < DASH_ON) {
                u8 yy = (u8)(y0 - rom_div((int)(y0 - y1) * (x - col0), col1 - col0));
                fill_rect(GRAPH_X0 + x, yy, TARGET_THICKNESS, TARGET_THICKNESS,
                          (u8)(color >> 8), (u8)(color & 0xff));
            }
            phase++;
            if (phase >= DASH_PERIOD) phase = 0;
        }
    }
}

/* Small downward tick at a waypoint's column, just below the axis line --
 * marks each stage boundary directly on the chart so the shape of the
 * ramp is legible at a glance, not just inferable from the dashed line's
 * slope changes. Purely decorative/informational, same axis colour. */
static void draw_waypoint_tick(u8 col)
{
    fill_rect(GRAPH_X0 + col, GRAPH_Y0 + GRAPH_H + 2, 1, 3,
              (u8)(COLOR_AXIS >> 8), (u8)(COLOR_AXIS & 0xff));
}

/* One-time per ramp run: axes + the full target line, computed from the
 * saved waypoints (known in advance, unlike the measured trace). */
static void draw_frame(u8 mode, u16 lo, u16 hi, u8 stride)
{
    u8  i;
    u16 cum_ticks = 0;
    u8  prev_col = 0, prev_y = 0, have_prev = 0;

    fill_rect(GRAPH_X0, GRAPH_Y0 + GRAPH_H, GRAPH_W, 1,
              (u8)(COLOR_AXIS >> 8), (u8)(COLOR_AXIS & 0xff));
    fill_rect(GRAPH_X0, GRAPH_Y0, 1, GRAPH_H,
              (u8)(COLOR_AXIS >> 8), (u8)(COLOR_AXIS & 0xff));

    for (i = 0; i < RAMP_NUM_SLOTS; i++) {
        u16 d = slot_duration(i);
        u16 t_f;
        u8  y, c;

        if (d == 0 || d == 0xffff)
            break;

        t_f = c_to_f(slot_temp_c(i));
        cum_ticks += d;

        c = (u8)rom_div((int)cum_ticks, (int)stride);
        if (c >= GRAPH_W) c = GRAPH_W - 1;
        y = temp_to_y(t_f, lo, hi);

        if (have_prev) {
            draw_dashed_segment(prev_col, prev_y, c, y, COLOR_TARGET);
        } else {
            draw_point(c, y, COLOR_TARGET);
        }
        draw_waypoint_tick(c);
        prev_col = c;
        prev_y = y;
        have_prev = 1;
    }
    (void)mode;
}

/* Only the columns sampled since the last call -- see the file header
 * for why this matters (most calls add zero new samples). Each new
 * segment is colour-graded by its own endpoint temperature. */
static void draw_new_trace_points(volatile ramp_state_t *st, u16 lo, u16 hi)
{
    u8 col;
    u8 prev_col, prev_y;
    u8 have_prev = st->graph_drawn_col > 0;

    if (have_prev) {
        u16 prev_f = (u16)st->trace[st->graph_drawn_col - 1] + TRACE_TEMP_BIAS;
        prev_col = st->graph_drawn_col - 1;
        prev_y   = temp_to_y(prev_f, lo, hi);
    } else {
        prev_col = 0;
        prev_y   = 0;
    }

    for (col = st->graph_drawn_col; col < st->trace_col; col++) {
        u16 temp_f = (u16)st->trace[col] + TRACE_TEMP_BIAS;
        u8  y = temp_to_y(temp_f, lo, hi);
        u16 color = heat_color(temp_f, lo, hi);

        if (have_prev) {
            draw_segment(prev_col, prev_y, col, y, color);
        } else {
            draw_point(col, y, color);
        }
        prev_col = col;
        prev_y = y;
        have_prev = 1;
    }

    /* A bright "you are here" cap at the newest sample, oversized relative
     * to the line itself so the current position is unmistakable at a
     * glance, not just inferable from "wherever the line stops". The next
     * new sample's segment draws right over it as the line extends, so
     * there's never a stale marker left behind mid-trace. */
    if (have_prev) {
        u8 half = LIVE_MARKER_SIZE / 2;
        u8 mx = (prev_col > half) ? (u8)(prev_col - half) : 0;
        u8 my = (prev_y > half) ? (u8)(prev_y - half) : 0;
        fill_rect(GRAPH_X0 + mx, my, LIVE_MARKER_SIZE, LIVE_MARKER_SIZE,
                  (u8)(COLOR_LIVE_MARKER >> 8), (u8)(COLOR_LIVE_MARKER & 0xff));
    }

    st->graph_drawn_col = st->trace_col;
}

#define STAGE_BADGE_PAD  4
#define STAGE_BADGE_W    (DIGIT_W + STAGE_BADGE_PAD * 2)
#define STAGE_BADGE_H    (DIGIT_H + STAGE_BADGE_PAD * 2)

/* Compact single-digit stage indicator ("1".."5"), replacing the
 * suppressed preset-rank badge. Drawn as a filled badge rather than a bare
 * glyph on the plain background -- a small, deliberate "chip" rather than
 * a number that looks like it landed there by accident. Only redraws when
 * the stage actually changes, same incremental-drawing reasoning as the
 * trace; since that means this is also the stage's one and only moment of
 * being drawn, the bright COLOR_LIVE_MARKER badge fill doubles as this
 * display's one animation-adjacent cue -- each new stage debuts "lit up"
 * rather than quietly appearing, the closest thing to a transition flash
 * achievable without a per-tick fade timer. */
static void draw_stage_digit(volatile ramp_state_t *st)
{
    if (st->waypoint_index == st->graph_drawn_stage)
        return;

    fill_rect(STAGE_X - STAGE_BADGE_PAD, STAGE_Y - STAGE_BADGE_PAD,
              STAGE_BADGE_W, STAGE_BADGE_H,
              (u8)(COLOR_LIVE_MARKER >> 8), (u8)(COLOR_LIVE_MARKER & 0xff));
    blit_glyph(STAGE_X, STAGE_Y, DIGIT_W, DIGIT_H,
               (u8)(COLOR_STAGE_TEXT >> 8), (u8)(COLOR_STAGE_TEXT & 0xff),
               DIGIT_TABLE + (u32)st->waypoint_index * DIGIT_GLYPH_SIZE);
    st->graph_drawn_stage = st->waypoint_index;
}

static void ramp_graph_draw(volatile ramp_state_t *st)
{
    u8  mode = STRUCT_BASE[MODE_FLAG_OFF];
    u16 lo, hi;
    u8  stride = st->trace_stride ? st->trace_stride : 1;

    graph_range(mode, &lo, &hi);

    if (!st->graph_frame_drawn) {
        draw_frame(mode, lo, hi, stride);
        st->graph_frame_drawn = 1;
    }

    draw_new_trace_points(st, lo, hi);
    draw_stage_digit(st);
}

/* ---- the three call-site replacements ---- */

void ramp_temp_display(void)
{
    volatile ramp_state_t *st = RAMP_STATE;
    if (ramp_is_active(st)) {
        ramp_graph_draw(st);
    } else {
        orig_temp_display();
    }
}

void ramp_countdown_or_skip(void)
{
    volatile ramp_state_t *st = RAMP_STATE;
    if (ramp_is_active(st)) {
        return;   /* no session countdown during a ramp -- waypoints carry
                   * their own, independent durations */
    }
    orig_countdown();
}

void ramp_badge_or_skip(void)
{
    volatile ramp_state_t *st = RAMP_STATE;
    if (ramp_is_active(st)) {
        return;   /* preset rank is meaningless during a ramp; this screen
                   * space goes to the bigger graph instead (see header) */
    }
    ((orig_fn)0xe42c)();
}
