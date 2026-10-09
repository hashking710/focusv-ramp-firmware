/* ramp_announce.c -- lets the app tell a patched device from a stock one.
 *
 * Installed in place of the stock call that sends the 0xAA dab-counter reply
 * (part of the sync burst the app triggers on connect). The reply is sent
 * unchanged, then a signature packet is queued for the next ticks:
 *
 *   [BC, 0C, 'T','R','M','P', protocol, device, flags, preset, offset, BC]
 *
 * device: 1 Carta 2, 2 Aeris, 3 Sport. flags: bit 0 ramps enabled, bit 1
 * stock mode (protocol 2+), bit 2 a ramp is running (protocol 3; protocol 1
 * sent 0 / 1 = enabled there). offset: signed F. It's also queued after a
 * mode switch and whenever a ramp starts or ends -- the status packet can't
 * tell an app that: its session byte is the countdown's low byte, which reads
 * 0 at every multiple of 256 s of a ramp. Stock firmware never
 * sends 0xBC, and nothing extra is sent to a stock device. Sent from the tick
 * rather than right after the 0xAA reply because the notify queue refuses
 * packets while the sync burst fills it (non-zero return); the fields are read
 * once, here, so the retries don't touch flash. */
#include "ramp.h"

#define ANNOUNCE_OP        0xbc
#define ANNOUNCE_LEN       12
#define ANNOUNCE_PROTOCOL  3
#define ANNOUNCE_ENABLED   0x01
#define ANNOUNCE_STOCK     0x02
#define ANNOUNCE_RAMP      0x04
/* Main-loop passes to keep retrying: the sync burst fills the notify queue
 * and it drains over several connection intervals, so this has to outlast that. */
#define ANNOUNCE_TRIES     0xffff

typedef int (*notify_fn)(int handle, const u8 *data, int len);
#define stock_notify  STOCK_FN(notify_fn, DEV_NOTIFY)

/* Captures the fields now and leaves the sending to ramp_announce_tick. */
void ramp_announce_queue(volatile ramp_state_t *st)
{
    st->ann_flags = (ramp_enabled() ? ANNOUNCE_ENABLED : 0) | (st->stock_mode ? ANNOUNCE_STOCK : 0) |
                    (ramp_active(st) ? ANNOUNCE_RAMP : 0);
    st->ann_preset = ramp_selected();
    st->ann_offset = (u8)ramp_offset();
    st->ann_tries = ANNOUNCE_TRIES;
}

int ramp_announce_entry(int handle, const u8 *data, int len)
{
    int r = stock_notify(handle, data, len);
    volatile ramp_state_t *st = RAMP_STATE;
    if (st->magic == RAMP_MAGIC)
        ramp_announce_queue(st);
    return r;
}

void ramp_announce_tick(volatile ramp_state_t *st)
{
    u8 pkt[ANNOUNCE_LEN];

    if (st->ann_tries == 0)
        return;
    pkt[0] = ANNOUNCE_OP;
    pkt[1] = ANNOUNCE_LEN;
    pkt[2] = 'T'; pkt[3] = 'R'; pkt[4] = 'M'; pkt[5] = 'P';
    pkt[6] = ANNOUNCE_PROTOCOL;
    pkt[7] = DEV_ID;
    pkt[8] = st->ann_flags;
    pkt[9] = st->ann_preset;
    pkt[10] = st->ann_offset;
    pkt[11] = ANNOUNCE_OP;
    if (stock_notify(DEV_NOTIFY_HANDLE, pkt, ANNOUNCE_LEN) == 0)
        st->ann_tries = 0;
    else
        st->ann_tries--;
}
