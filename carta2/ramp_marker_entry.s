    /* ramp_marker_entry: installed in place of the two stock instructions
     * at 0x11d96 ("tmovs r3,#0x28" / "tloadrb r3,[r5,r3]") that load the
     * incoming BLE packet's marker byte, immediately before the stock
     * firmware's own 0xA5/0xAF/0x66 compare chain. Replicates that same
     * load (both before AND after calling our C dispatcher, so r3 is
     * guaranteed correct on return regardless of what the dispatcher does
     * internally), calls ramp_marker_dispatch(marker) in between, then
     * returns to the stock chain exactly where it would have continued
     * anyway -- the untouched 0xA5/0xAF/0x66 handling downstream never
     * knows this ran. See ramp_save.c for why this replaces the marker
     * LOAD rather than any one leaf of the compare chain: only the load
     * runs unconditionally for every marker value, which the new
     * 0xB1-0xB5 ramp-waypoint markers need, since none of them match any
     * of the three existing comparisons. */
    .text
    .global ramp_marker_entry
ramp_marker_entry:
    tpush {lr}
    tmovs r3, #0x28
    tloadrb r3, [r5, r3]
    tmov r0, r3
    tjl ramp_marker_dispatch
    tmovs r3, #0x28
    tloadrb r3, [r5, r3]
    tpop {pc}
