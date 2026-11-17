/* SPDX-License-Identifier: MIT */
/* tone: see tone.h */
#include "tone.h"

#define TONE_RATE 44100.0f
#define TONE_RAMP (1.0f / (0.005f * TONE_RATE))   /* 5 ms from silent to full */
#define TONE_AMP 0.3f

static struct {
    volatile uint8_t gate;
    volatile float inc;                  /* phase step per frame, of a turn */
    float phase, ramp;                   /* ramp 0..1, shaped into the level */
} tone_v[TONE_N];
static volatile uint8_t tone_muted;

void tone_pitch(int v, float hz)
{
    if (v >= 0 && v < TONE_N)
        tone_v[v].inc = hz / TONE_RATE;
}

void tone_gate(int v, int on)
{
    if (v >= 0 && v < TONE_N)
        tone_v[v].gate = (uint8_t)(on != 0);
}

void tone_mute(int on) { tone_muted = (uint8_t)(on != 0); }

int tone_gated(int v) { return v >= 0 && v < TONE_N && tone_v[v].gate; }

void tone_stop(void)
{
    uint32_t v;
    for (v = 0; v < TONE_N; v++) {
        tone_v[v].gate = 0;
        tone_v[v].ramp = 0.0f;
    }
}

void tone_render(int32_t *lr, uint32_t n, float gain)
{
    uint32_t v, i;
    for (v = 0; v < TONE_N; v++) {
        float inc = tone_v[v].inc, phase = tone_v[v].phase, ramp = tone_v[v].ramp;
        int on = tone_v[v].gate;
        if (!on && ramp <= 0.0f)
            continue;
        for (i = 0; i < n; i++) {
            float lvl, s;
            ramp += on ? TONE_RAMP : -TONE_RAMP;
            ramp = ramp < 0.0f ? 0.0f : ramp > 1.0f ? 1.0f : ramp;
            phase += inc;
            if (phase >= 1.0f)
                phase -= 1.0f;
            if (tone_muted)
                continue;
            lvl = ramp * ramp * (3.0f - 2.0f * ramp);   /* smoothstep: the ramp's corners rounded */
            s = fm_sinf(phase * 6.2831853f) * lvl * TONE_AMP * gain;
            lr[2 * i] += (int32_t)s;
            lr[2 * i + 1] += (int32_t)s;
        }
        tone_v[v].phase = phase;
        tone_v[v].ramp = ramp;
    }
}
