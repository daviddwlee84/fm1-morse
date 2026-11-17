/* SPDX-License-Identifier: GPL-3.0-or-later */
/* learn: see learn.h. Needs morse.h's codes (how alike two characters sound, for the wrong choices). */
#include "learn.h"
#include "morse.h"

/* LCWO's Koch order (lcwo.net inc/functions.php) */
static const char LN_ORDER[LN_N + 1] = "KMURESNAPTLWI.JZ=FOY,VG5/Q92H38B?47C1D60X";

char ln_char(uint32_t i) { return i < LN_N ? LN_ORDER[i] : 0; }

int ln_index(char c)
{
    uint32_t i;
    if (c >= 'a' && c <= 'z')
        c = (char)(c - 32);
    for (i = 0; i < LN_N; i++)
        if (LN_ORDER[i] == c)
            return (int)i;
    return -1;
}

void ln_init(ln_state_t *s)
{
    uint32_t i, d;
    s->round = 0;
    s->streak = 0;
    s->open = LN_GROUP;
    s->rsv = 0;
    for (i = 0; i < LN_N; i++)
        for (d = 0; d < LN_NDIR; d++) {
            s->box[i][d] = 0;
            s->last[i][d] = LN_NEVER;
        }
    s->answered = s->right = 0;
}

int ln_valid(const ln_state_t *s)
{
    uint32_t i, d;
    if (s->open < LN_GROUP || s->open > LN_N || s->right > s->answered)
        return 0;
    for (i = 0; i < LN_N; i++)
        for (d = 0; d < LN_NDIR; d++)
            if (s->box[i][d] > LN_TOP || (s->last[i][d] != LN_NEVER && s->last[i][d] > s->round))
                return 0;
    return 1;
}

static uint32_t ln_rand(uint32_t *x)     /* xorshift32 */
{
    uint32_t v = *x ? *x : 1u;
    v ^= v << 13;
    v ^= v >> 17;
    v ^= v << 5;
    return *x = v;
}

int ln_due(const ln_state_t *s, uint32_t i, int dir)
{
    if (i >= s->open)
        return 0;
    if (s->last[i][dir] == LN_NEVER)     /* sending starts once it was recognised; a new character is not "due" */
        return dir == LN_SEND && s->box[i][LN_REC] >= 1u;
    return (uint32_t)(uint16_t)(s->round - s->last[i][dir]) >= (1u << s->box[i][dir]);
}

uint32_t ln_due_count(const ln_state_t *s)
{
    uint32_t i, n = 0;
    for (i = 0; i < s->open; i++)
        n += (uint32_t)ln_due(s, i, LN_REC) + (uint32_t)ln_due(s, i, LN_SEND);
    return n;
}

uint32_t ln_new_count(const ln_state_t *s)
{
    uint32_t i, n = 0;
    for (i = 0; i < s->open; i++)
        n += s->last[i][LN_REC] == LN_NEVER;
    return n;
}

/* recognising: pick from four first, recall it later. By sound it is always heard (muted: the lamp's rhythm); by
 * sight the first boxes show the code drawn, then it is heard */
static uint8_t ln_type(int dir, uint32_t box, uint32_t flags)
{
    if (dir == LN_SEND)
        return CARD_SEE_KEY;
    if (flags & LN_MUTED)
        return (flags & LN_VISUAL) ? CARD_CODE_CHOOSE : CARD_LIGHT_CHOOSE;
    if ((flags & LN_VISUAL) && box < 2u)
        return CARD_CODE_CHOOSE;
    return box >= 3u ? CARD_HEAR_KEY : CARD_HEAR_CHOOSE;
}

int ln_choose(int type) { return type == CARD_HEAR_CHOOSE || type == CARD_CODE_CHOOSE || type == CARD_LIGHT_CHOOSE; }

static uint32_t ln_alike(uint32_t a, uint32_t b)   /* how alike two characters' codes are */
{
    uint8_t ca = mo_code(ln_char(a)), cb = mo_code(ln_char(b));
    uint32_t la = mo_len(ca), lb = mo_len(cb), k, s = la == lb ? 3u : 0u;
    for (k = 0; k < la && k < lb; k++)
        s += mo_elem(ca, k) == mo_elem(cb, k);
    return s;
}

