    /* ramp_marker_entry: installed in place of the two stock instructions
     * at 0xb002 ("tmovs r3,#0x35" / "tloadrb r3,[r7,r3]") that load the
     * incoming BLE packet's marker byte, immediately before the stock
     * firmware's own 0xA5/0xAF/0x66 compare chain. Same design as the
     * Carta 2 and Aeris patches' marker_entry.s, adapted to this build's
     * addresses (register role confirmed identical: r7 holds the packet
     * buffer here too). Replicates the marker load both before and after
     * calling the C dispatcher, so r3 is guaranteed correct on return
     * regardless of what the dispatcher does internally. */
    .text
    .global ramp_marker_entry
ramp_marker_entry:
    tpush {lr}
    tmovs r3, #0x35
    tloadrb r3, [r7, r3]
    tmov r0, r3
    tjl ramp_marker_dispatch
    tmovs r3, #0x35
    tloadrb r3, [r7, r3]
    tpop {pc}
