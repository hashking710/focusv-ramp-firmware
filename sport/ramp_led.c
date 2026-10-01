/* ramp_led.c -- Carta Sport
 *
 * Same cool-blue-to-hot-amber progress gradient as the Aeris patch,
 * adapted to Sport's confirmed addresses and its 5 LEDs (vs Aeris's 4 --
 * confirmed hardware difference, not a typo: Sport's push routine loops
 * until a 5-LED boundary, Aeris's until 4).
 *
 * Confirmed facts this relies on, independently re-derived this session:
 *   - RGB colour buffer at fixed RAM address 0x8427f8 -- 15 bytes, one
 *     R,G,B triplet per LED, confirmed via direct literal-pool
 *     resolution against the real binary.
 *   - Global brightness byte at 0x844b3c+1 -- confirmed the same way.
 *   - The LED push routine (body starting 0x8cf8) has a DIFFERENT gate
 *     architecture than Aeris's equivalent, confirmed by direct
 *     decompile of both rather than assumed identical: Sport's gate
 *     condition only controls an optional "reset buffer to black"
 *     pre-step; the actual scaling + hardware push loop runs
 *     unconditionally every call, regardless of that gate. This file
 *     does NOT force any flag open before calling it -- unlike the Aeris
 *     patch, which genuinely needs to for its own (confirmed different)
 *     gate shape. Worth stating plainly since copying Aeris's
 *     gate-forcing logic here unchanged would have been silently
 *     harmless at best and silently wrong at worst if the two turned out
 *     to differ in some other way.
 *   - FUN_0001529c at 0x1529c = this build's own confirmed divide
 *     helper (quotient) -- see ramp_tick.c's header for why this is NOT
 *     the same address as the other two devices.
 */

typedef unsigned char  u8;
typedef unsigned short u16;

#define STRUCT_BASE        ((volatile u8 *)0x8426ec)
#define MODE_FLAG_OFF      0x6
#define MEASURED_F_OFF     0x2a

#define LED_RGB_BUFFER     ((volatile u8 *)0x8427f8)  /* 5 x (R,G,B) */
#define LED_COUNT          5

typedef short (*rom_div_fn)(int, int);
#define rom_div ((rom_div_fn)0x1529c)

typedef void (*led_push_fn)(void);
#define led_push ((led_push_fn)0x8cf8)

/* Real confirmed per-mode limits for Carta Sport (DEVICE_LIMITS,
 * terpline-web/lib/protocol/deviceLimits.ts) -- same ranges as Carta 2
 * (unlike Aeris, which has a lower concentrate ceiling). */
static void temp_range(u8 mode, u16 *lo, u16 *hi)
{
    if (mode == 1) { *lo = 275; *hi = 500; }
    else           { *lo = 365; *hi = 635; }
}

#define COLOR_COOL_R 20
#define COLOR_COOL_G 112
#define COLOR_COOL_B 255
#define COLOR_HOT_R  255
#define COLOR_HOT_G  161
#define COLOR_HOT_B  24

static void heat_color(u16 temp_f, u16 lo, u16 hi, u8 *r, u8 *g, u8 *b)
{
    u16 span = hi - lo;
    u16 clamped = temp_f;
    int frac;

    if (clamped < lo) clamped = lo;
    if (clamped > hi) clamped = hi;
    frac = rom_div((int)(clamped - lo) * 255, (int)span);

    *r = (u8)(COLOR_COOL_R + rom_div((COLOR_HOT_R - COLOR_COOL_R) * frac, 255));
    *g = (u8)(COLOR_COOL_G + rom_div((COLOR_HOT_G - COLOR_COOL_G) * frac, 255));
    *b = (u8)(COLOR_COOL_B + rom_div((COLOR_HOT_B - COLOR_COOL_B) * frac, 255));
}

/* Called every tick from ramp_trampoline (ramp_tick.c), which passes
 * whether a ramp is actually active -- must not touch the LEDs at all
 * otherwise. */
void ramp_led_update(u8 ramp_active)
{
    u8 mode;
    u16 measured_f;
    u16 lo, hi;
    u8 r, g, b;
    u8 i;

    if (!ramp_active)
        return;

    mode = STRUCT_BASE[MODE_FLAG_OFF];
    measured_f = *(volatile u16 *)(STRUCT_BASE + MEASURED_F_OFF);

    temp_range(mode, &lo, &hi);
    heat_color(measured_f, lo, hi, &r, &g, &b);

    for (i = 0; i < LED_COUNT; i++) {
        LED_RGB_BUFFER[i * 3 + 0] = r;
        LED_RGB_BUFFER[i * 3 + 1] = g;
        LED_RGB_BUFFER[i * 3 + 2] = b;
    }

    led_push();
}
