/* SPDX-License-Identifier: MIT */
/* ledfx: see ledfx.h */
#include "ledfx.h"

int ledfx_level(int fx, uint32_t now)
{
    switch (fx) {
    case FX_GLOW:
        return 1;
    case FX_MID:
        return 2;
    case FX_LIT:
        return 3;
    case FX_PULSE: {
        static const uint8_t STEP[4] = {1, 2, 3, 2};
        return STEP[(now / 400u) % 4u];
    }
    case FX_BLINK:
        return now % 500u < 250u ? 3 : 0;
    case FX_DBLINK: {
        uint32_t t = now % 1200u;
        return (t < 120u || (t >= 240u && t < 360u)) ? 3 : 0;
    }
    case FX_FAST:
        return now % 166u < 83u ? 3 : 0;
    default:
        return 0;
    }
}

void ledfx_set(ledfx_t *o, uint32_t i, int fx, uint32_t now)
{
    uint32_t bit = 1u << i;
    o->lit &= ~bit;
    o->mid &= ~bit;
    o->dim &= ~bit;
    switch (ledfx_level(fx, now)) {
    case 1:
        o->dim |= bit;
        break;
    case 2:
        o->mid |= bit;
        break;
    case 3:
        o->lit |= bit;
        break;
    default:
        break;
    }
}

void ledfx_show(const ledfx_t *b, const ledfx_t *k)
{
    plat_lights(b->lit, k->lit, b->dim, k->dim, 2);
    plat_lights_mid(b->mid, k->mid);
}
