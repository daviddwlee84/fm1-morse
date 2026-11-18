/* SPDX-License-Identifier: MIT */
/* hostlink: see hostlink.h */
#include "hostlink.h"

void hl_init(hostlink_t *h, uint8_t id0, uint8_t id1, uint8_t ver)
{
    h->id0 = id0;
    h->id1 = id1;
    h->ver = ver;
    h->acked = 0;
    h->transport = HL_USB;
    h->seen = h->sent = h->session = h->usb_shut = 0;
}

const uint8_t *hl_match(const hostlink_t *h, const uint8_t *p, uint32_t n, uint8_t *type, uint32_t *len)
{
    if (n < 5u || p[0] != 0x7Du || p[1] != h->id0 || p[2] != h->id1 || p[3] != h->ver)
        return 0;
    *type = p[4];
    *len = n - 5u;
    return p + 5;
}

void hl_seen(hostlink_t *h, uint32_t now) { h->seen = now ? now : 1u; }

void hl_acked(hostlink_t *h, uint32_t now, int transport)
{
    hl_seen(h, now);
    h->acked = 1;
    h->transport = (uint8_t)(transport == HL_BLE ? HL_BLE : HL_USB);
    h->session = plat_usb_session();
}

int hl_host(const hostlink_t *h, uint32_t now) { return h->acked && h->seen && now - h->seen < HOSTLINK_TIMEOUT_MS; }

/* ---------------------------------------------------------- USB-MIDI SysEx packets --- */
int hl_sx_packet(hl_sx_t *s, uint32_t pkt, const uint8_t **frame, uint32_t *n)
{
    uint32_t cin = pkt & 0x0Fu, k, nb;
    if (cin == 0x4u || cin == 0x7u)
        nb = 3;
    else if (cin == 0x6u)
        nb = 2;
    else if (cin == 0x5u)
        nb = 1;
    else
        return 0;                        /* not SysEx */
    for (k = 0; k < nb; k++) {
        uint8_t b = (uint8_t)(pkt >> (8u + 8u * k));
        if (b == 0xF0u) {
            s->open = 1;
            s->over = 0;
            s->n = 0;
        } else if (b == 0xF7u) {
            int whole = s->open && !s->over;
            s->open = 0;
            if (whole) {
                *frame = s->b;
                *n = s->n;
                return 1;
            }
        } else if (s->open) {
            if (s->n < HOSTLINK_RX_MAX)
                s->b[s->n++] = b;
            else
                s->over = 1;
        }
    }
    return 0;
}

uint32_t hl_sx_packets(const uint8_t *f, uint32_t n, uint32_t *out, uint32_t max)
{
    uint32_t i = 0, k = 0;
    while (i < n && k < max) {
        uint32_t left = n - i, take = left > 3u ? 3u : left, cin, j, pkt;
        if (left > 3u)
            cin = 0x4u;                  /* start / continue */
        else
            cin = 0x4u + take;           /* the end: 5, 6, 7 for 1, 2, 3 bytes */
        pkt = cin;
        for (j = 0; j < take; j++)
            pkt |= (uint32_t)f[i + j] << (8u + 8u * j);
        out[k++] = pkt;
        i += take;
    }
    return k;
}

/* ------------------------------------------------------------------- sending --- */
int hl_send_to(hostlink_t *h, int transport, uint8_t type, const uint8_t *payload, uint32_t n)
{
    uint8_t f[HOSTLINK_MAX + 7u];
    uint32_t i, k = 0;
    if (n > HOSTLINK_MAX)
        return -1;
    f[k++] = 0xF0;
    f[k++] = 0x7D;
    f[k++] = h->id0;
    f[k++] = h->id1;
    f[k++] = h->ver;
    f[k++] = type & 0x7Fu;
    for (i = 0; i < n; i++)
        f[k++] = payload[i] & 0x7Fu;
    f[k++] = 0xF7;
    if (transport == HL_BLE) {
#if FM1B_BLE
        uint32_t pk[(HOSTLINK_MAX + 7u + 2u) / 3u + 1u], np = hl_sx_packets(f, k, pk, sizeof pk / sizeof pk[0]);
        if (!plat_ble_connected() || plat_midi_out_room(TO_BLE_HOST) < np)
            return -1;                   /* whole or not at all: a frame cut short is worse than one late */
        for (i = 0; i < np; i++)
            plat_midi_send(pk[i], TO_BLE_HOST);
        return 0;
#else
        return -1;
#endif
    }
    return plat_sysex_send(f, k);
}

int hl_send(hostlink_t *h, uint8_t type, const uint8_t *payload, uint32_t n)
{
    return hl_send_to(h, h->transport, type, payload, n);
}

void hl_tick(hostlink_t *h, uint32_t now, uint8_t hello, uint8_t heartbeat, const uint8_t *hello_p, uint32_t hello_n)
{
    if (h->acked && !hl_host(h, now))
        h->acked = 0;                    /* lost: say HELLO again */
    if (h->acked && h->transport == HL_USB && plat_usb_session() != h->session)
        h->acked = 0;                    /* a new cable */
    if (h->acked && h->transport == HL_USB) {   /* the cable out leaves the session as it was, the bus suspended */
        if (plat_sysex_room())
            h->usb_shut = 0;
        else if (!h->usb_shut)
            h->usb_shut = now ? now : 1u;
        else if (now - h->usb_shut >= HOSTLINK_USB_GONE_MS)
            h->acked = 0, h->usb_shut = 0;
    } else
        h->usb_shut = 0;
#if FM1B_BLE
    if (h->acked && h->transport == HL_BLE && !plat_ble_connected())
        h->acked = 0;                    /* the Bluetooth link went */
#endif
    if (h->sent && now - h->sent < HOSTLINK_EVERY_MS)
        return;
    h->sent = now ? now : 1u;
    if (h->acked) {
        uint8_t hb[2] = {h->ver, h->transport};
        hl_send(h, heartbeat, hb, 2);
        return;
    }
    {
        uint8_t b[HOSTLINK_MAX];             /* the app's HELLO, then the transport it goes by */
        uint32_t i;
        if (hello_n >= HOSTLINK_MAX)
            return;
        for (i = 0; i < hello_n; i++)
            b[i] = hello_p[i];
        if (plat_usb_session()) {
            b[hello_n] = HL_USB;
            hl_send_to(h, HL_USB, hello, b, hello_n + 1u);
        }
#if FM1B_BLE
        if (plat_ble_connected()) {
            b[hello_n] = HL_BLE;
            hl_send_to(h, HL_BLE, hello, b, hello_n + 1u);
        }
#endif
    }
}
