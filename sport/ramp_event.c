/* ramp_event.c -- Carta Sport button events: the ramp preset picker.
 *
 * Installed at 0x58a8, in place of the stock call to the button-event consumer
 * (0x45cc), which is run for every event. The picker is only ever entered from
 * the awake screen with no session and the LEDs on, by a hold from a single
 * press, so stock clicks, holds and multi-press gestures are unaffected
 * outside it. An event the picker takes reaches the consumer as a press
 * (DEV_EV_NOOP), so its prelude still counts it as activity. Power off (five
 * presses, the app's command, the sleep request) and the app's start / stop /
 * +10 s always reach the stock code, closing the picker if it is open. */
#include "ramp.h"

#define EV_MB                ((volatile u8 *)DEV_EV_MB)
#define orig_event_consumer  STOCK_FN(void_fn, DEV_EV_CONSUMER)

void ramp_event_entry(void)
{
    volatile u8 *mb = EV_MB;
    int ev = mb[0];

    if (mb[1] && !DEV_EV_IGNORED()) {
        u8 pickable = ev != DEV_EV_POWER_OFF && ev < DEV_EV_APP_MIN;
        u8 can_enter = DEV_EV_CLICKS() <= 1 && DEV_LEDS_ON() != 0;
        if (ramp_picker_event(RAMP_STATE, ev, DEV_IDLE() && pickable,
                              can_enter ? DEV_PICK_ENTER : -2, DEV_PICK_NEXT,
                              DEV_PICK_PREV, DEV_PICK_EXIT, DEV_PICK_TOGGLE))
            mb[0] = DEV_EV_NOOP;
    }
    orig_event_consumer();
}
