/* SPDX-License-Identifier: GPL-3.0-or-later */
/* morse: Morse code without a platform. The code table, timing (PARIS, Farnsworth), a text player, an iambic keyer
 * and an adaptive decoder, all driven by a 1 ms tick or by durations, so the same code runs in fm1-base's timer
 * interrupt and in a unit test on the computer (tests/morse_test.c).
 *
 * A code is one byte: a leading 1, then the elements from the first, 0 a dit and 1 a dah. A (.-) is 0b101,
 * E (.) is 0b10, $ (...-..-) is 0b10001001. 0 is no code. */
#pragma once
#include <stdint.h>

#define MO_UNKNOWN '*'                   /* what the decoder gives for a pattern that is no character */

uint8_t mo_code(char c);                 /* the code of c (letters in either case); 0 when it has none */
char mo_char(uint8_t code);              /* the character of a code; 0 when none */
uint32_t mo_len(uint8_t code);           /* its number of elements */
int mo_elem(uint8_t code, uint32_t i);   /* element i: 0 dit, 1 dah */
uint32_t mo_units(uint8_t code);         /* its length in dit units, marks and the gaps inside it */
/* the code as text: '.' and '-' (out holds 8) */
void mo_pattern(uint8_t code, char *out);

/* timing: the dit at a character speed, and the gap unit when the spacing is slowed to an effective speed
 * (Farnsworth: the 19 gap units of PARIS stretched so the word takes 60 / eff_wpm seconds; eff >= wpm: no stretch) */
uint32_t mo_dit_ms(uint32_t wpm);
uint32_t mo_gap_ms(uint32_t wpm, uint32_t eff_wpm);

/* ------------------------------------------------------------------ player --- */
/* text to key-down / key-up, one millisecond a call. A queue: characters pushed while it plays are played after.
 * Characters between < and > are run together as one sign (a prosign: <SK> is ...-.-, <AR> is .-.-.) */
#define MO_PLAY_Q 64u
typedef struct {
    char q[MO_PLAY_Q];
    uint32_t head, tail;                 /* q[tail] is the next character to play */
    uint32_t dit, gap;                   /* ms: element, and gap unit (mo_gap_ms) */
    uint8_t pat;                         /* the elements of the character playing still to come (with the 1) */
    uint8_t on;                          /* the segment playing is a mark */
    uint32_t left;                       /* ms left of it */
    char cur;                            /* the character playing, 0 between characters */
    uint8_t started;                     /* cur just began (cleared by the reader) */
    uint8_t joined;                      /* inside < >: one dit between its characters */
} mo_player_t;

void mo_player_init(mo_player_t *p, uint32_t dit_ms, uint32_t gap_ms);
int mo_player_push(mo_player_t *p, char c);   /* 0 = queued, -1 = full; unknown characters are skipped at play */
void mo_player_stop(mo_player_t *p);
int mo_player_busy(const mo_player_t *p);
int mo_player_tick(mo_player_t *p);      /* this millisecond: 1 key down, 0 up */

/* ------------------------------------------------------------------- keyer --- */
enum { MO_STRAIGHT, MO_IAMBIC_A, MO_IAMBIC_B, MO_NKEYER };
typedef struct {
    uint8_t mode;
    uint8_t phase;                       /* 0 idle, 1 mark, 2 gap after it */
    uint8_t el;                          /* the element playing: 0 dit, 1 dah */
    uint8_t mem_dit, mem_dah;            /* B: the other paddle touched during an element */
    uint32_t left, dit;
} mo_keyer_t;

void mo_keyer_init(mo_keyer_t *k, int mode, uint32_t dit_ms);
/* this millisecond, from the paddles held (straight: either one is the key): 1 key down */
int mo_keyer_tick(mo_keyer_t *k, int dit, int dah);

/* ----------------------------------------------------------------- decoder --- */
/* marks and gaps in, characters out. It keeps running averages of the dit, the dah and the gap between characters
 * it hears and splits them halfway, so it follows a sender a good way off the speed it was started at, and a
 * Farnsworth spacing. Two marks in a character two or more times apart say which is which whatever the averages
 * thought: the averages move to them at once, and the earlier mark is read again (a beginner's slow dits taken
 * for dahs come right at the first real dah). gap_ms at the start: the spacing unit expected (mo_gap_ms; the dit
 * when not stretched). */
typedef struct {
    uint32_t dit_avg, dah_avg;           /* ms, smoothed */
    uint32_t cgap;                       /* the gap between characters it hears, ms, smoothed (Farnsworth stretches it);
                                          * a word's end is 5/3 of it */
    uint8_t pat;                         /* the elements so far, with the leading 1; 1 = none */
    uint8_t word;                        /* a character since the last space (a space is due after a long gap) */
    uint8_t last_pat;                    /* the elements of the character it gave last (a code; 0 too long) */
    uint32_t prev;                       /* the mark before, ms, in this character (0: none yet) */
    uint32_t marks, dits, dahs;          /* counted (for the speed shown, and a sending check) */
    uint32_t dit_sum, dah_sum;           /* ms of the dits and dahs counted (their ratio) */
    uint32_t egap_sum, egap_n;           /* the gaps inside characters (should be one dit) */
} mo_dec_t;

void mo_dec_init(mo_dec_t *d, uint32_t dit_ms, uint32_t gap_ms);
void mo_dec_mark(mo_dec_t *d, uint32_t ms);   /* a key-down of ms */
/* a key-up of ms, now over (the gap before the next mark): what it ended into out, the character and then ' ' for
 * a word's end; how many (0..2). What mo_dec_idle already gave for this gap is not given again */
uint32_t mo_dec_gap(mo_dec_t *d, uint32_t ms, char *out);
/* the key has been up ms so far: a character, or a space, once the silence says so (each given once) */
char mo_dec_idle(mo_dec_t *d, uint32_t ms);
uint32_t mo_dec_unit(const mo_dec_t *d);   /* its dit estimate, ms */
uint32_t mo_dec_wpm(const mo_dec_t *d);    /* the speed it hears */
uint8_t mo_dec_pending(const mo_dec_t *d); /* the pattern so far (a code, 1 = none) */
/* dah : dit ratio of what it counted, in tenths (30 = 3.0, the textbook), 0 before a dit and a dah */
uint32_t mo_dec_ratio10(const mo_dec_t *d);
/* the gaps inside characters in dits, tenths (10 = 1.0, the textbook; long ones split a character), 0 before one */
uint32_t mo_dec_gap10(const mo_dec_t *d);
int mo_dec_is_dah(const mo_dec_t *d, uint32_t ms);   /* how it would read a mark of ms now */
void mo_dec_reset_stats(mo_dec_t *d);
