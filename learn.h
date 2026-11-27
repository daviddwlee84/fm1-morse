/* SPDX-License-Identifier: GPL-3.0-or-later */
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

/* how each card has gone (0.9): kept beside the boxes, which only say where a card is now. A card can reach the top
 * box after many misses; these say so. Counted on a card's first ask in a round (not the ask again after a miss,
 * just after hearing the answer; not a new character shown; not a placement test). The totals are for reading
 * (CHART); the recent pair, halved every LN_RECENT asks, is what ln_hard judges, so an old struggle fades */
#define LN_RECENT 16u
typedef struct {
    uint16_t asked[LN_N][LN_NDIR];       /* first asks answered (all halved together at 0xFFFF) */
    uint16_t wrong[LN_N][LN_NDIR];       /* of them wrong */
    uint16_t near[LN_N][LN_NDIR];        /* of them almost (a keyed answer close to right) */
    uint8_t lapse[LN_N][LN_NDIR];        /* wrong from box 2 or higher: learnt, then lost (stops at 255) */
    uint8_t rq[LN_N][LN_NDIR];           /* recent asks, 0..LN_RECENT */
    uint8_t rm[LN_N][LN_NDIR];           /* recent misses, wrong 2, almost 1: 0..2 * rq */
} ln_stats_t;

typedef struct {
    ln_card_t card[LN_CARDS_MAX];
    uint8_t n, at;                       /* cards in the round, the one asked now */
    uint8_t ok, miss, up, fresh;         /* this round: right, wrong, moved up a box, new characters */
    uint8_t near;                        /* answers almost right (no box moved, asked again) */
    uint8_t place;                       /* a placement test (LN_PLACE): answers change nothing until applied */
    uint32_t known[2];                   /* placement: the characters answered right (bit i of index i) */
    uint32_t rng;
} ln_round_t;

char ln_char(uint32_t i);                /* the character at index i of the order */
int ln_index(char c);                    /* its index, -1 when not in the course */
void ln_init(ln_state_t *s);
int ln_valid(const ln_state_t *s);       /* 1 when a loaded record makes sense */
/* t (0: none) makes a hard card come back as if ln_hard(t, i, dir) boxes lower; its box stays */
int ln_due(const ln_state_t *s, const ln_stats_t *t, uint32_t i, int dir);   /* due this round (new ones count) */
uint32_t ln_due_count(const ln_state_t *s, const ln_stats_t *t);
uint32_t ln_new_count(const ln_state_t *s);   /* open characters not seen yet */
#define LN_MUTED 1u                      /* ln_build's flags: no card that needs sound */
#define LN_VISUAL 2u                     /* learning by sight: drawn codes allowed */
#define LN_PLACE 4u                      /* a placement test: each character of the next two groups once, to pick */
#define LN_PLACE_N (2u * LN_GROUP)
int ln_choose(int type);                 /* a card answered by picking one of four */
/* a round of n cards (n <= LN_CARDS_MAX / 2: room for the ones asked again); t as ln_due's, also in the fill's order */
void ln_build(const ln_state_t *s, const ln_stats_t *t, ln_round_t *r, uint32_t n, uint32_t flags, uint32_t seed);
const ln_card_t *ln_card(const ln_round_t *r);   /* the card asked now, 0 when the round is over */
/* the answer to the card asked now (ignored for CARD_NEW, which only marks the character seen): returns 1 if it
 * moved up a box. LN_ALMOST: no box moves, the card is asked again at the end */
enum { LN_WRONG, LN_RIGHT, LN_ALMOST };
int ln_answer(ln_state_t *s, ln_round_t *r, int result);
/* the same answer into the stats: call it just before ln_answer (it reads the box the card is in) */
void ln_tally(ln_stats_t *t, const ln_state_t *s, const ln_round_t *r, int result);
/* the round is over (or left): counted, the streak, and the next group opened when the open ones are learnt.
 * Returns 1 when a group opened */
int ln_finish(ln_state_t *s, const ln_round_t *r);
uint32_t ln_level(const ln_state_t *s, uint32_t i);   /* 0..5 for the chart: its two boxes' average, rounded down */

/* changes by hand (the chart's edit, a placement test, a loaded backup); each leaves ln_valid true */
void ln_set_level(ln_state_t *s, uint32_t i, uint32_t lv);   /* both boxes to lv; opens up to its group if closed */
void ln_mark_known(ln_state_t *s, uint32_t i);   /* recognise box 3, send box 2 */
void ln_open_to(ln_state_t *s, uint32_t n);      /* n characters open (rounded up to a group); the newly opened set
                                                  * just at the next group's line, the closed ones back to new */
void ln_step_back(ln_state_t *s);                /* every box one down: all come back sooner */
uint32_t ln_place_count(const ln_round_t *r);    /* placement: characters answered right */
/* placement over: those answered right marked known, and each group answered all right in turn opened. Returns how
 * many were marked */
uint32_t ln_place_apply(ln_state_t *s, const ln_round_t *r);
/* the record as bytes for a backup (LN_BLOB_N, little-endian, no padding) and back; ln_unblob returns 1 when it
 * makes sense */
#define LN_BLOB_N (2u + 2u + 1u + 4u + 4u + LN_N * LN_NDIR + LN_N * LN_NDIR * 2u)
void ln_blob(const ln_state_t *s, uint8_t *b);
int ln_unblob(ln_state_t *s, const uint8_t *b);

/* the stats (ln_stats_t) */
void ln_stats_init(ln_stats_t *t);
int ln_stats_valid(const ln_stats_t *t);  /* 1 when a loaded copy makes sense */
#define LN_HARD_MIN 4u                   /* recent asks before ln_hard judges */
/* 0 fine, 1 hard (recent misses 20 % or more, an almost half a miss), 2 very hard (40 % or more) */
uint32_t ln_hard(const ln_stats_t *t, uint32_t i, int dir);
/* the misses of all its first asks in percent (an almost half a miss), rounded; -1 never asked */
int32_t ln_miss_pct(const ln_stats_t *t, uint32_t i, int dir);
