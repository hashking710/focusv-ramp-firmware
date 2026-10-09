/* ramp_led.c -- Carta Sport ramp progress on its 5 RGB LEDs and the button light.
 *
 * Same display as the Aeris: the number of lit LEDs is progress through the
 * ramp (1..5), and their colour is the measured temperature on a five-stop
 * blue -> violet -> magenta -> orange -> gold scale spanning this ramp's own
 * coolest-to-hottest stage, so every stage reads as a distinct colour. The
 * control button's light shows the same colour.
 *
 * Both are addressable LEDs on ONE PWM0 output (IR-DMA FIFO mode, DMA7), muxed
 * between two pins with gpio_set_func, and both pushes encode into the same
 * DMA buffer (0x8447a4):
 *   0x8efc  button: G, R, B from the u16s 0x844b0c / 0x844b12 / 0x844b0e, then
 *           waits 50 us for its own transfer before returning
 *   0x8cf8  ring: the 15-byte buffer at 0x8427f8 (LEDs 1-5, then 1-3 again),
 *           returns while its ~230 us transfer is still running. It first
 *           clears the buffer unless 0x842694+15 != 0 (the user's LED preset;
 *           0 = LEDs off) or one of two forced states, scales it IN PLACE by
 *           3/10 in dim mode (0x842694+0x1f) or under 21 % battery, and sends
 *           it scaled by the animation level 0x844b3d (/100).
 * Every stock push (the dispatcher's tail, the power-on and BLE animations)
 * runs: button push, PA0 high (the LED rail, cleared only on the way to
 * sleep), ring push -- so the next push is always a dispatcher pass (20 ms)
 * away and never cuts a ring transfer short. This file does exactly the same,
 * from one place: the dispatcher's call site.
 *
 * Measured temperature is +0x28 (F): +0x2a is Celsius (the orchestrator's
 * out-of-range fallback writes 77 / 25 there).
 */
#include "ramp.h"

#define LED_ENABLED      DEV_LEDS_ON()
#define LED_RGB          ((volatile u8 *)0x8427f8)   /* 5 x (R,G,B) */
#define LED_COUNT        5
#define led_push         STOCK_FN(void_fn, 0x8cf8)

/* The stock dispatcher's animation state (FUN_00008ff8, read from its code):
 * +0 the effect, +1 the level every push is scaled by (0-100), +5 blinks still
 * to play (20 dispatcher passes on, 20 off -- 3 are queued at every session
 * start, 0x6cf0), +6 a fade-in, +7 a fade-out. With nothing queued it sets
 * the level back to 100 on its next pass. Effect 9 is the warning flash: +4
 * flashes (25 passes on, 25 off), then effect 0 -- low battery at session
 * start (0x6c30), heater faults, the dim-mode toggle. While any of that is
 * running the lights stay stock's, so its cues and warnings play out and this
 * file never freezes the level mid-blink (it only moves while the dispatcher
 * runs). Across the whole image nothing else writes +1/+5/+6/+7, so none of
 * this can hold the lights for more than a couple of seconds. */
#define LED_ANIM         ((volatile u8 *)0x844b3c)
#define LED_EFFECT_FLASH 9
#define BTN_R            (*(volatile u16 *)0x844b12)
#define BTN_G            (*(volatile u16 *)0x844b0c)
#define BTN_B            (*(volatile u16 *)0x844b0e)
#define PA_OUT           (*(volatile u8 *)0x800583)  /* bit 0: LED rail */
#define stock_led_dispatch  STOCK_FN(void_fn, 0x8ff8)
#define stock_btn_push      STOCK_FN(void_fn, 0x8efc)

static const u8 STOP_R[5] = {  60, 140, 220, 255, 255 };
static const u8 STOP_G[5] = {  90,  70,  60, 120, 215 };
static const u8 STOP_B[5] = { 255, 230, 160,  60,  60 };

static u8 stock_animating(void)
{
    return LED_ANIM[5] || LED_ANIM[6] || LED_ANIM[7] || LED_ANIM[1] < 100 ||
           LED_ANIM[0] == LED_EFFECT_FLASH;
}

/* Preset picker: one LED per preset position, lit up to the chosen preset in
 * that preset's colour; with the ramp system off, every LED is dim red. The
 * button shows the preset's colour, or red when off. */
static void show_selection(u8 sel, u8 enabled)
{
    int i;
    for (i = 0; i < LED_COUNT; i++) {
        u8 on = i <= sel;
        LED_RGB[i * 3 + 0] = enabled ? (on ? RAMP_PRESET_RGB[sel][0] : 0) : 60;
        LED_RGB[i * 3 + 1] = enabled ? (on ? RAMP_PRESET_RGB[sel][1] : 0) : 0;
        LED_RGB[i * 3 + 2] = enabled ? (on ? RAMP_PRESET_RGB[sel][2] : 0) : 0;
    }
    BTN_R = enabled ? RAMP_PRESET_RGB[sel][0] : RAMP_OFF_R;
    BTN_G = enabled ? RAMP_PRESET_RGB[sel][1] : RAMP_OFF_G;
    BTN_B = enabled ? RAMP_PRESET_RGB[sel][2] : RAMP_OFF_B;
}

static void show_ramp(volatile ramp_state_t *st)
{
    int lo = 0x7fff, hi = 0, f, frac, x, seg, t, lit, i;
    u8 r, g, b;

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
    BTN_R = r;
    BTN_G = g;
    BTN_B = b;
}

/* After every ramp tick: the picker only shows on the idle screen. */
void ramp_led_tick(void)
{
    volatile ramp_state_t *st = RAMP_STATE;
    if (st->picker_on && !ramp_active(st) && !DEV_IDLE())
        ramp_picker_close(st);   /* asleep, or off the idle state: stop showing it */
}

/* Installed at 0x57ee, the only call of the stock LED effect dispatcher 0x8ff8
 * (every other 10 ms main-loop pass: 50 Hz). While a ramp or the picker owns the lights,
 * fill both and push them the way the dispatcher's own tail does; otherwise
 * -- LEDs switched off, a stock cue playing, nothing to show -- run the
 * dispatcher unchanged, which also puts the stock colours back. The buffers
 * are refilled before every push, so 0x8cf8's in-place dimming never
 * compounds. The level is 100 whenever this draws (see LED_ANIM), so the
 * button values need no scaling: the dispatcher would send colour * 100 / 100. */
void ramp_led_entry(void)
{
    volatile ramp_state_t *st = RAMP_STATE;

    if (st->magic != RAMP_MAGIC || st->stock_mode || LED_ENABLED == 0 || stock_animating()) {
        stock_led_dispatch();
        return;
    }
    if (ramp_active(st))
        show_ramp(st);
    else if (st->picker_on && DEV_IDLE())
        show_selection(st->picker_sel, st->picker_enabled);
    else {
        stock_led_dispatch();
        return;
    }
    stock_btn_push();
    PA_OUT |= 1;
    led_push();
}
