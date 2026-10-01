/* ramp_led.c -- Aeris
 *
 * Aeris has no screen, so the Carta 2 patch's on-screen graph has no
 * equivalent here -- instead, while a ramp is running, the 4 RGB LEDs
 * show a cool-blue-to-hot-amber gradient based on the live measured
 * temperature's position within the mode's real range. Same colour
 * language and reasoning as the Carta 2 graph's trace (see that patch's
 * ramp_display.c header for the full "why a gradient, not a flat
 * user-picked colour" reasoning -- it applies unchanged here), just
 * driven through the LED hardware instead of a screen.
 *
 * Confirmed facts this relies on, independently re-derived this session:
 *   - 4 individually-addressable RGB LEDs (confirmed hardware fact, from
 *     the loop count in the LED push routine below).
 *   - RGB colour buffer at fixed RAM address 0x8431dc -- 12 bytes, one
 *     R,G,B triplet per LED. Confirmed via direct literal-pool
 *     resolution: FUN_000090bc (the LED PWM push routine) loads this
 *     exact address and iterates it in 3-byte groups, 4 times.
 *   - Global brightness byte at 0x84562c+1 -- confirmed via the same
 *     function, which scales each channel by `channel * brightness/100`
 *     before pushing it to the PWM hardware. This file writes full-
 *     intensity, pre-scaled colour values directly into the RGB buffer;
 *     FUN_000090bc applies the device's own current brightness setting
 *     on top, exactly as it would for any other LED effect -- not
 *     bypassed or duplicated here.
 *   - FUN_000090bc itself gates on `(*(char*)(0x84308c+0xe) != 0) ||
 *     (*(char*)(0x84308c+2) == 2)` before actually pushing anything to
 *     hardware. What struct+0xe means in general wasn't traced to full
 *     confidence (not needed to be -- see below), but forcing it
 *     non-zero immediately before every push, only while a ramp is
 *     actively running, is independently sufficient on its own to make
 *     the push go through, which is all this file needs from it. This
 *     is a deliberate, scoped override of whatever the stock LED-effect
 *     system is doing for the duration of a ramp -- the same philosophy
 *     the Carta 2 patch uses for suppressing the countdown/badge during
 *     a ramp, just applied to LEDs instead of screen elements.
 *   - FUN_000001ac at 0x1ac = the same confirmed ROM divide helper used
 *     throughout this whole project.
 */

typedef unsigned char  u8;
typedef unsigned short u16;

#define STRUCT_BASE        ((volatile u8 *)0x8430e4)
#define MODE_FLAG_OFF      0x6
#define MEASURED_F_OFF     0x2a

#define LED_GATE_FLAG      ((volatile u8 *)0x843098)  /* 0x84308c + 0xe */
#define LED_RGB_BUFFER     ((volatile u8 *)0x8431dc)  /* 4 x (R,G,B) */
#define LED_COUNT          4

typedef short (*rom_div_fn)(int, int);
#define rom_div ((rom_div_fn)0x1ac)

typedef void (*led_push_fn)(void);
#define led_push ((led_push_fn)0x90bc)

/* Real confirmed per-mode limits for Aeris specifically (DEVICE_LIMITS,
 * terpline-web/lib/protocol/deviceLimits.ts) -- lower concentrate ceiling
 * than Carta 2/Sport (600F vs 635F), confirmed project-wide fact. */
static void temp_range(u8 mode, u16 *lo, u16 *hi)
{
    if (mode == 1) { *lo = 275; *hi = 500; }
    else           { *lo = 365; *hi = 600; }
}

/* Same cool-blue -> hot-amber linear gradient as the Carta 2 graph's
 * trace -- see that file for why this shape was chosen over anything
 * fancier (no hardware divide/float, every extra stop costs another
 * rom_div call). Placeholder RGB endpoints, same as Carta 2's -- no way
 * to see this on a real LED yet. */
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
 * whether a ramp is actually active -- this must NOT touch the LEDs at
 * all otherwise, or it would hijack normal LED behaviour on every single
 * tick regardless of whether a ramp is running. Cheap enough when it does
 * run (one 12-byte buffer fill, one function call) that there's no need
 * for the incremental/only-on-change complexity the Carta 2 screen patch
 * needs to avoid expensive redraws. */
void ramp_led_update(u8 ramp_active)
{
    u8 mode;

    if (!ramp_active)
        return;

    mode = STRUCT_BASE[MODE_FLAG_OFF];
    u16 measured_f = *(volatile u16 *)(STRUCT_BASE + MEASURED_F_OFF);
    u16 lo, hi;
    u8 r, g, b;
    u8 i;

    temp_range(mode, &lo, &hi);
    heat_color(measured_f, lo, hi, &r, &g, &b);

    for (i = 0; i < LED_COUNT; i++) {
        LED_RGB_BUFFER[i * 3 + 0] = r;
        LED_RGB_BUFFER[i * 3 + 1] = g;
        LED_RGB_BUFFER[i * 3 + 2] = b;
    }

    *LED_GATE_FLAG = 1;
    led_push();
}
