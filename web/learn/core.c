/* SPDX-License-Identifier: GPL-3.0-or-later */
/* core: MORSE's platform-free parts (learn.c, morse.c, content.c, unchanged) built for the browser as WebAssembly,
 * with the record the FM-1 keeps at 0xDA000 (app.c rec_t) byte for byte. wasm32 is little-endian and aligns like
 * the FM-1's ARM core, so the struct below is the same 1020 bytes; the asserts say so. The page keeps the record's
 * bytes (rec_ptr, REC_SIZE) and calls these; nothing here knows about the screen or the clock.
 *   clang --target=wasm32 -O2 -nostdlib -Wl,--no-entry -Wl,--export-dynamic -I../.. core.c -o core.wasm
 * (build.sh). Keep set_t, rec_t and set_valid in step with app.c. */
#include <stddef.h>
#include <stdint.h>
#include "learn.h"
#include "morse.h"
#include "content.h"

#define EXPORT __attribute__((visibility("default")))

/* the compiler's own calls (struct copies) */
void *memcpy(void *d, const void *s, size_t n)
{
    uint8_t *a = (uint8_t *)d;
    const uint8_t *b = (const uint8_t *)s;
    while (n--)
        *a++ = *b++;
    return d;
}
void *memset(void *d, int c, size_t n)
{
    uint8_t *a = (uint8_t *)d;
    while (n--)
        *a++ = (uint8_t)c;
    return d;
}

/* ------------------------------------------------------------ the record --- */
#define REC_MAGIC 0x4D4F5231u            /* "MOR1" */
#define REC_VER 2u
enum { LM_SOUND, LM_VISUAL };
#define KV_N 3u
#define TL_N 3u
#define START_N 7u
typedef struct {                         /* app.c set_t */
    uint8_t wpm, eff, keyer, swap, flags, cards, learn, look;
    uint16_t pitch;
    uint8_t send, prog;
} set_t;
typedef struct {                         /* app.c rec_t */
    uint32_t magic;
    uint16_t ver, size;
    set_t set;
    ln_state_t ln;
    ln_stats_t st;
} rec_t;
#define REC1_SIZE ((uint32_t)offsetof(rec_t, st))
typedef char rec1_is_0_8s[REC1_SIZE == 280u ? 1 : -1];
typedef char rec2_is_0_9s[sizeof(rec_t) == 1020u ? 1 : -1];
typedef char set_at_8[offsetof(rec_t, set) == 8u && sizeof(set_t) == 12u ? 1 : -1];
typedef char ln_at_20[offsetof(rec_t, ln) == 20u ? 1 : -1];

static rec_t rec;
#define SET (rec.set)

static void set_defaults(void)
{
    SET.wpm = 20;
    SET.eff = 8;
    SET.keyer = MO_STRAIGHT;
    SET.swap = 0;
    SET.flags = 0;
    SET.cards = 20;
    SET.learn = LM_SOUND;
    SET.look = 0;
    SET.pitch = 600;
    SET.send = 10;
    SET.prog = LP_KOCH;
}

static int flags_valid(uint32_t f) { return f < 0x20u && (f >> 2 & 7u) < START_N; }
static int set_valid(const set_t *s)
{
    return s->wpm >= 5u && s->wpm <= 40u && s->eff >= 5u && s->eff <= s->wpm && s->keyer < MO_NKEYER && s->swap <= 1u
           && flags_valid(s->flags) && s->cards >= 10u && s->cards <= 30u && s->learn <= LM_VISUAL && s->pitch >= 300u
           && s->pitch <= 1200u && (s->look & 3u) < KV_N && (s->look >> 3 & 3u) < TL_N
           && s->send >= 5u && s->send <= 40u && s->prog < LP_N;
}

EXPORT uint8_t *rec_ptr(void) { return (uint8_t *)&rec; }
EXPORT uint32_t rec_size(void) { return sizeof rec; }
EXPORT uint32_t rec1_size(void) { return REC1_SIZE; }
EXPORT uint8_t *set_ptr(void) { return (uint8_t *)&rec.set; }
EXPORT uint8_t *ln_ptr(void) { return (uint8_t *)&rec.ln; }
EXPORT uint8_t *st_ptr(void) { return (uint8_t *)&rec.st; }

EXPORT void rec_seal(void)               /* app.c rec_save's header */
{
    rec.magic = REC_MAGIC;
    rec.ver = REC_VER;
    rec.size = (uint16_t)sizeof rec;
}

EXPORT void rec_new(void)                /* what MORSE starts with on a clean store */
{
    set_defaults();
    ln_init(&rec.ln);
    ln_stats_init(&rec.st);
    rec_seal();
}

