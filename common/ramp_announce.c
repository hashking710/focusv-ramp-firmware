/* ramp_announce.c -- lets the app tell a patched device from a stock one.
 *
 * Installed in place of the stock call that sends the 0xAA dab-counter reply
 * (part of the sync burst the app triggers on connect). The reply is sent
 * unchanged, then a signature packet is queued for the next ticks:
 *
 *   [BC, 0C, 'T','R','M','P', protocol, device, enabled, preset, offset, BC]
 *
 * device: 1 Carta 2, 2 Aeris, 3 Sport. offset: signed F. Stock firmware never
 * sends 0xBC, and nothing extra is sent to a stock device. Sent from the tick
 * rather than right after the 0xAA reply because the notify queue can refuse
 * a second packet sent immediately (it returns non-zero, and is retried). */
#include "ramp.h"

#define ANNOUNCE_OP        0xbc
#define ANNOUNCE_LEN       12
#define ANNOUNCE_PROTOCOL  1
#define ANNOUNCE_TRIES     50

typedef int (*notify_fn)(int handle, const u8 *data, int len);
#define stock_notify  STOCK_FN(notify_fn, DEV_NOTIFY)

int ramp_announce_entry(int handle, const u8 *data, int len)
{
    int r = stock_notify(handle, data, len);
    volatile ramp_state_t *st = RAMP_STATE;
    if (st->magic == RAMP_MAGIC)
        st->announce = ANNOUNCE_TRIES;
    return r;
}

void ramp_announce_tick(volatile ramp_state_t *st)
{
    u8 pkt[ANNOUNCE_LEN];

    if (st->announce == 0)
        return;
    pkt[0] = ANNOUNCE_OP;
    pkt[1] = ANNOUNCE_LEN;
    pkt[2] = 'T'; pkt[3] = 'R'; pkt[4] = 'M'; pkt[5] = 'P';
    pkt[6] = ANNOUNCE_PROTOCOL;
    pkt[7] = DEV_ID;
    pkt[8] = ramp_enabled() ? 1 : 0;
    pkt[9] = ramp_selected();
    pkt[10] = (u8)ramp_offset();
    pkt[11] = ANNOUNCE_OP;
    if (stock_notify(DEV_NOTIFY_HANDLE, pkt, ANNOUNCE_LEN) == 0)
        st->announce = 0;
    else
        st->announce--;
}
