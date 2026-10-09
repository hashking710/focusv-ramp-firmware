/* ramp_input.c -- Carta 2 buttons.
 *
 * Installed at 0x6d0c, the only call to the stock button-event consumer
 * FUN_00005618 (mailbox 0x84319c: +0 event, +1 pending). Decoded from the
 * gesture decoder and the edit-screen handlers: event 7 = main button single
 * click, 2 / 1 = + / - short press, 4 / 3 = + / - held (auto-repeat),
 * 8 = double click (stock: +10 s).
 *
 * While a ramp is active (everything else is stock):
 *   single click -> stop. Screen is forced to 1, the heating screen every
 *                   stock session start sets (0x5a9e and the other starts):
 *                   its handler 0x5708 runs the stop routine 0x97f0 on event
 *                   7 or 17 while +0x1 (session) is 1. The consumer's screen
 *                   table pointer 0x1a3c0 is a runtime address -- the table
 *                   is at disassembly 0x1a398 (entries 4 and 14 both point
 *                   at 0x5740, which tests for exactly those two screens).
 *                   Read 0x28 bytes late it looks as if screen 5 stops and
 *                   screen 1 returns; screen 5 is really the edit screen
 *                   (0x57aa), where a click doesn't stop anything.
 *   + / - short  -> next / previous stage
 *   + / - held   -> ignored (auto-repeat would skip every stage)
 *   double click -> ignored (stock +10 s would rewind the ramp clock)
 * Up/down are consumed rather than passed on, so the stock edit screen -- which
 * would rewrite the active preset slot mid-ramp -- can't be entered.
 *
 * Preset picker (ramp_picker.c), when no ramp is running: a hold of - on home
 * or the "ready" prompt opens it (see DEV_HOME_SCREEN in device.h: with
 * two-step heat set, the press itself moves home to the prompt, where stock
 * ignores + and -); + / - step through the presets, a double click switches the
 * ramp system on or off, a click leaves. Events it takes reach the stock
 * consumer as 1 (- short), which home and the prompt ignore, so the consumer's
 * prelude still counts them as activity (keep-awake +0x57 / +0x58, auto-off
 * +0xb0 / +0xb4). It's drawn from here on every change and from the 0xce70 hook
 * whenever stock redraws home. Leaving it does what a stock click on the
 * prompt does (countdown 1): stock returns to home and redraws all of it.
 */
#include "ramp.h"

#define EVENT_MAILBOX        ((volatile u8 *)0x84319c)
#define orig_event_consumer  STOCK_FN(void_fn, 0x5618)

#define SCREEN_HEATING 1

/* Events the picker never takes: 11 is power on / off (five clicks, and the
 * firmware's own request at 0xfee8); 16 / 17 / 18 are the app's A5 / AF / 66
 * markers (0x125f2, 0x125e6, 0x12726), and 19+ are other system events. 14,
 * posted on every press of any button, is taken like the click it precedes. */
#define EV_POWER        11
#define EV_SYSTEM_MIN   16
#define EV_NOOP         1

/* Back to stock home: exactly what a click on the prompt does (0x587a). The
 * countdown expires on the next tick and 0x7044 sets screen 4 and redraws
 * view 18 in full, picker box included. */
static void home_redraw(void)
{
    if (DEV_HOME_SCREEN(STRUCT_BASE[OFF_SCREEN])) {
        STRUCT_BASE[OFF_SCREEN] = SCREEN_PROMPT;
        STRUCT_BASE[OFF_PROMPT_TIMER] = 1;
        STRUCT_BASE[OFF_PROMPT_TIMER + 1] = 0;
    }
}

/* The picker timed out (ramp_core.c). */
void ramp_picker_closed_redraw(void)
{
    home_redraw();
}

void ramp_event_entry(void)
{
    volatile u8 *mb = EVENT_MAILBOX;
    volatile ramp_state_t *st = RAMP_STATE;

    if (mb[1] && ramp_active(st)) {
        u8 ev = mb[0];
        if (ev == 2 || ev == 1) {
            mb[1] = 0;
            ramp_step(st, ev == 2 ? 1 : -1);
            return;
        }
        if (ev == 3 || ev == 4 || ev == 8 || ev == 16) {
            mb[1] = 0;
            return;
        }
        if (ev == 7)
            STRUCT_BASE[OFF_SCREEN] = SCREEN_HEATING;
    }

    if (mb[1]) {
        u8 ev = mb[0];
        u8 idle = DEV_HOME_SCREEN(STRUCT_BASE[OFF_SCREEN]) && STRUCT_BASE[OFF_SESSION] == 0 &&
                  ev != EV_POWER && ev < EV_SYSTEM_MIN;
        u8 was_on = st->picker_on;
        if (ramp_picker_event(st, ev, idle, DEV_PICK_ENTER, DEV_PICK_NEXT,
                              DEV_PICK_PREV, DEV_PICK_EXIT, DEV_PICK_TOGGLE)) {
            mb[0] = EV_NOOP;
            if (st->picker_on)
                ramp_picker_draw(st->picker_sel, st->picker_enabled);
            else if (was_on)
                home_redraw();
        }
    }
    orig_event_consumer();
}
