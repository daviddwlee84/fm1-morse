/* SPDX-License-Identifier: GPL-3.0-or-later */
/* morse: see morse.h. No platform, no libc: part of the app's unity build and of tests/morse_test.c. */
#include "morse.h"

/* ITU-R M.1677-1, plus ! (-.-.--, not ITU but common). Prosigns run together are characters here: AR is +,
 * BT is =, KN is (. */
static const struct {
    char c;
    const char *p;
} MO_TABLE[] = {
    {'A', ".-"},     {'B', "-..."},   {'C', "-.-."},   {'D', "-.."},    {'E', "."},      {'F', "..-."},
    {'G', "--."},    {'H', "...."},   {'I', ".."},     {'J', ".---"},   {'K', "-.-"},    {'L', ".-.."},
    {'M', "--"},     {'N', "-."},     {'O', "---"},    {'P', ".--."},   {'Q', "--.-"},   {'R', ".-."},
    {'S', "..."},    {'T', "-"},      {'U', "..-"},    {'V', "...-"},   {'W', ".--"},    {'X', "-..-"},
    {'Y', "-.--"},   {'Z', "--.."},   {'0', "-----"},  {'1', ".----"},  {'2', "..---"},  {'3', "...--"},
    {'4', "....-"},  {'5', "....."},  {'6', "-...."},  {'7', "--..."},  {'8', "---.."},  {'9', "----."},
    {'.', ".-.-.-"}, {',', "--..--"}, {'?', "..--.."}, {'\'', ".----."}, {'!', "-.-.--"}, {'/', "-..-."},
    {'(', "-.--."},  {')', "-.--.-"}, {'&', ".-..."},  {':', "---..."}, {';', "-.-.-."}, {'=', "-...-"},
    {'+', ".-.-."},  {'-', "-....-"}, {'_', "..--.-"}, {'"', ".-..-."}, {'$', "...-..-"}, {'@', ".--.-."},
};
#define MO_NTABLE (sizeof MO_TABLE / sizeof MO_TABLE[0])

static uint8_t mo_from_pattern(const char *p)
{
    uint8_t c = 1;
    while (*p)
        c = (uint8_t)(c << 1 | (*p++ == '-'));
    return c;
}

static char mo_upper(char c) { return c >= 'a' && c <= 'z' ? (char)(c - 32) : c; }

uint8_t mo_code(char c)
{
    uint32_t i;
    c = mo_upper(c);
    for (i = 0; i < MO_NTABLE; i++)
        if (MO_TABLE[i].c == c)
            return mo_from_pattern(MO_TABLE[i].p);
    return 0;
}

char mo_char(uint8_t code)
{
    uint32_t i;
    if (code < 2u)
        return 0;
    for (i = 0; i < MO_NTABLE; i++)
        if (mo_from_pattern(MO_TABLE[i].p) == code)
            return MO_TABLE[i].c;
    return 0;
}

uint32_t mo_len(uint8_t code)
{
    uint32_t n = 0;
    while (code > 1u) {
        code >>= 1;
        n++;
    }
    return n;
}

int mo_elem(uint8_t code, uint32_t i)
{
    uint32_t n = mo_len(code);
    return i < n ? (int)(code >> (n - 1u - i) & 1u) : 0;
}

uint32_t mo_units(uint8_t code)
{
    uint32_t n = mo_len(code), i, u = 0;
    for (i = 0; i < n; i++)
        u += mo_elem(code, i) ? 3u : 1u;
    return n ? u + n - 1u : 0;
}

void mo_pattern(uint8_t code, char *out)
{
    uint32_t n = mo_len(code), i;
    for (i = 0; i < n && i < 7u; i++)
        out[i] = mo_elem(code, i) ? '-' : '.';
    out[i] = 0;
}

uint32_t mo_dit_ms(uint32_t wpm) { return 1200u / (wpm ? wpm : 1u); }

uint32_t mo_gap_ms(uint32_t wpm, uint32_t eff_wpm)
{
    uint32_t dit = mo_dit_ms(wpm), word;
    if (!eff_wpm || eff_wpm >= wpm)
        return dit;
    word = 60000u / eff_wpm;             /* PARIS at the effective speed: 31 dits of marks and inner gaps, 19 gap units */
    return word > 31u * dit ? (word - 31u * dit) / 19u : dit;
}

