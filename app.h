/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Morse: the FM-1 as a pocket Morse code machine. Key on the piano keys (a straight key, or two halves as iambic
 * paddles) and read what you sent; type letters on the keys and hear them; learn the code with flash cards a few
 * minutes at a time (Leitner boxes, Koch order); keep it silent and read the lights instead. Keying goes out and comes
 * in as USB-MIDI note 0 on channel 1, the Vail adapter's format, so vailmorse.com and its training tools take it.
 * A host program (host/morse) can type what you send into the computer and send text to play.
 * Built on fm1-base (MPL-2.0, its headers). This app is GPL-3.0-or-later; common/ (hostlink, ledfx) is MIT. */
#pragma once
#include <stdint.h>
#include "plat.h"

#define APP_NAME "MORSE"
#define APP_SPLASH_RGB RGB(90, 200, 255)
#define APP_BLOCK 256u                   /* frames per audio block: 5.8 ms at 44.1 kHz (the sidetone's jitter) */
#define APP_TICK                         /* 1 kHz: the key, the keyer, the players, MIDI in and out */
#define APP_SYSEX                        /* our frames from the host (PROTOCOL.md in github.com/daviddwlee84/M-VAVE_FM-1 host/morse) */

/* the settings and the flash cards' progress, one record (app.c rec_t), at 0xDA000: away from the 0xFC000 settings
 * sector SLOOP, Agent Deck and CTL-1 share (docs/hardware/flash-map.md). The panel calibration is the platform's */
enum { OBJ_PANEL, OBJ_MORSE, OBJ_NOBJ };
#define APP_STORE_MAP {0xDC000u, 0xDA000u}

void app_init(void);
void app_render(int32_t *lr, uint32_t n, uint32_t master_q12);
void app_panic(void);
void app_ui_init(void);
void app_frame(void);
void app_input_only(void);
void app_say(const char *a, const char *b);
void app_tick(void);
int app_sysex(const uint8_t *p, uint32_t n);

/* host simulator: checks (page, mute, wpm, ps, card, ...), commands (expect_text, sendkeys, answer, clear_text) and
 * the failures of those commands */
#define APP_HOST_EXPECT
int app_expect(const char *what, int *got);
#define APP_HOST_CMD
int app_host_cmd(const char *cmd, const char *a, const char *b, const char *c, const char *d);
#define APP_HOST_FAILS
int app_host_fails(void);
