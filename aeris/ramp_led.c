/* ramp_led.c -- Aeris ramp progress on its 4 RGB LEDs and the button light.
 *
 * While a ramp runs: the number of lit LEDs is progress through the ramp
 * (1..4), and their colour is the measured temperature on a five-stop
 * blue -> violet -> magenta -> orange -> gold scale spanning this ramp's own
 * coolest-to-hottest stage, so every stage reads as a distinct colour. The
 * control button's light shows the same colour.
 *
 * Stock ring push FUN_000090bc (decompiled): drives the LEDs only when
 * 0x84308c+0xe != 0 -- the user's LED preset (the triple-click cycle, 0 = LEDs
 * off) -- or UI state 2; dims the buffer at 0x8431dc IN PLACE (3/10) in dim
 * mode (+0x1c) or under 21 % battery; sends it scaled by the animation level
 * 0x84562d (/100). This file reads the LED preset and never writes it.
 *
 * Measured temperature is +0x28 (F): +0x2a is Celsius on this device.
 */
#include "ramp.h"

#define LED_ENABLED      DEV_LEDS_ON()
#define LED_RGB          ((volatile u8 *)0x8431dc)   /* 4 x (R,G,B) */
#define LED_COUNT        4
#define led_push         STOCK_FN(void_fn, 0x90bc)

/* The stock dispatcher's animation state (FUN_0000920c, the same scheme as the
 * Sport's): +0 the effect, +1 the level every push is scaled by (0-100), +5
 * blinks still to play (3 queued at every session start, 0x745e), +6 a
 * fade-in, +7 a fade-out; with nothing queued it sets the level back to 100.
 * Effect 9 is the warning flash: +4 flashes, then effect 0 (low battery at
 * session start, faults, the dim-mode toggle). While any of that runs the
 * lights stay stock's, so cues and warnings play out and the level is never
 * frozen mid-blink -- it only moves while the dispatcher runs. */
#define LED_ANIM         ((volatile u8 *)0x84562c)
#define LED_EFFECT_FLASH 9
/* The dispatcher powers the LEDs by setting one GPIO output bit after every
 * push: the pin is the u16 at 0x84317c (port in the high byte, bit mask in
 * the low byte, chosen at boot by board revision), output register 0x800583 +
 * port * 8 -- and only while 0x843184 (in the OTA state next to the OTA
 * offset 0x843180) is clear. A session path of the dispatcher clears that bit,
 * so this file sets it the same way before each push, and leaves the lights
 * to stock while the flag is set. */
#define LED_RAIL_PIN     (*(volatile u16 *)0x84317c)
#define LED_RAIL_BLOCK   (*(volatile u8 *)0x843184)
#define GPIO_OUT_BASE    0x800583

/* The control button's light: an RGB LED on PB5/PB6/PB7, driven by a
 * software-PWM timer interrupt (0x49c: counter 0-99 against three duty
 * values, active low): 0x845602 red, 0x8455fc green, 0x8455fe blue, each
 * 0-100. The dispatcher sets them to colour * level / 100 after its ring
 * push. No DMA is involved, so its timing can't disturb the ring. */
#define BTN_DUTY_R   (*(volatile u16 *)0x845602)
#define BTN_DUTY_G   (*(volatile u16 *)0x8455fc)
#define BTN_DUTY_B   (*(volatile u16 *)0x8455fe)
#define stock_led_dispatch  STOCK_FN(void_fn, 0x920c)

static const u8 STOP_R[5] = {  60, 140, 220, 255, 255 };
static const u8 STOP_G[5] = {  90,  70,  60, 120, 215 };
static const u8 STOP_B[5] = { 255, 230, 160,  60,  60 };

static u8 stock_animating(void)
{
    return LED_ANIM[5] || LED_ANIM[6] || LED_ANIM[7] || LED_ANIM[1] < 100 ||
           LED_ANIM[0] == LED_EFFECT_FLASH;
}

