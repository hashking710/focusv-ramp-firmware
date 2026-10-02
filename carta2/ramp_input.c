/* ramp_input.c -- Carta 2 buttons during a ramp.
 *
 * Installed at 0x6d0c, the only call to the stock button-event consumer
 * FUN_00005618 (mailbox 0x84319c: +0 event, +1 pending). Decoded from the
 * gesture decoder and the edit-screen handlers: event 7 = main button single
 * click, 2 / 1 = + / - short press, 4 / 3 = + / - held (auto-repeat),
 * 8 = double click (stock: +10 s).
 *
 * While a ramp is active (everything else is stock):
 *   single click -> stop. Screen is forced to "heating" (1) so the stock
 *                   heating-screen handler (0x5708) performs the stop itself via
 *                   0x97f0, whatever screen was showing.
 *   + / - short  -> next / previous stage
 *   + / - held   -> ignored (auto-repeat would skip every stage)
 *   double click -> ignored (stock +10 s would rewind the ramp clock)
 * Up/down are consumed rather than passed on, so the stock edit screen -- which
 * would rewrite the active preset slot mid-ramp -- can't be entered.
 */
#include "ramp.h"

#define EVENT_MAILBOX        ((volatile u8 *)0x84319c)
#define orig_event_consumer  STOCK_FN(void_fn, 0x5618)

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
            STRUCT_BASE[OFF_SCREEN] = 1;
    }
    orig_event_consumer();
}
