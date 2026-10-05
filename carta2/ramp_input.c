/* ramp_input.c -- Carta 2 buttons.
 *
 * Installed at 0x6d0c, the only call to the stock button-event consumer
 * FUN_00005618 (mailbox 0x84319c: +0 event, +1 pending). Decoded from the
 * gesture decoder and the edit-screen handlers: event 7 = main button single
 * click, 2 / 1 = + / - short press, 4 / 3 = + / - held (auto-repeat),
 * 8 = double click (stock: +10 s).
 *
 * While a ramp is active (everything else is stock):
 *   single click -> stop. Screen is forced to 5 (the live heating screen). Its
 *                   stock handler at 0x5954 runs the stop routine 0x97f0 on
 *                   event 7 and returns to idle. Screen 1, used before, is a
 *                   plain return in the consumer's table: it never stopped.
 *   + / - short  -> next / previous stage
 *   + / - held   -> ignored (auto-repeat would skip every stage)
 *   double click -> ignored (stock +10 s would rewind the ramp clock)
 * Up/down are consumed rather than passed on, so the stock edit screen -- which
 * would rewrite the active preset slot mid-ramp -- can't be entered.
 *
 * Preset picker (ramp_picker.c), when no ramp is running: a hold of - on the
 * idle screen (0) opens it; + / - step through the presets, a double click
 * switches the ramp system on or off, a click leaves. It's drawn from here on
 * every change (stock doesn't redraw the idle screen for consumed events) and
 * from the 0xce70 hook when stock redraws it.
 */
#include "ramp.h"

#define EVENT_MAILBOX        ((volatile u8 *)0x84319c)
#define orig_event_consumer  STOCK_FN(void_fn, 0x5618)

#define SCREEN_HEATING 5

typedef void (*draw_screen_fn)(unsigned int screen);
#define stock_draw_screen STOCK_FN(draw_screen_fn, 0xf338)

/* The picker timed out (ramp_core.c): put the stock idle screen back. */
void ramp_picker_closed_redraw(void)
{
    if (STRUCT_BASE[OFF_SCREEN] == 0)
        stock_draw_screen(0);
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
        u8 idle = STRUCT_BASE[OFF_SCREEN] == 0 && STRUCT_BASE[OFF_SESSION] == 0;
        u8 was_on = st->picker_on;
        if (ramp_picker_event(st, mb[0], idle, DEV_PICK_ENTER, DEV_PICK_NEXT,
                              DEV_PICK_PREV, DEV_PICK_EXIT, DEV_PICK_TOGGLE)) {
            mb[1] = 0;
            if (st->picker_on)
                ramp_picker_draw(st->picker_sel, st->picker_enabled);
            else if (was_on)
                stock_draw_screen(0);
            return;
        }
    }
    orig_event_consumer();
}