/* 0-255 colour -> 0-100 duty (the level is 100 whenever this draws) */
static void button(u8 r, u8 g, u8 b)
{
    BTN_DUTY_R = (u16)rom_div(r * 100, 255);
    BTN_DUTY_G = (u16)rom_div(g * 100, 255);
    BTN_DUTY_B = (u16)rom_div(b * 100, 255);
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
    if (enabled)
        button(RAMP_PRESET_RGB[sel][0], RAMP_PRESET_RGB[sel][1], RAMP_PRESET_RGB[sel][2]);
    else
        button(RAMP_OFF_R, RAMP_OFF_G, RAMP_OFF_B);
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
    button(r, g, b);
}

/* The mode-switch cue (five presses, the fifth held): Terpline is the logo's
 * flame sweep across the LEDs -- cyan, green, yellow, orange -- with a green
 * button; Focus V is plain white. */
static const u8 CUE_R[4] = {   0,  40, 240, 255 };
static const u8 CUE_G[4] = { 190, 200, 220, 135 };
static const u8 CUE_B[4] = { 210,  70,  20,   0 };

static void show_cue(u8 cue)
{
    int i, x, seg, t;
    for (i = 0; i < LED_COUNT; i++) {
        u8 r = 200, g = 200, b = 200;
        if (cue == RAMP_CUE_TERPLINE) {
            x = rom_div(i * 3 * 256, LED_COUNT - 1); seg = x >> 8; t = x & 0xff;
            if (seg > 2) { seg = 2; t = 255; }
            r = (u8)(CUE_R[seg] + (((CUE_R[seg + 1] - CUE_R[seg]) * t) >> 8));
            g = (u8)(CUE_G[seg] + (((CUE_G[seg + 1] - CUE_G[seg]) * t) >> 8));
            b = (u8)(CUE_B[seg] + (((CUE_B[seg + 1] - CUE_B[seg]) * t) >> 8));
        }
        LED_RGB[i * 3 + 0] = r;
        LED_RGB[i * 3 + 1] = g;
        LED_RGB[i * 3 + 2] = b;
    }
    if (cue == RAMP_CUE_TERPLINE)
        button(40, 200, 70);
    else
        button(200, 200, 200);
}

/* After every ramp tick: the picker only shows on the idle screen. */
void ramp_led_tick(void)
{
    volatile ramp_state_t *st = RAMP_STATE;
    if (st->picker_on && !ramp_active(st) && !DEV_IDLE())
        ramp_picker_close(st);   /* asleep, or off the idle state: stop showing it */
}

/* Installed at 0x61ae, the only call of the stock LED effect dispatcher 0x920c
 * (main loop, every tick). While a ramp or the picker owns the lights, fill
 * both, power the LEDs and push the ring the way the dispatcher's own tail
 * does; otherwise -- LEDs off, a stock cue playing, OTA, nothing to show --
 * run the dispatcher unchanged, which also puts the stock colours back. The
 * buffer is refilled before every push, so 0x90bc's in-place dimming never
 * compounds. */
void ramp_led_entry(void)
{
    volatile ramp_state_t *st = RAMP_STATE;
    u16 pin;

    /* The mode-switch cue shows in either mode, for its 1.5 s. */
    if (st->magic == RAMP_MAGIC && st->mode_cue) {
        if (DEV_SYS_TICK - st->mode_cue_t0 >= RAMP_MODE_CUE_TICKS)
            st->mode_cue = 0;
        else if (LED_ENABLED != 0 && LED_RAIL_BLOCK == 0 && !stock_animating()) {
            show_cue(st->mode_cue);
            pin = LED_RAIL_PIN;
            *(volatile u8 *)(GPIO_OUT_BASE + (pin >> 8) * 8) |= (u8)pin;
            led_push();
            return;
        }
    }

    if (st->magic != RAMP_MAGIC || st->stock_mode || LED_ENABLED == 0 || LED_RAIL_BLOCK != 0 || stock_animating()) {
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
    pin = LED_RAIL_PIN;
    *(volatile u8 *)(GPIO_OUT_BASE + (pin >> 8) * 8) |= (u8)pin;
    led_push();
}