/* ------------------------------------------------------------------ player --- */
void mo_player_init(mo_player_t *p, uint32_t dit_ms, uint32_t gap_ms)
{
    p->head = p->tail = 0;
    p->dit = dit_ms;
    p->gap = gap_ms;
    p->pat = 0;
    p->on = 0;
    p->left = 0;
    p->cur = 0;
    p->started = 0;
    p->joined = 0;
}

int mo_player_push(mo_player_t *p, char c)
{
    uint32_t next = (p->head + 1u) % MO_PLAY_Q;
    if (next == p->tail)
        return -1;
    p->q[p->head] = c;
    p->head = next;
    return 0;
}

void mo_player_stop(mo_player_t *p)
{
    p->tail = p->head;
    p->pat = 0;
    p->on = 0;
    p->left = 0;
    p->cur = 0;
    p->joined = 0;
}

int mo_player_busy(const mo_player_t *p) { return p->left || p->on || p->pat > 1u || p->head != p->tail; }

static void mo_player_element(mo_player_t *p)
{
    uint32_t n = mo_len(p->pat);
    int el = (int)(p->pat >> (n - 1u) & 1u);
    p->pat = (uint8_t)((p->pat & ((1u << (n - 1u)) - 1u)) | 1u << (n - 1u));   /* the rest, its 1 moved down */
    p->on = 1;
    p->left = el ? 3u * p->dit : p->dit;
}

static int mo_player_next(mo_player_t *p)
{
    if (p->on) {                         /* a mark ended: the gap inside the character, or after it */
        p->on = 0;
        if (p->pat > 1u)
            p->left = p->dit;
        else {
            p->cur = 0;
            p->left = p->joined ? p->dit : 3u * p->gap;   /* in a prosign: run on */
        }
        return 1;
    }
    if (p->pat > 1u) {
        mo_player_element(p);
        return 1;
    }
    while (p->tail != p->head) {
        char c = p->q[p->tail];
        uint8_t code;
        p->tail = (p->tail + 1u) % MO_PLAY_Q;
        if (c == ' ') {                  /* with the 3 after the character before: a word's 7 */
            p->joined = 0;
            p->left = 4u * p->gap;
            return 1;
        }
        if (c == '<') {
            p->joined = 1;
            continue;
        }
        if (c == '>') {                  /* the prosign's end: the rest of a character gap after its last part */
            p->joined = 0;
            if (3u * p->gap > p->dit) {
                p->left = 3u * p->gap - p->dit;
                return 1;
            }
            continue;
        }
        if (!(code = mo_code(c)))
            continue;
        p->pat = code;
        p->cur = mo_upper(c);
        p->started = 1;
        mo_player_element(p);
        return 1;
    }
    p->pat = 0;
    return 0;
}

int mo_player_tick(mo_player_t *p)
{
    while (!p->left)
        if (!mo_player_next(p))
            return 0;
    p->left--;
    return p->on;
}

/* ------------------------------------------------------------------- keyer --- */
void mo_keyer_init(mo_keyer_t *k, int mode, uint32_t dit_ms)
{
    k->mode = (uint8_t)(mode >= 0 && mode < MO_NKEYER ? mode : MO_STRAIGHT);
    k->phase = 0;
    k->el = 0;
    k->mem_dit = k->mem_dah = 0;
    k->left = 0;
    k->dit = dit_ms;
}

static void mo_keyer_start(mo_keyer_t *k, int el)
{
    k->el = (uint8_t)el;
    k->phase = 1;
    k->left = el ? 3u * k->dit : k->dit;
    if (el)
        k->mem_dah = 0;
    else
        k->mem_dit = 0;
}

int mo_keyer_tick(mo_keyer_t *k, int dit, int dah)
{
    if (k->mode == MO_STRAIGHT)
        return dit || dah;
    if (k->phase && k->mode == MO_IAMBIC_B) {   /* B remembers the other paddle touched while an element runs */
        if (dit && k->el == 1u)
            k->mem_dit = 1;
        if (dah && k->el == 0u)
            k->mem_dah = 1;
    }
    if (k->phase && !k->left) {
        if (k->phase == 1u) {
            k->phase = 2;
            k->left = k->dit;
        } else
            k->phase = 3;                /* the gap is over: what comes next is chosen below */
    }
    if (!k->phase || k->phase == 3u) {
        int after = k->phase == 3u, next = -1;
        if (dit && dah)
            next = after ? !k->el : 0;   /* squeezed: they alternate */
        else if (after && k->mem_dit && k->el == 1u)
            next = 0;
        else if (after && k->mem_dah && k->el == 0u)
            next = 1;
        else if (dit)
            next = 0;
        else if (dah)
            next = 1;
        if (next >= 0)
            mo_keyer_start(k, next);
        else {
            k->phase = 0;
            k->mem_dit = k->mem_dah = 0;
        }
    }
    if (!k->phase)
        return 0;
    k->left--;
    return k->phase == 1u;
}

