/* ramp_led.c -- Aeris ramp progress on its 4 RGB LEDs.
 *
 * While a ramp runs: the number of lit LEDs is progress through the ramp
 * (1..4), and their colour is the measured temperature on a five-stop
 * blue -> violet -> magenta -> orange -> gold scale spanning this ramp's own
 * coolest-to-hottest stage, so every stage reads as a distinct colour.
 *
 * Stock push routine FUN_000090bc (decompiled): drives the LEDs only when
 * device+0xe != 0 -- that byte is the user's LED preset (the triple-click cycle,
 * 0 = LEDs off) -- and dims the buffer in place in low-power / low-battery mode.
 * This file reads that setting and never writes it: with LEDs switched off,
 * nothing is shown. (An earlier version forced it to 1 every tick, overriding
 * the user's LED preset and turning switched-off LEDs back on.)
 *
 * Measured temperature is +0x28 (F): +0x2a is Celsius on this device.
 */
#include "ramp.h"

#define LED_ENABLED      (*(volatile u8 *)(0x84308c + 0x0e))
#define LED_RGB          ((volatile u8 *)0x8431dc)   /* 4 x (R,G,B) */
#define LED_COUNT        4
#define led_push         STOCK_FN(void_fn, 0x90bc)

static const u8 STOP_R[5] = {  60, 140, 220, 255, 255 };
static const u8 STOP_G[5] = {  90,  70,  60, 120, 215 };
static const u8 STOP_B[5] = { 255, 230, 160,  60,  60 };

void ramp_led_update(void)
{
    volatile ramp_state_t *st = RAMP_STATE;
    int lo = 0x7fff, hi = 0, f, frac, x, seg, t, lit, i;
    u8 r, g, b;

    if (!ramp_active(st) || LED_ENABLED == 0)
        return;

    for (i = 0; i < st->n_stages; i++) {
        f = WP_F(st, i);
        if (f < lo) lo = f;
        if (f > hi) hi = f;
    }
    f = FIELD16(OFF_MEAS_F);
    if (f < lo) f = lo;
    if (f > hi) f = hi;
    frac = (hi > lo) ? rom_div((f - lo) * 255, hi - lo) : 255;

    x = frac * 4; seg = x >> 8; t = x & 0xff;
    if (seg > 3) { seg = 3; t = 255; }
    r = (u8)(STOP_R[seg] + (((STOP_R[seg + 1] - STOP_R[seg]) * t) >> 8));
    g = (u8)(STOP_G[seg] + (((STOP_G[seg + 1] - STOP_G[seg]) * t) >> 8));
    b = (u8)(STOP_B[seg] + (((STOP_B[seg + 1] - STOP_B[seg]) * t) >> 8));

    lit = st->total_s ? 1 + rom_div(ramp_elapsed(st) * (LED_COUNT - 1), st->total_s) : LED_COUNT;
    for (i = 0; i < LED_COUNT; i++) {
        u8 on = i < lit;
        LED_RGB[i * 3 + 0] = on ? r : 0;
        LED_RGB[i * 3 + 1] = on ? g : 0;
        LED_RGB[i * 3 + 2] = on ? b : 0;
    }
    led_push();
}
