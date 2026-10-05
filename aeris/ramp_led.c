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

/* Preset picker: one LED per preset position, lit up to the chosen preset in
 * that preset's colour; with the ramp system off, every LED is dim red. */
static void show_selection(u8 sel, u8 enabled)
{
    int i;
    for (i = 0; i < LED_COUNT; i++) {
        u8 on = i <= sel;
        LED_RGB[i * 3 + 0] = enabled ? (on ? RAMP_PRESET_RGB[sel][0] : 0) : 60;
        LED_RGB[i * 3 + 1] = enabled ? (on ? RAMP_PRESET_RGB[sel][1] : 0) : 0;
        LED_RGB[i * 3 + 2] = enabled ? (on ? RAMP_PRESET_RGB[sel][2] : 0) : 0;
    }
    led_push();
}

static void button(volatile ramp_state_t *st, u8 r, u8 g, u8 b)
{
    st->btn_rgb[0] = r;
    st->btn_rgb[1] = g;
    st->btn_rgb[2] = b;
    st->btn_on = 1;
}

void ramp_led_update(void)
{
    volatile ramp_state_t *st = RAMP_STATE;
    int lo = 0x7fff, hi = 0, f, frac, x, seg, t, lit, i;
    u8 r, g, b;

    if (st->picker_on && !ramp_active(st) && !DEV_IDLE())
        ramp_picker_close(st);   /* asleep, or off the idle state: stop showing it */
    st->btn_on = 0;              /* the button is stock's unless set below */
    if (LED_ENABLED == 0)
        return;
    if (!ramp_active(st)) {
        if (st->picker_on) {
            show_selection(st->picker_sel, st->picker_enabled);
            if (st->picker_enabled)
                button(st, RAMP_PRESET_RGB[st->picker_sel][0], RAMP_PRESET_RGB[st->picker_sel][1],
                       RAMP_PRESET_RGB[st->picker_sel][2]);
            else
                button(st, RAMP_OFF_R, RAMP_OFF_G, RAMP_OFF_B);
        }
        return;
    }

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
    button(st, r, g, b);
}

/* The control button's light. It's an RGB LED on PB5/PB6/PB7, driven by a
 * software-PWM timer interrupt (0x49c: counter 0-99 against three duty values,
 * active low): 0x845602 red, 0x8455fc green, 0x8455fe blue, each 0-100. The
 * stock LED effect dispatcher 0x920c (one caller, 0x61ae, top of the main loop)
 * sets them, scaled by the brightness byte 0x84562d. Installed at 0x61ae: while a
 * ramp or the picker owns the lights, set the duties here instead of running
 * the stock effects (the ring is pushed by ramp_led_update); otherwise run the
 * dispatcher unchanged, which also puts the stock colour back. */
#define BTN_DUTY_R   (*(volatile u16 *)0x845602)
#define BTN_DUTY_G   (*(volatile u16 *)0x8455fc)
#define BTN_DUTY_B   (*(volatile u16 *)0x8455fe)
#define BRIGHTNESS   (*(volatile u8 *)0x84562d)
#define stock_led_dispatch  STOCK_FN(void_fn, 0x920c)

void ramp_btn_entry(void)
{
    volatile ramp_state_t *st = RAMP_STATE;
    int bri = BRIGHTNESS > 100 ? 100 : BRIGHTNESS;
    if (st->magic != RAMP_MAGIC || !st->btn_on || LED_ENABLED == 0) {
        stock_led_dispatch();
        return;
    }
    BTN_DUTY_R = (u16)rom_div(st->btn_rgb[0] * bri, 255);
    BTN_DUTY_G = (u16)rom_div(st->btn_rgb[1] * bri, 255);
    BTN_DUTY_B = (u16)rom_div(st->btn_rgb[2] * bri, 255);
}
