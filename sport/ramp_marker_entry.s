    /* ramp_marker_entry: installed at 0xb002 in place of the two stock instructions
     * that load the 0xCC packet's marker byte ("tmovs r3,#0x35 / tloadrb r3,[r7,r3]"),
     * just before the stock A5/AF/66 compare chain. Calls
     * ramp_marker_dispatch(marker), then returns with r3 = marker, exactly as
     * those two instructions would.
     *
     * Register contract: the replaced instructions modify only r3, so r0-r2
     * (free for the C code to clobber) are saved and restored around the call.
     * On the Sport r0-r2 are all overwritten before use after 0xb002; they're preserved anyway, at no cost.
     * Flags are not live (the chain compares immediately). The call into C is a
     * PC-relative tjl; the return pops the hardware-set return address, as every
     * stock function does.
     *
     * Hooking the LOAD rather than a leaf of the compare chain is what lets the
     * new markers 0xB1-0xBA be seen at all: none of them match A5/AF/66. */
    .text
    .global ramp_marker_entry
ramp_marker_entry:
    tpush {r0, r1, r2, lr}
    tmovs r3, #0x35
    tloadrb r3, [r7, r3]
    tmov r0, r3
    tjl ramp_marker_dispatch
    tmovs r3, #0x35
    tloadrb r3, [r7, r3]
    tpop {r0, r1, r2, pc}