static void ln_options(const ln_state_t *s, ln_round_t *r, ln_card_t *c)
{
    uint32_t top = r->place ? (s->open + LN_PLACE_N > LN_N ? LN_N : s->open + LN_PLACE_N) : s->open;
    uint32_t pool = top < 4u ? 4u : top, score[LN_N], i, k, pick[3];
    for (i = 0; i < pool; i++)
        score[i] = i == c->ch ? 0u : 1u + ln_alike(c->ch, i) * 4u + ln_rand(&r->rng) % 6u;
    for (k = 0; k < 3u; k++) {
        uint32_t best = 0;
        for (i = 1; i < pool; i++)
            if (score[i] > score[best])
                best = i;
        pick[k] = best;
        score[best] = 0;
    }
    c->right = (uint8_t)(ln_rand(&r->rng) % 4u);
    for (i = 0, k = 0; i < 4u; i++)
        c->opt[i] = (uint8_t)(i == c->right ? c->ch : pick[k++]);
}

static void ln_add(const ln_state_t *s, ln_round_t *r, uint32_t ch, int dir, uint32_t flags)
{
    ln_card_t *c;
    if (r->n >= LN_CARDS_MAX)
        return;
    c = &r->card[r->n++];
    c->ch = (uint8_t)ch;
    c->dir = (uint8_t)dir;
    c->again = 0;
    c->type = ln_type(dir, s->box[ch][dir], flags);
    c->right = 0;
    c->opt[0] = c->opt[1] = c->opt[2] = c->opt[3] = (uint8_t)ch;
    if (ln_choose(c->type))
        ln_options(s, r, c);
}

