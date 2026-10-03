/* ramp_event.c -- Carta Sport button events: the ramp preset picker.
 *
 * Installed at 0x58a8, in place of the stock call to the button-event consumer
 * (0x45cc), which is run unchanged for every event the picker doesn't take.
 * The picker is only ever entered from the awake screen with no session, so
 * stock clicks, holds and the quadruple-click on/off switch are unaffected
 * outside it. */
#include "ramp.h"

#define EV_MB                ((volatile u8 *)DEV_EV_MB)
#define orig_event_consumer  STOCK_FN(void_fn, DEV_EV_CONSUMER)

void ramp_event_entry(void)
{
    volatile u8 *mb = EV_MB;

    if (mb[1]) {
        u8 idle = *(volatile u8 *)DEV_UI_STATE_ADDR == 1 && STRUCT_BASE[OFF_SESSION] == 0;
        if (ramp_picker_event(RAMP_STATE, mb[0], idle,
                              DEV_PICK_ENTER, DEV_PICK_NEXT, DEV_PICK_PREV, DEV_PICK_EXIT))
            mb[1] = 0;
    }
    orig_event_consumer();
}
