/* SPDX-License-Identifier: MIT */
/* hostlink's Bluetooth half on the computer: frames cut into USB-MIDI SysEx packets and put back together.
 *   cc -I.. -o /tmp/hostlink_test hostlink_test.c && /tmp/hostlink_test      (just fw-common-test) */
#include <stdio.h>
#include <string.h>
#define FM1B_BLE 0
static unsigned plat_usb_session(void) { return 1; }
static int plat_sysex_send(const unsigned char *f, unsigned n) { (void)f, (void)n; return 0; }
#include "../hostlink.c"

static int fails;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL %s\n", msg); fails++; } } while (0)

static void round_trip(uint32_t len)
{
    uint8_t f[HOSTLINK_RX_MAX + 2];
    uint32_t pk[260], np, i, n = 0, got = 0;
    const uint8_t *out = 0;
    hl_sx_t s;
    memset(&s, 0, sizeof s);
    f[0] = 0xF0;
    for (i = 0; i < len; i++)
        f[1 + i] = (uint8_t)((i * 37u + 5u) & 0x7Fu);
    f[1 + len] = 0xF7;
    np = hl_sx_packets(f, len + 2, pk, 260);
    CHECK(np == (len + 2 + 2) / 3, "packet count");
    for (i = 0; i < np; i++) {
        uint32_t cin = pk[i] & 0xFu;
        CHECK(i + 1 < np ? cin == 4 : cin >= 5 && cin <= 7, "CIN: 4 until the last, then 5 / 6 / 7");
        if (i == np / 2)
            hl_sx_packet(&s, 0x0090u | 0x3C00u << 8, &out, &n);   /* a note-on in between: not SysEx, ignored */
        got += (uint32_t)hl_sx_packet(&s, pk[i], &out, &n);
    }
    CHECK(got == 1 && n == len && !memcmp(out, f + 1, len), "the same bytes back");
}

int main(void)
{
    uint32_t len;
    for (len = 0; len <= 20; len++)
        round_trip(len);
    round_trip(551);                       /* a DETAIL picture line between F0 and F7 */
    round_trip(HOSTLINK_RX_MAX);
    {                                      /* one byte too long: dropped whole, and the next frame is fine */
        uint8_t f[HOSTLINK_RX_MAX + 3];
        uint32_t pk[260], np, i, n = 0, got = 0;
        const uint8_t *out = 0;
        hl_sx_t s;
        memset(&s, 0, sizeof s);
        memset(f, 0x11, sizeof f);
        f[0] = 0xF0, f[HOSTLINK_RX_MAX + 2] = 0xF7;
        np = hl_sx_packets(f, sizeof f, pk, 260);
        for (i = 0; i < np; i++)
            got += (uint32_t)hl_sx_packet(&s, pk[i], &out, &n);
        CHECK(got == 0, "an over-long frame is dropped");
        np = hl_sx_packets((const uint8_t *)"\xF0\x7D\x43\x4E\xF7", 5, pk, 260);
        for (i = 0; i < np; i++)
            got += (uint32_t)hl_sx_packet(&s, pk[i], &out, &n);
        CHECK(got == 1 && n == 3, "the next frame after it");
    }
    printf(fails ? "hostlink: %d FAILED\n" : "hostlink: all ok\n", fails);
    return fails != 0;
}