/* ----------------------------------------------------------------- decoder --- */
void mo_dec_init(mo_dec_t *d, uint32_t dit_ms, uint32_t gap_ms)
{
    d->dit_avg = dit_ms;
    d->dah_avg = 3u * dit_ms;
    d->cgap = 3u * (gap_ms > dit_ms ? gap_ms : dit_ms);
    d->pat = 1;
    d->word = 0;
    d->last_pat = 0;
    d->prev = 0;
    mo_dec_reset_stats(d);
}

void mo_dec_reset_stats(mo_dec_t *d)
{
    d->marks = d->dits = d->dahs = 0;
    d->dit_sum = d->dah_sum = 0;
    d->egap_sum = d->egap_n = 0;
}

int mo_dec_is_dah(const mo_dec_t *d, uint32_t ms) { return ms >= (d->dit_avg + d->dah_avg) / 2u; }

uint32_t mo_dec_unit(const mo_dec_t *d) { return (d->dit_avg + d->dah_avg / 3u) / 2u; }

void mo_dec_mark(mo_dec_t *d, uint32_t ms)
{
    int dah;
    if (ms < 4u)                         /* a bounce, not a mark */
        return;
    if (d->prev && d->pat > 1u && d->pat < 0x80u) {   /* two marks of this character, far apart: a dit and a dah */
        uint32_t lo = ms < d->prev ? ms : d->prev, hi = ms < d->prev ? d->prev : ms;
        if (hi >= 2u * lo && hi <= 8u * lo) {
            int prev_dah = (int)(d->pat & 1u), prev_should = d->prev == hi;
            d->dit_avg = (d->dit_avg + lo) / 2u;
            d->dah_avg = (d->dah_avg + hi) / 2u;
            if (prev_dah != prev_should) {   /* the mark before was read wrong: read it again */
                d->pat ^= 1u;
                if (prev_should) {
                    d->dits--, d->dit_sum -= d->prev;
                    d->dahs++, d->dah_sum += d->prev;
                } else {
                    d->dahs--, d->dah_sum -= d->prev;
                    d->dits++, d->dit_sum += d->prev;
                }
            }
        }
    }
    dah = ms >= (d->dit_avg + d->dah_avg) / 2u;
    if (dah) {
        d->dah_avg = (3u * d->dah_avg + ms) / 4u;
        d->dit_avg = (3u * d->dit_avg + d->dah_avg / 3u) / 4u;   /* a sender of only dahs (T M O) moves both */
        d->dahs++;
        d->dah_sum += ms;
    } else {
        d->dit_avg = (3u * d->dit_avg + ms) / 4u;
        d->dah_avg = (3u * d->dah_avg + 3u * d->dit_avg) / 4u;
        d->dits++;
        d->dit_sum += ms;
    }
    if (d->dit_avg < 10u)
        d->dit_avg = 10u;
    if (d->dah_avg < 2u * d->dit_avg)
        d->dah_avg = 2u * d->dit_avg;
    d->marks++;
    d->prev = ms;
    if (d->pat >= 0x80u || !d->pat)
        d->pat = 0;                      /* longer than any character: unknown at its end */
    else
        d->pat = (uint8_t)(d->pat << 1 | (dah ? 1u : 0u));
}

static char mo_dec_end(mo_dec_t *d)
{
    char c = d->pat ? mo_char(d->pat) : 0;
    d->last_pat = d->pat;
    d->pat = 1;
    d->prev = 0;
    d->word = 1;
    return c ? c : MO_UNKNOWN;
}

static uint32_t mo_dec_word(const mo_dec_t *d)   /* a gap this long ends a word */
{
    uint32_t u = mo_dec_unit(d), w = d->cgap * 5u / 3u;
    return w > 4u * u ? w : 4u * u;
}

char mo_dec_idle(mo_dec_t *d, uint32_t ms)
{
    uint32_t u = mo_dec_unit(d);
    if (d->pat != 1u && ms >= 2u * u)
        return mo_dec_end(d);
    if (d->word && d->pat == 1u && ms >= mo_dec_word(d)) {
        d->word = 0;
        return ' ';
    }
    return 0;
}

