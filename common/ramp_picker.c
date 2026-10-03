/* ramp_picker.c -- choosing a built-in ramp preset on the device, shared by
 * every device. Each device supplies its own event codes and idle test in its
 * event hook (carta2/ramp_input.c, aeris/ramp_event.c, sport/ramp_event.c).
 *
 *   idle + enter        -> select mode on; the chosen preset is shown
 *   select: next / prev -> step through the presets (wraps)
 *   select: exit        -> leave, writing the choice to flash if it changed
 *   select: anything else is swallowed, so the stock handler never sees it
 *
 * Nothing starts from select mode: starting a ramp is still the stock sentinel
 * session. A ramp that is already running always takes precedence. */
#include "ramp.h"

u8 ramp_picker_event(volatile ramp_state_t *st, int ev, u8 idle,
                     int enter, int next, int prev, int exit)
{
    if (st->magic != RAMP_MAGIC)
        return 0;

    if (ramp_active(st) || !ramp_enabled()) {
        st->picker_on = 0;
        return 0;
    }

    if (st->picker_on) {
        if (ev == exit) {
            if (st->picker_dirty)
                ramp_store_set(RAMP_SEL_OFFSET, st->picker_sel);
            st->picker_on = 0;
            st->picker_dirty = 0;
            return 1;
        }
        if (ev == next) {
            st->picker_sel = (u8)(st->picker_sel + 1 < DEV_PICK_COUNT ? st->picker_sel + 1 : 0);
            st->picker_dirty = 1;
        } else if (ev == prev) {
            st->picker_sel = (u8)(st->picker_sel > 0 ? st->picker_sel - 1 : DEV_PICK_COUNT - 1);
            st->picker_dirty = 1;
        }
        return 1;
    }

    if (idle && ev == enter) {
        st->picker_on = 1;
        st->picker_sel = ramp_selected();
        st->picker_dirty = 0;
        return 1;
    }
    return 0;
}
