/* SPDX-License-Identifier: MIT */
/* tone: the sidetone, two sine voices keyed on and off without a click (a 5 ms raised ramp each way), one for what
 * this FM-1 sends and one for what it receives, at different pitches.
 *
 *   tone_pitch(TONE_TX, 600.0f);         main loop
 *   tone_gate(TONE_TX, 1);               the 1 kHz tick (or anywhere): key down
 *   tone_render(lr, n, gain);            audio interrupt: adds the voices into lr (24-bit stereo)
 *
 * The gate is read once per audio block (APP_BLOCK frames, 5.8 ms at 256): that is the keying's jitter. Muted, the
 * voices run silent (the screen and the lights show the keying instead). Needs fastmath.h (fm_sinf). */
#pragma once
#include <stdint.h>

enum { TONE_TX, TONE_RX, TONE_N };

void tone_pitch(int v, float hz);
void tone_gate(int v, int on);
void tone_mute(int on);
int tone_gated(int v);                   /* the gate as last set */
void tone_render(int32_t *lr, uint32_t n, float gain);
void tone_stop(void);                    /* both silent now (app_panic) */
