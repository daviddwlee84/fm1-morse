/* SPDX-License-Identifier: MIT */
/* hostlink: an app's own SysEx conversation with a program on the computer, over fm1-base, by USB or Bluetooth.
 *
 * Frames are F0 7D <id0> <id1> <ver> <type> <payload...> F7. 7D is the MIDI non-commercial ID; pick
 * id0/id1 so you stay apart from fm1-base's own 7D 4C .. frames (Conductor uses 43 4E, "CN").
 * The device says HELLO once a second (on every transport that has a computer) until the host answers, then a
 * HEARTBEAT once a second on the transport the answer came by; the host counts as present while frames keep
 * coming (HOSTLINK_TIMEOUT_MS).
 *
 * Transports:
 *   USB  frames in through APP_SYSEX (app_sysex), out through plat_sysex_send.
 *   BLE  fm1-base's own stack (ble_stack = "own") turns BLE-MIDI SysEx into USB-MIDI SysEx packets (CIN 4..7) on
 *        PORT_BLE; hl_sx_packet puts them back together. Out: the frame cut into such packets, plat_midi_send to
 *        TO_BLE_HOST. (JieLi's stack drops incoming SysEx: Bluetooth needs the own stack.)
 *
 * Use (unity build: app.c includes hostlink.c; fm1.toml include = ["../common"]):
 *   static hostlink_t hl;  static hl_sx_t ble_rx;
 *   hl_init(&hl, 0x43, 0x4E, 1);
 *   int app_sysex(const uint8_t *p, uint32_t n) { return my_frame(p, n, HL_USB); }
 *   main loop: while (plat_midi_in(&pkt, &port)) if (port == PORT_BLE && hl_sx_packet(&ble_rx, pkt, &f, &n))
 *                  my_frame(f, n, HL_BLE);
 *   my_frame: d = hl_match(&hl, p, n, &t, &len); if (!d) return 0; hl_seen(&hl, now); on HELLO_ACK
 *             hl_acked(&hl, now, transport); ...
 *   each frame: hl_tick(&hl, now, T_HELLO, T_HEARTBEAT, hello_payload, n);
 * HELLO carries the app's payload and then the transport byte (HL_USB / HL_BLE); HEARTBEAT carries ver, transport.
 */
#pragma once
#include <stdint.h>

#define HOSTLINK_TIMEOUT_MS 10000u
#define HOSTLINK_EVERY_MS 1000u
#define HOSTLINK_MAX 64u                 /* the longest payload an app sends through hl_send */
#define HOSTLINK_RX_MAX 640u             /* the longest frame taken from Bluetooth (fm1-base's APP_SYSEX limit) */

enum { HL_USB, HL_BLE, HL_NTRANSPORTS };

typedef struct {
    uint8_t id0, id1, ver;
    uint8_t acked;                       /* the host answered our HELLO since it was last lost */
    uint8_t transport;                   /* HL_USB / HL_BLE: where the answer came from, where we reply */
    uint32_t seen;                       /* plat_ms of the last frame from the host (0: never) */
    uint32_t sent;                       /* plat_ms of the last HELLO / HEARTBEAT */
    uint32_t session;                    /* plat_usb_session when acked over USB: a new cable means a new HELLO */
} hostlink_t;

/* a SysEx frame put back together from USB-MIDI packets (Bluetooth) */
typedef struct {
    uint8_t b[HOSTLINK_RX_MAX];
    uint32_t n;
    uint8_t open, over;                  /* inside a frame; it ran past HOSTLINK_RX_MAX (dropped at its end) */
} hl_sx_t;

void hl_init(hostlink_t *h, uint8_t id0, uint8_t id1, uint8_t ver);
/* one of ours (p: the bytes between F0 and F7): its type and payload; 0 when it is someone else's */
const uint8_t *hl_match(const hostlink_t *h, const uint8_t *p, uint32_t n, uint8_t *type, uint32_t *len);
void hl_seen(hostlink_t *h, uint32_t now);
void hl_acked(hostlink_t *h, uint32_t now, int transport);
int hl_host(const hostlink_t *h, uint32_t now);   /* 1: the host is there */
int hl_send(hostlink_t *h, uint8_t type, const uint8_t *payload, uint32_t n);   /* to the host's transport; 0 = queued */
int hl_send_to(hostlink_t *h, int transport, uint8_t type, const uint8_t *payload, uint32_t n);
void hl_tick(hostlink_t *h, uint32_t now, uint8_t hello, uint8_t heartbeat, const uint8_t *hello_p, uint32_t hello_n);
/* 1 when pkt (a USB-MIDI event packet) ended a SysEx frame: *frame, *n are its bytes between F0 and F7 */
int hl_sx_packet(hl_sx_t *s, uint32_t pkt, const uint8_t **frame, uint32_t *n);
/* a whole frame F0 .. F7 as USB-MIDI SysEx packets (CIN 4, then 5 / 6 / 7 at the end): how many went into out */
uint32_t hl_sx_packets(const uint8_t *f, uint32_t n, uint32_t *out, uint32_t max);
