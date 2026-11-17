/* SPDX-License-Identifier: MIT */
/* learn: flash cards for Morse, the way a phrasebook or a kana deck works. Each character has two cards, recognise
 * it (hear it, or see its code, and name it) and send it (see it, key it), each in a Leitner box 0..5: right moves
 * it up a box, wrong sends it back to 0, and box n comes back every 2^n rounds. The FM-1 has no clock that lives
 * through a power-off, so the schedule counts rounds, not days.
 *
 * New characters come in Koch order (LCWO's), five at a time like a kana row; the next five open once every open
 * character is in box 2 or higher to recognise and box 1 or higher to send. No platform, no libc (tests/morse_test.c).
 *
 * Two ways to learn (LN_VISUAL): by sound, the default, where nothing shows the dots and dashes (Koch, LCWO and CW
 * Academy teach each character as one sound: read off a picture, it gets counted, and copying stalls around 10-15
 * WPM), and by sight, where the code is drawn too. Muted, the sound way shows the lamp flashing in the code's own
 * rhythm instead of a drawing. */
#pragma once
#include <stdint.h>

#define LN_N 41u                         /* characters in the course */
#define LN_GROUP 5u                      /* opened at a time */
#define LN_TOP 5u                        /* the highest box */
#define LN_NEW_MAX 3u                    /* new characters in one round at most */
#define LN_CARDS_MAX 48u                 /* cards in a round, with the ones asked again */
#define LN_NEVER 0xFFFFu

enum { LN_REC, LN_SEND, LN_NDIR };
enum {
    CARD_NEW,                            /* a new character: shown and played, nothing asked */
    CARD_HEAR_CHOOSE,                    /* hear it, pick it from four */
    CARD_HEAR_KEY,                       /* hear it, key it back */
    CARD_SEE_KEY,                        /* see the letter, key its code */
    CARD_CODE_CHOOSE,                    /* see the dots and dashes drawn, pick from four (by sight only) */
    CARD_LIGHT_CHOOSE,                   /* watch the lamp flash its rhythm, pick from four (muted, by sound) */
    CARD_NTYPES
};

typedef struct {
    uint16_t round;                      /* rounds finished */
    uint16_t streak;                     /* rounds in a row at 80 % or better */
    uint8_t open;                        /* characters open, from the start of the order */
    uint8_t rsv;
    uint8_t box[LN_N][LN_NDIR];
    uint16_t last[LN_N][LN_NDIR];        /* the round it was last answered in; LN_NEVER: not yet */
    uint32_t answered, right;            /* since the start */
} ln_state_t;

typedef struct {
    uint8_t ch, dir, type;               /* ch: index in the order */
    uint8_t again;                       /* asked again after a miss (once) */
    uint8_t opt[4];                      /* the four to choose from (indexes), for the CHOOSE types */
    uint8_t right;                       /* which of them is right */
} ln_card_t;

typedef struct {
    ln_card_t card[LN_CARDS_MAX];
    uint8_t n, at;                       /* cards in the round, the one asked now */
    uint8_t ok, miss, up, fresh;         /* this round: right, wrong, moved up a box, new characters */
    uint32_t rng;
} ln_round_t;

char ln_char(uint32_t i);                /* the character at index i of the order */
int ln_index(char c);                    /* its index, -1 when not in the course */
void ln_init(ln_state_t *s);
int ln_valid(const ln_state_t *s);       /* 1 when a loaded record makes sense */
int ln_due(const ln_state_t *s, uint32_t i, int dir);   /* this card is due this round (new ones count) */
uint32_t ln_due_count(const ln_state_t *s);
uint32_t ln_new_count(const ln_state_t *s);   /* open characters not seen yet */
#define LN_MUTED 1u                      /* ln_build's flags: no card that needs sound */
#define LN_VISUAL 2u                     /* learning by sight: drawn codes allowed */
int ln_choose(int type);                 /* a card answered by picking one of four */
/* a round of n cards (n <= LN_CARDS_MAX / 2: room for the ones asked again) */
void ln_build(const ln_state_t *s, ln_round_t *r, uint32_t n, uint32_t flags, uint32_t seed);
const ln_card_t *ln_card(const ln_round_t *r);   /* the card asked now, 0 when the round is over */
/* the answer to the card asked now (ignored for CARD_NEW, which only marks the character seen): returns 1 if it
 * moved up a box */
int ln_answer(ln_state_t *s, ln_round_t *r, int right);
/* the round is over (or left): counted, the streak, and the next group opened when the open ones are learnt.
 * Returns 1 when a group opened */
int ln_finish(ln_state_t *s, const ln_round_t *r);
uint32_t ln_level(const ln_state_t *s, uint32_t i);   /* 0..5 for the chart: its two boxes' average, rounded down */
