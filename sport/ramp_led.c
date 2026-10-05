/* ramp_led.c -- Carta Sport ramp progress on its 5 RGB LEDs.
 *
 * Same display as the Aeris: the number of lit LEDs is progress through the
 * ramp (1..5), and their colour is the measured temperature on a five-stop
 * blue -> violet -> magenta -> orange -> gold scale spanning this ramp's own
 * coolest-to-hottest stage, so every stage reads as a distinct colour.
 *
 * Stock push routine FUN_00008cf8 (disassembled): always pushes the 15-byte
 * buffer at 0x8427f8, but first clears it to black unless 0x842694+15 != 0
 * (the user's LED preset, cycled by event 9 in the consumer at 0x45cc; 0 = LEDs
 * off) or one of two stock states that force the LEDs on. It scales the buffer
 * by the brightness byte 0x844b3c+1 itself, and dims it in low-battery mode.
 * This file reads the LED preset and never writes it: with LEDs switched off,
 * nothing is drawn and nothing is pushed.
 *
 * Measured temperature is +0x28 (F): +0x2a is Celsius (the orchestrator's
 * out-of-range fallback writes 77 / 25 there). The previous Sport version read
 * +0x2a as Fahrenheit.
 */
#include "ramp.h"

#define LED_ENABLED      (*(volatile u8 *)(0x842694 + 0x0f))
#define LED_RGB          ((volatile u8 *)0x8427f8)   /* 5 x (R,G,B) */
#define LED_COUNT        5
#define led_push         STOCK_FN(void_fn, 0x8cf8)

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

/* The control button's light: one more addressable RGB LED, sent by 0x8efc
 * (green, red, blue) from 0x844b0c / 0x844b12 / 0x844b0e. The stock LED effect
 * dispatcher 0x8ff8 (one caller, 0x57ee) fills those from its colour scaled
 * by the brightness byte 0x844b3d (/100), then sends them. Installed at 0x57ee:
 * while a ramp or the picker owns the lights, fill and send them here instead
 * of running the stock effects (the ring is pushed by ramp_led_update);
 * otherwise run the dispatcher unchanged, which also puts the stock colour
 * back. */
#define BTN_R        (*(volatile u16 *)0x844b12)
#define BTN_G        (*(volatile u16 *)0x844b0c)
#define BTN_B        (*(volatile u16 *)0x844b0e)
#define BRIGHTNESS   (*(volatile u8 *)0x844b3d)
#define stock_led_dispatch  STOCK_FN(void_fn, 0x8ff8)
#define stock_btn_push      STOCK_FN(void_fn, 0x8efc)

void ramp_btn_entry(void)
{
    volatile ramp_state_t *st = RAMP_STATE;
    int bri = BRIGHTNESS > 100 ? 100 : BRIGHTNESS;
    if (st->magic != RAMP_MAGIC || !st->btn_on || LED_ENABLED == 0) {
        stock_led_dispatch();
        return;
    }
    BTN_R = (u16)rom_div(st->btn_rgb[0] * bri, 100);
    BTN_G = (u16)rom_div(st->btn_rgb[1] * bri, 100);
    BTN_B = (u16)rom_div(st->btn_rgb[2] * bri, 100);
    stock_btn_push();
}
