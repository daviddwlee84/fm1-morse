/* SPDX-License-Identifier: MIT */
/* ledfx: per-LED patterns on the FM-1's single-colour LEDs (fm1-base's off / glow / mid / lit), timed in
 * software, so a key can say how urgent it is without colour.
 *
 *   ledfx_t k = {0}, b = {0};
 *   ledfx_set(&k, key, FX_BLINK, now);   ...
 *   ledfx_show(&b, &k, extra_lit_buttons);   (once a frame: plat_lights + plat_lights_mid)
 *
 * PLAY has two LEDs: its bit B_PLAY is the red one, LED_PLAY_GREEN (bit NB) the green one. */
#pragma once
#include <stdint.h>

enum {
    FX_OFF,
    FX_GLOW,                             /* dim, steady */
    FX_MID,                              /* between glow and lit, steady */
    FX_LIT,                              /* full, steady */
    FX_PULSE,                            /* glow - mid - lit - mid, 1.6 s round: alive, busy */
    FX_BLINK,                            /* on / off at 2 Hz: needs you */
    FX_DBLINK,                           /* two short flashes every 1.2 s: something broke */
    FX_FAST,                             /* on / off at 6 Hz */
    FX_N
};

typedef struct {
    uint32_t lit, mid, dim;              /* the bits at each level, built by ledfx_set */
} ledfx_t;

int ledfx_level(int fx, uint32_t now);   /* 0 off, 1 glow, 2 mid, 3 lit */
void ledfx_set(ledfx_t *o, uint32_t bit_index, int fx, uint32_t now);
void ledfx_show(const ledfx_t *buttons, const ledfx_t *keys);
