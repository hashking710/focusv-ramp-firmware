    /* ramp_click_entry: installed at disassembly 0x50d2 in place of the two
     * stock instructions that increment and store the LED-preset click
     * counter ("tadds r3, #1" / "tstorerb r3, [r4, #14]") -- struct 0x84308c,
     * the same field ramp_led.c reads as LED_ENABLED, confirmed to be a plain
     * 0-5 counter the device's own single-button handler increments on every
     * click and wraps back to 0 past 5 (its own code at disassembly 0x5112).
     *
     * ramp_click_dispatch (ramp_store.c) replicates the increment+store
     * itself in C -- hand-written arithmetic isn't used in this file, same as
     * every other entry point in this project -- then repurposes a count of
     * exactly 4 (see its own header). r3 is reloaded after the call so the
     * stock code that follows this site sees the same thing it always would:
     * the counter, now possibly reset to 0 if this was the toggle.
     *
     * Register contract: the replaced instructions modify only r3 (and
     * memory); r0-r2 are free for the stock code around this site and are
     * preserved anyway, at no cost. The call is a PC-relative tjl.
     */
    .text
    .global ramp_click_entry
ramp_click_entry:
    tpush {r0, r1, r2, lr}
    tmov r0, r4
    tmovs r1, #14
    tjl ramp_click_dispatch
    tloadrb r3, [r4, #14]
    tpop {r0, r1, r2, pc}