uint32_t mo_dec_gap(mo_dec_t *d, uint32_t ms, char *out)
{
    uint32_t n = 0, u = mo_dec_unit(d);
    char c;
    if (ms >= 2u * u && ms < mo_dec_word(d))   /* a gap between characters: learn the spacing (Farnsworth, a hand) */
        d->cgap = (3u * d->cgap + ms) / 4u;
    else if (ms < 2u * u && d->pat > 1u) {      /* inside a character */
        d->egap_sum += ms;
        d->egap_n++;
    }
    while (n < 2u && (c = mo_dec_idle(d, ms)) != 0)
        out[n++] = c;
    return n;
}

uint32_t mo_dec_wpm(const mo_dec_t *d)
{
    uint32_t u = mo_dec_unit(d);
    return u ? (1200u + u / 2u) / u : 0;
}

uint8_t mo_dec_pending(const mo_dec_t *d) { return d->pat; }

uint32_t mo_dec_gap10(const mo_dec_t *d)
{
    uint32_t dit = d->dits ? d->dit_sum / d->dits : d->dit_avg;
    if (!d->egap_n || !dit)
        return 0;
    return (d->egap_sum / d->egap_n * 10u + dit / 2u) / dit;
}

uint32_t mo_dec_ratio10(const mo_dec_t *d)
{
    uint32_t dit, dah;
    if (!d->dits || !d->dahs)
        return 0;
    dit = d->dit_sum / d->dits;
    dah = d->dah_sum / d->dahs;
    return dit ? (dah * 10u + dit / 2u) / dit : 0;
}

void mo_dec_restart(mo_dec_t *d)
{
    d->pat = 1;
    d->word = 0;
    d->last_pat = 0;
    d->prev = 0;
    mo_dec_reset_stats(d);
}

void mo_dec_teach(mo_dec_t *d, const uint16_t *mark, uint32_t n, uint8_t want)
{
    uint32_t i, ds = 0, dn = 0, hs = 0, hn = 0;
    if (!want || mo_len(want) != n)
        return;
    for (i = 0; i < n; i++)
        if (mo_elem(want, i))
            hs += mark[i], hn++;
        else
            ds += mark[i], dn++;
    if (dn)
        d->dit_avg = (d->dit_avg + ds / dn) / 2u;
    else
        d->dit_avg = (d->dit_avg + hs / hn / 3u) / 2u;
    if (hn)
        d->dah_avg = (d->dah_avg + hs / hn) / 2u;
    else
        d->dah_avg = (d->dah_avg + 3u * ds / dn) / 2u;
    if (d->dit_avg < 10u)
        d->dit_avg = 10u;
    if (d->dah_avg < 2u * d->dit_avg)
        d->dah_avg = 2u * d->dit_avg;
}

/* ------------------------------------------------------------------- judge --- */
static uint32_t mo_dev(uint32_t got, uint32_t want)   /* how far got is off want, in % (at most 100) */
{
    uint32_t d, p;
    if (!want)
        return 100u;
    d = got > want ? got - want : want - got;
    p = d * 100u / want;
    return p > 100u ? 100u : p;
}

static uint32_t mo_spread(uint32_t lo, uint32_t hi, uint32_t sum, uint32_t n)   /* marks of one kind: max - min, % */
{
    uint32_t avg = n ? sum / n : 0u;
    if (n < 2u || !avg)
        return 0;
    return (hi - lo) * 100u / avg > 100u ? 100u : (hi - lo) * 100u / avg;
}