/* n bytes of a record have been put at rec_ptr(): 1 when MORSE would take it (app.c rec_load, plus the settings'
 * check app_init makes). A version 1 record (0.1-0.8, 280 bytes) comes up with the stats empty, as 0.9 does it.
 * Anything else: 0, and the record is the defaults */
EXPORT int rec_check(uint32_t n)
{
    int ok = 0;
    if (n >= REC1_SIZE && rec.magic == REC_MAGIC) {
        if (rec.ver == 1u && rec.size == REC1_SIZE && n == REC1_SIZE) {
            ln_stats_init(&rec.st);
            ok = 1;
        } else
            ok = rec.ver == REC_VER && rec.size == sizeof rec && n == sizeof rec && ln_stats_valid(&rec.st);
    }
    ok = ok && ln_valid(&rec.ln) && set_valid(&rec.set);
    if (!ok)
        rec_new();
    rec_seal();
    return ok;
}
EXPORT int set_ok(void) { return set_valid(&rec.set); }
EXPORT int stats_ok(void) { return ln_stats_valid(&rec.st); }

/* a backup's progress (learn.h's blob, what the SysEx frames and the JSON carry) */
static uint8_t blob[LN_BLOB_N];
EXPORT uint8_t *blob_ptr(void) { return blob; }
EXPORT uint32_t blob_size(void) { return LN_BLOB_N; }
EXPORT void blob_out(void) { ln_blob(&rec.ln, blob); }
EXPORT int blob_in(void) { return ln_unblob(&rec.ln, blob); }   /* 0: not taken, the progress unchanged */
EXPORT int blob_check(void) { ln_state_t t; return ln_unblob(&t, blob); }

/* ------------------------------------------------------------- a round --- */
static ln_round_t round_;
EXPORT uint8_t *round_ptr(void) { return (uint8_t *)&round_; }
EXPORT uint32_t card_size(void) { return sizeof(ln_card_t); }
EXPORT uint32_t round_n_off(void) { return offsetof(ln_round_t, n); }
EXPORT void round_build(uint32_t flags, uint32_t seed)
{
    ln_build(&rec.ln, &rec.st, &round_, SET.cards, flags, seed);
}
EXPORT const ln_card_t *round_card(void) { return ln_card(&round_); }
EXPORT int round_answer(int res)         /* app.c practice_answer: the stats, then the box */
{
    const ln_card_t *c = ln_card(&round_);
    if (!c)
        return 0;
    if (c->type == CARD_NEW)
        return ln_answer(&rec.ln, &round_, LN_RIGHT);
    ln_tally(&rec.st, &rec.ln, &round_, res);
    return ln_answer(&rec.ln, &round_, res);
}
EXPORT int round_finish(void) { return ln_finish(&rec.ln, &round_); }
EXPORT uint32_t round_place_count(void) { return ln_place_count(&round_); }
EXPORT uint32_t round_place_apply(void) { return ln_place_apply(&rec.ln, &round_); }
EXPORT int round_known(uint32_t i) { return i < LN_N && (round_.known[i / 32u] >> (i % 32u) & 1u); }

EXPORT uint32_t due_count(void) { return ln_due_count(&rec.ln, &rec.st); }
EXPORT uint32_t new_count(void) { return ln_new_count(&rec.ln); }
EXPORT uint32_t level(uint32_t i) { return ln_level(&rec.ln, i); }
EXPORT uint32_t hard(uint32_t i, int dir) { return ln_hard(&rec.st, i, dir); }
EXPORT int32_t miss_pct(uint32_t i, int dir) { return ln_miss_pct(&rec.st, i, dir); }
EXPORT int choose_type(int type) { return ln_choose(type); }
EXPORT uint32_t ch_at(uint32_t i) { return (uint32_t)ln_char(i); }
EXPORT int ch_index(uint32_t c) { return ln_index((char)c); }
EXPORT void set_level(uint32_t i, uint32_t lv) { ln_set_level(&rec.ln, i, lv); }
EXPORT void open_to(uint32_t n) { ln_open_to(&rec.ln, n); }
EXPORT void step_back(void) { ln_step_back(&rec.ln); }
EXPORT void progress_reset(void) { ln_init(&rec.ln); ln_stats_init(&rec.st); }