void ln_build(const ln_state_t *s, ln_round_t *r, uint32_t n, uint32_t flags, uint32_t seed)
{
    uint8_t qch[LN_N * LN_NDIR], qdir[LN_N * LN_NDIR], used[LN_N][LN_NDIR];
    uint32_t key[LN_N * LN_NDIR];
    uint32_t nq = 0, i, d, j, fresh = 0, first;
    r->n = r->at = r->ok = r->miss = r->up = r->fresh = r->near = 0;
    r->place = (uint8_t)((flags & LN_PLACE) != 0u);
    r->known[0] = r->known[1] = 0;
    r->rng = seed ? seed : 0x9E3779B9u;
    if (r->place) {                      /* the next two groups, each once, shuffled; recognising, at full skill */
        uint32_t top = s->open + LN_PLACE_N > LN_N ? LN_N : s->open + LN_PLACE_N;
        for (i = s->open; i < top; i++)
            qch[nq++] = (uint8_t)i;
        for (i = nq; i > 1u; i--) {
            uint32_t k = ln_rand(&r->rng) % i;
            uint8_t c = qch[i - 1u];
            qch[i - 1u] = qch[k], qch[k] = c;
        }
        for (i = 0; i < nq; i++) {
            ln_card_t *c;
            ln_add(s, r, qch[i], LN_REC, flags);
            c = &r->card[r->n - 1u];
            c->type = ln_type(LN_REC, 2u, flags);
            if (ln_choose(c->type))
                ln_options(s, r, c);
        }
        return;
    }
    if (n > LN_CARDS_MAX / 2u)
        n = LN_CARDS_MAX / 2u;
    if (n < 4u)
        n = 4u;
    for (i = 0; i < LN_N; i++)
        used[i][0] = used[i][1] = 0;
    /* new characters: each shown first, then asked */
    for (i = 0; i < s->open && fresh < LN_NEW_MAX; i++)
        if (s->last[i][LN_REC] == LN_NEVER) {
            ln_add(s, r, i, LN_REC, flags);
            r->card[r->n - 1u].type = CARD_NEW;
            qch[nq] = (uint8_t)i, qdir[nq] = LN_REC, key[nq] = 0, nq++;
            used[i][LN_REC] = 1;
            fresh++;
        }
    first = nq;
    /* the due ones, lowest box and longest waiting first; then the weakest of the rest, to fill the round */
    for (j = 0; j < 2u; j++)
        for (i = 0; i < s->open; i++)
            for (d = 0; d < LN_NDIR; d++) {
                uint32_t wait;
                if (used[i][d] || (j == 0u) != (ln_due(s, i, (int)d) != 0))
                    continue;
                if (j == 1u && s->last[i][d] == LN_NEVER)
                    continue;            /* not yet seen, not yet due: a later round */
                wait = s->last[i][d] == LN_NEVER ? 0u : (uint16_t)(s->round - s->last[i][d]);
                qch[nq] = (uint8_t)i;
                qdir[nq] = (uint8_t)d;
                key[nq] = j * 0x10000000u + s->box[i][d] * 0x100000u + (0xFFFFu - (wait > 0xFFFFu ? 0xFFFFu : wait)) * 16u
                          + ln_rand(&r->rng) % 16u;
                used[i][d] = 1;
                nq++;
            }
    for (i = first + 1u; i < nq; i++) {  /* by key, new ones kept first */
        uint32_t k = key[i];
        uint8_t c = qch[i], dd = qdir[i];
        for (j = i; j > first && key[j - 1u] > k; j--) {
            key[j] = key[j - 1u];
            qch[j] = qch[j - 1u];
            qdir[j] = qdir[j - 1u];
        }
        key[j] = k, qch[j] = c, qdir[j] = dd;
    }
    {
        /* the new and the due first; then the weak ones of them (box 0-1, the new ones too) once more later in
         * the round, like a deck that repeats a new card the day it comes; only then the rest, weakest first */
        uint32_t ndue = first, base, weak[LN_N * LN_NDIR], nw = 0, nrep, nfill;
        while (ndue < nq && key[ndue] < 0x10000000u)
            ndue++;
        base = ndue > n ? (n > first ? n : first) : ndue;
        for (i = 0; i < base; i++)
            if (s->box[qch[i]][qdir[i]] <= 1u)
                weak[nw++] = i;
        nrep = nw < (n > base ? n - base : 0u) ? nw : (n > base ? n - base : 0u);
        nfill = nq - ndue;
        if (nfill > n - base - nrep)
            nfill = n - base - nrep;
        for (i = 0; i < nfill; i++) {    /* the fill moves up behind the base */
            qch[base + i] = qch[ndue + i];
            qdir[base + i] = qdir[ndue + i];
        }
        {
            uint8_t rch[LN_N * LN_NDIR], rdir[LN_N * LN_NDIR];
            uint32_t m = base + nfill;
            for (i = 0; i < nrep; i++)
                rch[i] = qch[weak[i]], rdir[i] = qdir[weak[i]];
            for (i = m; i > 1u; i--) {   /* asked in a shuffled order */
                uint32_t k = ln_rand(&r->rng) % i;
                uint8_t c = qch[i - 1u], dd = qdir[i - 1u];
                qch[i - 1u] = qch[k], qdir[i - 1u] = qdir[k];
                qch[k] = c, qdir[k] = dd;
            }
            for (i = 0; i < m; i++)
                ln_add(s, r, qch[i], qdir[i], flags);
            for (i = nrep; i > 1u; i--) {
                uint32_t k = ln_rand(&r->rng) % i;
                uint8_t c = rch[i - 1u], dd = rdir[i - 1u];
                rch[i - 1u] = rch[k], rdir[i - 1u] = rdir[k];
                rch[k] = c, rdir[k] = dd;
            }
            for (i = 0; i < nrep; i++) { /* the second time: right does not move it a second box */
                ln_add(s, r, rch[i], rdir[i], flags);
                r->card[r->n - 1u].again = 1;
            }
        }
    }
}

const ln_card_t *ln_card(const ln_round_t *r) { return r->at < r->n ? &r->card[r->at] : 0; }

