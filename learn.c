/* SPDX-License-Identifier: MIT */
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
    uint32_t pool = s->open < 4u ? 4u : s->open, score[LN_N], i, k, pick[3];
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
    r->n = r->at = r->ok = r->miss = r->up = r->fresh = 0;
    r->rng = seed ? seed : 0x9E3779B9u;
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
    if (nq > n)
        nq = n > first ? n : first;
    for (i = nq; i > 1u; i--) {          /* asked in a shuffled order */
        uint32_t k = ln_rand(&r->rng) % i;
        uint8_t c = qch[i - 1u], dd = qdir[i - 1u];
        qch[i - 1u] = qch[k], qdir[i - 1u] = qdir[k];
        qch[k] = c, qdir[k] = dd;
    }
    for (i = 0; i < nq; i++)
        ln_add(s, r, qch[i], qdir[i], flags);
}

const ln_card_t *ln_card(const ln_round_t *r) { return r->at < r->n ? &r->card[r->at] : 0; }

int ln_answer(ln_state_t *s, ln_round_t *r, int right)
{
    ln_card_t *c;
    uint8_t *box;
    int up = 0;
    if (r->at >= r->n)
        return 0;
    c = &r->card[r->at];
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
    uint32_t i, asked = r->ok + r->miss;
    if (!asked)
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