/* ------------------------------------------------------------ the codes --- */
EXPORT uint32_t code(uint32_t c) { return mo_code((char)c); }
EXPORT uint32_t code_len(uint32_t k) { return mo_len((uint8_t)k); }
EXPORT int code_elem(uint32_t k, uint32_t i) { return mo_elem((uint8_t)k, i); }
EXPORT uint32_t dit_ms(uint32_t wpm) { return mo_dit_ms(wpm); }
EXPORT uint32_t gap_ms(uint32_t wpm, uint32_t eff) { return mo_gap_ms(wpm, eff); }

/* text to a timeline: mo_player run a millisecond at a time, as the FM-1's tick does. Out: alternating durations,
 * the first a mark (a leading silence is 0 ms of mark), into tl[]; returns how many */
#define TL_MAX 4096u
static uint32_t tl[TL_MAX];
static char text[CT_LINE_MAX + 2u];
EXPORT char *text_ptr(void) { return text; }
EXPORT uint32_t *tl_ptr(void) { return tl; }
EXPORT uint32_t timeline(uint32_t dit, uint32_t gap)
{
    static mo_player_t p;
    uint32_t n = 0, i, run = 0, guard = 0;
    int on = 1;
    mo_player_init(&p, dit, gap);
    for (i = 0; text[i]; i++)
        if (mo_player_push(&p, text[i]) < 0)
            break;
    while (n + 1u < TL_MAX && guard++ < 600000u) {
        int k;
        if (!p.left && !mo_player_busy(&p))
            break;
        k = mo_player_tick(&p);
        if (k != on) {
            tl[n++] = run;
            run = 0;
            on = k;
        }
        run++;
    }
    if (run && n < TL_MAX)
        tl[n++] = run;
    return n;
}

/* --------------------------------------------------- a keyed answer, judged --- */
static uint16_t am[8], ag[8];            /* marks and the gaps before them, ms (app.c pr.am, pr.ag) */
static mo_judge_t jd;
EXPORT uint16_t *marks_ptr(void) { return am; }
EXPORT uint16_t *gaps_ptr(void) { return ag; }
EXPORT uint32_t judge(uint32_t n, uint32_t want, uint32_t dit_est)
{
    jd = mo_judge(am, ag, n > 8u ? 8u : n, (uint8_t)want, dit_est);
    return jd.verdict | (uint32_t)jd.hint << 8 | (uint32_t)jd.score << 16;
}
EXPORT uint32_t judge_dit(void) { return jd.dit; }

/* the own decoder (app.c dsrc[SRC_OWN].dec) */
static mo_dec_t dec;
static char dout[4];
EXPORT void dec_init(uint32_t dit, uint32_t gap) { mo_dec_init(&dec, dit, gap); }
EXPORT void dec_restart(void) { mo_dec_restart(&dec); }
EXPORT void dec_mark(uint32_t ms) { mo_dec_mark(&dec, ms); }
EXPORT uint32_t dec_gap(uint32_t ms)     /* characters out: up to 2, packed low byte first */
{
    uint32_t n;
    dout[0] = dout[1] = 0;
    n = mo_dec_gap(&dec, ms, dout);
    return n ? (uint8_t)dout[0] | (uint32_t)(n > 1u ? (uint8_t)dout[1] : 0u) << 8 : 0u;
}
EXPORT uint32_t dec_idle(uint32_t ms) { return (uint8_t)mo_dec_idle(&dec, ms); }
EXPORT uint32_t dec_unit(void) { return mo_dec_unit(&dec); }
EXPORT uint32_t dec_wpm(void) { return mo_dec_wpm(&dec); }
EXPORT uint32_t dec_ratio10(void) { return mo_dec_ratio10(&dec); }
EXPORT uint32_t dec_gap10(void) { return mo_dec_gap10(&dec); }
EXPORT void dec_teach(uint32_t n, uint32_t want) { mo_dec_teach(&dec, am, n > 8u ? 8u : n, (uint8_t)want); }
EXPORT void dec_reset_stats(void) { mo_dec_reset_stats(&dec); }

/* the iambic keyer (app.c), one millisecond a call */
static mo_keyer_t key;
EXPORT void keyer_init(int mode, uint32_t dit) { mo_keyer_init(&key, mode, dit); }
EXPORT int keyer_tick(int dit, int dah) { return mo_keyer_tick(&key, dit, dah); }

/* ------------------------------------------------------------ LISTEN's text --- */
static ct_gen_t gen;
static const char *gloss;
EXPORT void listen_init(uint32_t seed) { ct_init(&gen, seed); }
EXPORT char *listen_line(int prog)       /* into text_ptr(); listen_gloss() for an abbreviation */
{
    ct_line(&gen, prog, rec.ln.open, text, &gloss);
    return text;
}
EXPORT const char *listen_gloss(void) { return gloss ? gloss : ""; }
