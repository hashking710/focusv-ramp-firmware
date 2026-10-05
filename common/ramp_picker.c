/* ramp_picker.c -- choosing a built-in ramp preset and switching the ramp
 * system on or off on the device, shared by every device. Each device supplies
 * its own event codes and idle test in its event hook (carta2/ramp_input.c,
 * aeris/ramp_event.c, sport/ramp_event.c).
 *
 *   idle + enter        -> picker opens (also when the system is off)
 *   picker: next / prev -> step through the presets (wraps)
 *   picker: toggle      -> ramp system on / off (flash, survives power-off)
 *   picker: exit        -> leave, writing the choice to flash if it changed
 *   picker: anything else is swallowed, so the stock handler never sees it
 *
 * It closes after 30 s with no button event (ramp_core.c), keeping the choice.
 * It also closes, passing the event through, as soon as the device is no
 * longer idle (screen change, sleep, a session) or a ramp is running. Nothing
 * starts from the picker: a ramp still starts as a stock sentinel session. */
#include "ramp.h"

void ramp_picker_close(volatile ramp_state_t *st)
{
    if (st->picker_dirty && st->picker_sel != ramp_selected())
        ramp_store_set(RAMP_SEL_OFFSET, st->picker_sel);
    st->picker_on = 0;
    st->picker_dirty = 0;
}

u8 ramp_picker_event(volatile ramp_state_t *st, int ev, u8 idle,
                     int enter, int next, int prev, int exit, int toggle)
{
    if (st->magic != RAMP_MAGIC)
        return 0;

    if (ramp_active(st)) {
        st->picker_on = 0;
        st->picker_dirty = 0;
        return 0;
    }

    if (st->picker_on) {
        st->picker_t0 = DEV_SYS_TICK;
        if (!idle) {
            ramp_picker_close(st);
            return 0;
        }
        if (ev == exit) {
            ramp_picker_close(st);
        } else if (ev == toggle) {
            ramp_toggle_enabled();
            st->picker_enabled = ramp_enabled();
        } else if (ev == next) {
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
        st->picker_enabled = ramp_enabled();
        st->picker_dirty = 0;
        st->picker_t0 = DEV_SYS_TICK;
        return 1;
    }
    return 0;
}