int ln_answer(ln_state_t *s, ln_round_t *r, int result)
{
    ln_card_t *c;
    uint8_t *box;
    int up = 0, right = result == LN_RIGHT;
    if (r->at >= r->n)
        return 0;
    c = &r->card[r->at];
    if (r->place) {                      /* nothing changes until ln_place_apply */
        if (right) {
            r->ok++;
            r->known[c->ch / 32u] |= 1u << (c->ch % 32u);
        } else
            r->miss++;
        r->at++;
        return 0;
    }
    if (c->type == CARD_NEW) {
        r->fresh++;
        r->at++;
        return 0;
    }
    box = &s->box[c->ch][c->dir];
    s->answered++;
    if (right) {
        s->right++;
        r->ok++;
        if (!c->again && *box < LN_TOP) {  /* right after a miss in the same round: stays in box 0 */
            (*box)++;
            r->up++;
            up = 1;
        }
    } else if (result == LN_ALMOST) {    /* close: the box stays, asked once more */
        r->near++;
        if (!c->again && r->n < LN_CARDS_MAX) {
            ln_card_t *a = &r->card[r->n++];
            *a = *c;
            a->again = 1;
        }
    } else {
        r->miss++;
        *box = 0;
        if (!c->again && r->n < LN_CARDS_MAX) {   /* asked once more at the end of the round */
            ln_card_t *a = &r->card[r->n++];
            *a = *c;
            a->again = 1;
            if (ln_choose(a->type))
                ln_options(s, r, a);
        }
    }
    s->last[c->ch][c->dir] = s->round;
    r->at++;
    return up;
}

int ln_finish(ln_state_t *s, const ln_round_t *r)
{
    uint32_t i, asked = r->ok + r->miss + r->near;
    if (!asked || r->place)
        return 0;
    s->round++;
    if (r->at >= r->n)                   /* a whole round: the streak; one left halfway does not change it */
        s->streak = r->ok * 100u >= asked * 80u ? (uint16_t)(s->streak + 1u) : 0u;
    if (s->open >= LN_N)
        return 0;
    for (i = 0; i < s->open; i++)
        if (s->box[i][LN_REC] < 2u || s->box[i][LN_SEND] < 1u)
            return 0;
    s->open = (uint8_t)(s->open + LN_GROUP > LN_N ? LN_N : s->open + LN_GROUP);
    return 1;
}

uint32_t ln_level(const ln_state_t *s, uint32_t i)
{
    if (i >= LN_N)
        return 0;
    return (s->box[i][LN_REC] + s->box[i][LN_SEND]) / 2u;
}

/* ----------------------------------------------------------- by hand --- */
static void ln_seen(ln_state_t *s, uint32_t i)   /* answered now, as far as the schedule goes */
{
    s->last[i][LN_REC] = s->last[i][LN_SEND] = s->round;
}

static void ln_open_raw(ln_state_t *s, uint32_t n)   /* more open: the new ones at the line the next group needs */
{
    uint32_t i;
    n = (n + LN_GROUP - 1u) / LN_GROUP * LN_GROUP;
    n = n < LN_GROUP ? LN_GROUP : n > LN_N ? LN_N : n;
    for (i = s->open; i < n; i++) {
        if (s->box[i][LN_REC] < 2u)
            s->box[i][LN_REC] = 2u;
        if (s->box[i][LN_SEND] < 1u)
            s->box[i][LN_SEND] = 1u;
        ln_seen(s, i);
    }
    if (n > s->open)
        s->open = (uint8_t)n;
}

void ln_set_level(ln_state_t *s, uint32_t i, uint32_t lv)
{
    if (i >= LN_N)
        return;
    if (lv > LN_TOP)
        lv = LN_TOP;
    if (i >= s->open)
        ln_open_raw(s, i + 1u);
    s->box[i][LN_REC] = s->box[i][LN_SEND] = (uint8_t)lv;
    if (lv == 0u) {                      /* back to new: shown again, then asked */
        s->last[i][LN_REC] = s->last[i][LN_SEND] = LN_NEVER;
    } else
        ln_seen(s, i);
}

void ln_mark_known(ln_state_t *s, uint32_t i)
{
    if (i >= LN_N)
        return;
    if (i >= s->open)
        ln_open_raw(s, i + 1u);
    if (s->box[i][LN_REC] < 3u)
        s->box[i][LN_REC] = 3u;
    if (s->box[i][LN_SEND] < 2u)
        s->box[i][LN_SEND] = 2u;
    ln_seen(s, i);
}

