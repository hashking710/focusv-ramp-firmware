/* ramp_event.c -- Aeris button events: the ramp preset picker.
 *
 * Installed at 0x645c, in place of the stock call to the button-event consumer
 * (0x4ee8), which is run unchanged for every event the picker doesn't take.
 * The picker is only ever entered from the idle screen with no session, so
 * stock clicks and holds are unaffected outside it. */
#include "ramp.h"

#define EV_MB                ((volatile u8 *)DEV_EV_MB)
#define orig_event_consumer  STOCK_FN(void_fn, DEV_EV_CONSUMER)

void ramp_event_entry(void)
{
    volatile u8 *mb = EV_MB;

    if (mb[1] && !DEV_EV_IGNORED()) {
        if (ramp_picker_event(RAMP_STATE, mb[0], DEV_IDLE(), DEV_PICK_ENTER, DEV_PICK_NEXT,
                              DEV_PICK_PREV, DEV_PICK_EXIT, DEV_PICK_TOGGLE))
            mb[1] = 0;
    }
    orig_event_consumer();
}