mo_judge_t mo_judge(const uint16_t *mark, const uint16_t *gap, uint32_t n, uint8_t want, uint32_t dit_est)
{
    mo_judge_t j = {MJ_WRONG, MH_NONE, 0, 0};
    uint32_t len = mo_len(want), i, dmin = 0xFFFFFFFFu, dmax = 0, hmin = 0xFFFFFFFFu, hmax = 0;
    uint32_t ds = 0, dn = 0, hs = 0, hn = 0, gs = 0, gn = 0, gmax = 0, dit, q = 0, dev_r, dev_g = 0, sp, sd, sh;
    int32_t score;
    if (dit_est < 10u)
        dit_est = 10u;
    j.dit = (uint16_t)dit_est;
    if (!want || !n) {
        j.hint = MH_MISSING;
        return j;
    }
    if (n != len) {
        j.hint = n > len ? MH_EXTRA : MH_MISSING;
        return j;
    }
    for (i = 0; i < n; i++) {
        uint32_t m = mark[i];
        if (mo_elem(want, i)) {
            hs += m, hn++;
            hmin = m < hmin ? m : hmin;
            hmax = m > hmax ? m : hmax;
        } else {
            ds += m, dn++;
            dmin = m < dmin ? m : dmin;
            dmax = m > dmax ? m : dmax;
        }
        if (i) {
            gs += gap[i], gn++;
            gmax = gap[i] > gmax ? gap[i] : gmax;
        }
    }
    dit = dn ? ds / dn : hs / hn / 3u;
    if (!dit)
        dit = 1;
    j.dit = (uint16_t)(dit > 0xFFFFu ? 0xFFFFu : dit);
    if (dn && hn) {                      /* its own contrast says which is which */
        q = (hs / hn) * 10u / (ds / dn ? ds / dn : 1u);
        dev_r = mo_dev(q, 30u);
        if (hmin * 10u >= dmax * 15u)
            j.verdict = MJ_RIGHT;
        else {
            j.verdict = hmin * 10u >= dmax * 12u ? MJ_ALMOST : MJ_WRONG;
            /* a dit well over the other dits is the odd one; else a dah came out short */
            j.hint = dn >= 2u && dmax * 10u >= dmin * 15u ? MH_DIT_LONG : MH_DAH_SHORT;
        }
    } else if (hn) {                     /* dahs only: against this hand's dit */
        dev_r = mo_dev(hs / hn, 3u * dit_est) / 2u;
        if (hmax >= 2u * hmin) {         /* a dit and a dah inside it after all */
            j.verdict = MJ_WRONG;
            j.hint = MH_DAH_SHORT;
        } else if (hmin * 10u >= dit_est * 16u)
            j.verdict = MJ_RIGHT;
        else {
            j.verdict = hmin * 10u >= dit_est * 13u ? MJ_ALMOST : MJ_WRONG;
            j.hint = MH_DAH_SHORT;
        }
    } else {                             /* dits only */
        dev_r = mo_dev(ds / dn, dit_est) / 2u;
        if (dmax >= 2u * dmin) {
            j.verdict = MJ_WRONG;
            j.hint = MH_DIT_LONG;
        } else if (dmax * 10u <= dit_est * 24u)
            j.verdict = MJ_RIGHT;
        else {
            j.verdict = dmax * 10u <= dit_est * 30u ? MJ_ALMOST : MJ_WRONG;
            j.hint = MH_DIT_LONG;
        }
    }
    if (gn)
        dev_g = mo_dev(gs / gn, dit);
    sd = mo_spread(dmin, dmax, ds, dn);
    sh = mo_spread(hmin, hmax, hs, hn);
    sp = sd > sh ? sd : sh;
    score = 100 - (int32_t)(dev_r * 3u / 2u) - (int32_t)(dev_g / 3u) - (int32_t)(sp / 3u);
    if (gn && gmax * 10u >= dit * 25u) { /* a gap inside it near a letter's: it would split for a listener */
        if (j.verdict == MJ_RIGHT)
            j.verdict = MJ_ALMOST;
        if (j.hint == MH_NONE || j.verdict == MJ_ALMOST)
            j.hint = MH_GAP_LONG;
    }
    if (j.verdict == MJ_WRONG && score > 40)
        score = 40;
    j.score = (uint8_t)(score < 0 ? 0 : score > 100 ? 100 : score);
    if (j.hint == MH_NONE && j.score < 85u) {   /* right: what costs the most */
        uint32_t cr = dev_r * 3u / 2u, cg = dev_g / 3u, cs = sp / 3u;
        if (cr >= cg && cr >= cs && cr >= 8u) {
            if (dn && hn)
                j.hint = q < 30u ? MH_DAH_SHORT : MH_DAH_LONG;
            else if (hn)                 /* dahs only: against the hand's dit */
                j.hint = hs / hn < 3u * dit_est ? MH_DAH_SHORT : MH_DAH_LONG;
            else
                j.hint = ds / dn > dit_est ? MH_DIT_LONG : MH_NONE;
        } else if (cg >= cs && cg >= 8u && gn && gs / gn > dit)
            j.hint = MH_GAP_LONG;
        else if (cs >= 8u)
            j.hint = MH_UNEVEN;
    }
    return j;
}