void ln_open_to(ln_state_t *s, uint32_t n)
{
    uint32_t i;
    n = (n + LN_GROUP - 1u) / LN_GROUP * LN_GROUP;
    n = n < LN_GROUP ? LN_GROUP : n > LN_N ? LN_N : n;
    if (n >= s->open) {
        ln_open_raw(s, n);
        return;
    }
    for (i = n; i < s->open; i++) {
        s->box[i][LN_REC] = s->box[i][LN_SEND] = 0;
        s->last[i][LN_REC] = s->last[i][LN_SEND] = LN_NEVER;
    }
    s->open = (uint8_t)n;
}

void ln_step_back(ln_state_t *s)
{
    uint32_t i, d;
    for (i = 0; i < LN_N; i++)
        for (d = 0; d < LN_NDIR; d++)
            if (s->box[i][d])
                s->box[i][d]--;
}

uint32_t ln_place_count(const ln_round_t *r)
{
    uint32_t i, n = 0;
    for (i = 0; i < LN_N; i++)
        n += r->known[i / 32u] >> (i % 32u) & 1u;
    return n;
}

uint32_t ln_place_apply(ln_state_t *s, const ln_round_t *r)
{
    uint32_t i, g, n = 0, open = s->open;
    if (!r->place)
        return 0;
    for (g = open; g < LN_N; g += LN_GROUP) {   /* whole groups right, in turn: opened */
        uint32_t end = g + LN_GROUP > LN_N ? LN_N : g + LN_GROUP, all = 1;
        for (i = g; i < end; i++)
            all &= r->known[i / 32u] >> (i % 32u) & 1u;
        if (!all)
            break;
        ln_open_raw(s, end);
    }
    for (i = 0; i < LN_N; i++)
        if (r->known[i / 32u] >> (i % 32u) & 1u) {
            if (i < s->open) {
                ln_mark_known(s, i);
                n++;
            }
        }
    return n;
}

/* ------------------------------------------------------------ a backup --- */
static uint8_t *ln_put(uint8_t *b, uint32_t v, uint32_t n)
{
    uint32_t k;
    for (k = 0; k < n; k++)
        *b++ = (uint8_t)(v >> (8u * k));
    return b;
}

static uint32_t ln_get(const uint8_t **b, uint32_t n)
{
    uint32_t v = 0, k;
    for (k = 0; k < n; k++)
        v |= (uint32_t)(*(*b)++) << (8u * k);
    return v;
}

void ln_blob(const ln_state_t *s, uint8_t *b)
{
    uint32_t i, d;
    b = ln_put(b, s->round, 2);
    b = ln_put(b, s->streak, 2);
    b = ln_put(b, s->open, 1);
    b = ln_put(b, s->answered, 4);
    b = ln_put(b, s->right, 4);
    for (i = 0; i < LN_N; i++)
        for (d = 0; d < LN_NDIR; d++)
            *b++ = s->box[i][d];
    for (i = 0; i < LN_N; i++)
        for (d = 0; d < LN_NDIR; d++)
            b = ln_put(b, s->last[i][d], 2);
}

int ln_unblob(ln_state_t *s, const uint8_t *b)
{
    ln_state_t t;
    uint32_t i, d;
    t.round = (uint16_t)ln_get(&b, 2);
    t.streak = (uint16_t)ln_get(&b, 2);
    t.open = (uint8_t)ln_get(&b, 1);
    t.rsv = 0;
    t.answered = ln_get(&b, 4);
    t.right = ln_get(&b, 4);
    for (i = 0; i < LN_N; i++)
        for (d = 0; d < LN_NDIR; d++)
            t.box[i][d] = *b++;
    for (i = 0; i < LN_N; i++)
        for (d = 0; d < LN_NDIR; d++)
            t.last[i][d] = (uint16_t)ln_get(&b, 2);
    if (!ln_valid(&t))
        return 0;
    *s = t;
    return 1;
}
