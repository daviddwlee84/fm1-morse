/* SPDX-License-Identifier: MIT */
/* content: what the LISTEN page plays and the TYPE page's phrases. Lines made up on the spot from small lists:
 * groups of the letters you have open (Koch), common English words, a made-up QSO between two random stations,
 * the usual CW abbreviations with what they mean, and proverbs (old sayings, nobody's text). No platform, no libc. */
#pragma once
#include <stdint.h>

#define CT_LINE_MAX 120u

enum { LP_KOCH, LP_WORDS, LP_QSO, LP_ABBR, LP_TEXT, LP_N };
extern const char *const LP_NAME[LP_N];

typedef struct {
    uint32_t rng;
    uint8_t step;                        /* QSO: the next of its six lines */
    char a[8], b[8];                     /* QSO: the two calls */
    uint8_t na, nb, qa, qb, ra, rb;      /* QSO: their names, places, reports (indexes) */
    uint8_t last;                        /* the list entry given last (not twice in a row) */
} ct_gen_t;

void ct_init(ct_gen_t *g, uint32_t seed);
/* the next line of program p (LP_*) into out (CT_LINE_MAX + 1); open: the Koch letters open (KOCH); *gloss: what it
 * means, for an abbreviation (ABBR), else 0 */
void ct_line(ct_gen_t *g, int p, uint32_t open, char *out, const char **gloss);
const char *ct_gloss(const char *abbr);  /* what an abbreviation means, 0 when it is not in the list */

/* the TYPE page's phrase keys: one each, white keys left to right, then black */
#define CT_PHRASE_WHITE_N 16u
#define CT_PHRASE_BLACK_N 11u
extern const char *const CT_PHRASE_WHITE[CT_PHRASE_WHITE_N];
extern const char *const CT_PHRASE_BLACK[CT_PHRASE_BLACK_N];
