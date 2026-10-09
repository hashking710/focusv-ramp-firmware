/* ramp_input.c -- Carta 2 buttons.
 *
 * Installed at 0x6d0c, the only call to the stock button-event consumer
 * FUN_00005618 (mailbox 0x84319c: +0 event, +1 pending). Decoded from the
 * gesture decoder and the edit-screen handlers: event 7 = main button single
 * click, 2 / 1 = + / - short press, 4 / 3 = + / - held (auto-repeat),
 * 8 = double click (stock: +10 s).
 *
 * While a ramp is active (everything else is stock):
 *   single click -> passed to stock untouched, exactly as in a stock session.
 *                   A ramp is a stock session started on screen 1 (every
 *                   start sets it: 0x5a9e and the others) and nothing here
 *                   changes the screen, so on the heating screen the click
 *                   stops (handler 0x5708 -> stop 0x97f0 while +0x1 is 1),
 *                   and on the screensaver it does whatever stock does there
 *                   (it brings the main screen back -- seen on the device).
 *                   An earlier version forced screen 1 / 5 first; forcing
 *                   would turn that wake click into a stop. (The consumer's
 *                   screen table pointer 0x1a3c0 is a runtime address: the
 *                   table is at disassembly 0x1a398 -- entries 4 and 14 both
 *                   point at 0x5740, which tests for exactly those screens.)
 *   + / - short  -> next / previous stage (stock: open the temperature editor,
 *                   screen 6, and clear "reached" -- 0x610a);
 *                   locked, passed to stock, which ignores them in a session
 *                   (0x5bfc -> 0x5c06 -> 0x5728 -> return), so the lock holds
 *   + / - held   -> ignored (auto-repeat would skip every stage)
 *   everything else -> taken, except events whose stock effect is safe
 *                   mid-ramp: 7 / 17 (stop), 11 (power off), 14 (a press:
 *                   nothing on screen 1), 18 (app +10 s: the ramp undoes the
 *                   countdown change), 19-21 (system, overlays). The Carta 2
 *                   only sets "reached" on screen 1 (0xb21c -> 0xb58c) and the
 *                   ramp clears it at every stage, so nothing may move the
 *                   screen off 1 mid-ramp. Taken, with their stock effect in a
 *                   session: 3 / 4 (held +/-: temperature editor, screen 6),
 *                   8 / 16 (+10 s / start: would rewind the ramp clock), 9
 *                   (triple click: menu index, view 11), 10 (four clicks:
 *                   lock -> lock prompt, screen 12 -- and a locked click
 *                   can't stop), 12 (four clicks + hold: low power, screen
 *                   10), 13 (long hold: next preset rank -- 0x61f6 / 0x6236
 *                   -- and screen 8, which would move the active slot under
 *                   the ramp), 6 / 15.
 *
 * Preset picker (ramp_picker.c), when no ramp is running: a single click on
 * the idle live view, with the device unlocked, opens it (see
 * DEV_PICKER_SCREEN in device.h -- the only button stock leaves unused there);
 * + / - step through the presets, a double click switches the ramp system on
 * or off, a click leaves. Events it takes reach the stock consumer as 6, which
 * the live view ignores (0x5bf6), so the consumer's prelude still counts them
 * as activity (keep-awake +0x57 / +0x58, auto-off +0xb0 / +0xb4). It's drawn
 * from here on every change and from the 0xce70 hook whenever stock redraws
 * the live view. Leaving redraws the live view the way stock does (clear, then
 * 0xfa1c).
 */
#include "ramp.h"

#define EVENT_MAILBOX        ((volatile u8 *)0x84319c)
#define orig_event_consumer  STOCK_FN(void_fn, 0x5618)

/* Events the picker never takes: 11 is power on / off (five clicks, and the
 * firmware's own request at 0xfee8); 16 / 17 / 18 are the app's A5 / AF / 66
 * markers (0x125f2, 0x125e6, 0x12726), and 19+ are other system events. 14,
 * posted on every press of any button, is taken like the click it precedes. */
#define EV_POWER        11
#define EV_SYSTEM_MIN   16
#define EV_NOOP         6

typedef void (*clear_fn)(unsigned int y, unsigned int h, unsigned int color);
#define stock_clear      STOCK_FN(clear_fn, 0x7734)
#define stock_live_view  STOCK_FN(void_fn, 0xfa1c)

/* Back to the stock live view, exactly as stock draws it (0x5aa4: clear the
 * whole screen, then 0xfa1c). */
static void home_redraw(void)
{
    if (DEV_PICKER_SCREEN() && STRUCT_BASE[OFF_SESSION] == 0) {
        stock_clear(0, 239, 0);
        stock_live_view();
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
        if ((ev == 2 || ev == 1) && STRUCT_BASE[OFF_LOCKED] == 0) {
            mb[1] = 0;
            ramp_step(st, ev == 2 ? 1 : -1);
            return;
        }
        /* Everything else that could leave screen 1 or move the active slot
         * is taken; only events whose stock effect is safe mid-ramp pass. */
        if (!(ev == 7 || ev == 11 || ev == 14 || ev == 17 || ev >= 18 ||
              ev == 1 || ev == 2)) {
            mb[1] = 0;
            return;
        }
    }

    if (mb[1]) {
        u8 ev = mb[0];
        u8 idle = DEV_PICKER_SCREEN() && STRUCT_BASE[OFF_SESSION] == 0 &&
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
