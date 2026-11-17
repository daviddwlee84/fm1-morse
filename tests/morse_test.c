/* SPDX-License-Identifier: MIT */
/* morse.c and learn.c on the computer: the code table, timing, player, keyer, decoder and flash cards.
 *   just morse-unit */
#include <stdio.h>
#include <string.h>
#include "../morse.c"
#include "../learn.c"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static uint32_t rng = 12345u;
static uint32_t rnd(void) { return ln_rand(&rng); }

/* ------------------------------------------------------------------- table --- */
static void test_table(void)
{
    static const char ALL[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.,?'!/()&:;=+-_\"$@";
    char pat[8];
    uint32_t i;
    for (i = 0; ALL[i]; i++) {
        uint8_t c = mo_code(ALL[i]);
        CHECK(c > 1u, "code for %c", ALL[i]);
        CHECK(mo_char(c) == ALL[i], "round trip %c", ALL[i]);
    }
    CHECK(mo_code('a') == mo_code('A'), "lower case");
    CHECK(mo_code('#') == 0, "no code for #");
    CHECK(mo_char(0) == 0 && mo_char(1) == 0, "no character for 0 / 1");
    mo_pattern(mo_code('A'), pat);
    CHECK(!strcmp(pat, ".-"), "A is .- (%s)", pat);
    mo_pattern(mo_code('0'), pat);
    CHECK(!strcmp(pat, "-----"), "0 is ----- (%s)", pat);
    mo_pattern(mo_code('$'), pat);
    CHECK(!strcmp(pat, "...-..-"), "$ has 7 elements (%s)", pat);
    CHECK(mo_len(mo_code('E')) == 1 && mo_units(mo_code('E')) == 1, "E: one unit");
    CHECK(mo_units(mo_code('P')) == 11, "P: .--. is 11 units");
    /* PARIS: the characters' units, 4 gaps of 3 between them, a word gap of 7 */
    CHECK(mo_units(mo_code('P')) + mo_units(mo_code('A')) + mo_units(mo_code('R')) + mo_units(mo_code('I'))
              + mo_units(mo_code('S')) + 4 * 3 + 7 == 50, "PARIS is 50 units");
}

/* ------------------------------------------------------------------ timing --- */
static uint32_t play_ms(const char *text, uint32_t wpm, uint32_t eff)
{
    mo_player_t p;
    uint32_t t = 0;
    mo_player_init(&p, mo_dit_ms(wpm), mo_gap_ms(wpm, eff));
    while (*text)
        mo_player_push(&p, *text++);
    while (mo_player_busy(&p)) {
        mo_player_tick(&p);
        t++;
    }
    return t;
}

static void test_timing(void)
{
    uint32_t t;
    CHECK(mo_dit_ms(20) == 60, "20 WPM: 60 ms dit");
    CHECK(mo_dit_ms(5) == 240, "5 WPM: 240 ms dit");
    CHECK(mo_gap_ms(20, 20) == 60 && mo_gap_ms(20, 0) == 60 && mo_gap_ms(20, 25) == 60, "no Farnsworth: gap = dit");
    CHECK(mo_gap_ms(20, 10) == 217, "20/10 WPM gap unit 217 (%u)", mo_gap_ms(20, 10));
    t = play_ms("PARIS ", 20, 20);
    CHECK(t == 3000, "PARIS at 20 WPM takes 3 s (%u)", t);
    t = play_ms("PARIS ", 20, 10);
    CHECK(t > 5900 && t <= 6000, "PARIS at 20/10 WPM takes about 6 s (%u)", t);
    t = play_ms("E", 10, 10);
    CHECK(t == 120 + 3 * 120, "E then the character gap (%u)", t);
    t = play_ms("#E", 20, 20);
    CHECK(t == 60 + 180, "unknown characters skipped (%u)", t);
}

static void test_player_queue(void)
{
    mo_player_t p;
    uint32_t i, n = 0;
    mo_player_init(&p, 10, 10);
    for (i = 0; i < MO_PLAY_Q + 5u; i++)
        n += mo_player_push(&p, 'E') == 0;
    CHECK(n == MO_PLAY_Q - 1u, "queue holds %u", n);
    mo_player_tick(&p);
    CHECK(p.cur == 'E' && p.started, "cur and started");
    mo_player_stop(&p);
    CHECK(!mo_player_busy(&p) && mo_player_tick(&p) == 0, "stopped");
}

/* -------------------------------------------------------- player -> decoder --- */
/* the player's key-down / key-up, each run stretched by up to +-jitter %, into the decoder */
static void decode_run(const char *text, uint32_t wpm, uint32_t eff, uint32_t dec_wpm, uint32_t jitter, char *out)
{
    mo_player_t p;
    mo_dec_t d;
    uint32_t n = 0, run = 0, len;
    int last = 0, k;
    char c[2];
    mo_player_init(&p, mo_dit_ms(wpm), mo_gap_ms(wpm, eff));
    mo_dec_init(&d, mo_dit_ms(dec_wpm), mo_gap_ms(dec_wpm, eff == wpm ? dec_wpm : eff));
    while (*text)
        mo_player_push(&p, *text++);
    mo_player_push(&p, ' ');
    for (;;) {
        int busy = mo_player_busy(&p);
        k = busy ? mo_player_tick(&p) : 0;
        if (k == last && busy) {
            run++;
            continue;
        }
        len = run;
        if (jitter)
            len = (uint32_t)((int32_t)len + (int32_t)len * ((int32_t)(rnd() % (2 * jitter + 1)) - (int32_t)jitter) / 100);
        if (last)
            mo_dec_mark(&d, len);
        else if (run) {
            uint32_t g = mo_dec_gap(&d, busy ? len : 100000u, c), i;
            for (i = 0; i < g; i++)
                out[n++] = c[i];
        }
        if (!busy) {
            uint32_t g = mo_dec_gap(&d, 100000u, c), i;
            for (i = 0; i < g; i++)
                out[n++] = c[i];
            break;
        }
        last = k;
        run = 1;
    }
    while (n && out[n - 1] == ' ')
        n--;
    out[n] = 0;
}

static uint32_t diff(const char *a, const char *b)   /* edit distance */
{
    uint32_t row[256], la = (uint32_t)strlen(a), lb = (uint32_t)strlen(b), i, j;
    for (j = 0; j <= lb; j++)
        row[j] = j;
    for (i = 1; i <= la; i++) {
        uint32_t prev = row[0], t;
        row[0] = i;
        for (j = 1; j <= lb; j++) {
            t = row[j];
            row[j] = a[i - 1] == b[j - 1] ? prev : 1u + (prev < row[j] ? (prev < row[j - 1] ? prev : row[j - 1])
                                                                       : (row[j] < row[j - 1] ? row[j] : row[j - 1]));
            prev = t;
        }
    }
    return row[lb];
}

static void test_decoder(void)
{
    static const char TEXT[] = "THE QUICK BROWN FOX JUMPS OVER THE LAZY DOG 0123456789 .,?/=";
    static const uint32_t W[] = {5, 15, 25, 35};
    char out[256];
    uint32_t i;
    for (i = 0; i < 4; i++) {
        decode_run(TEXT, W[i], W[i], W[i], 0, out);
        CHECK(!strcmp(out, TEXT), "%u WPM exact: %s", W[i], out);
        decode_run(TEXT, W[i], W[i], W[i], 20, out);
        CHECK(!strcmp(out, TEXT), "%u WPM, +-20 %% jitter: %s", W[i], out);
    }
    decode_run("CQ CQ DE FM1 K", 20, 8, 20, 10, out);
    CHECK(!strcmp(out, "CQ CQ DE FM1 K"), "Farnsworth 20/8: %s", out);
    /* started at 20, the sender 30 % slower and faster: it follows within a few characters */
    decode_run(TEXT, 14, 14, 20, 10, out);
    CHECK(diff(out, TEXT) <= 3, "decoder at 20, sender 14 (%u off): %s", diff(out, TEXT), out);
    decode_run(TEXT, 28, 28, 20, 10, out);
    CHECK(diff(out, TEXT) <= 3, "decoder at 20, sender 28 (%u off): %s", diff(out, TEXT), out);
    {
        mo_dec_t d;
        char c[2];
        uint32_t k;
        mo_dec_init(&d, 60, 60);
        for (k = 0; k < 8; k++) {
            mo_dec_mark(&d, 60);
            mo_dec_gap(&d, 60, c);
        }
        CHECK(mo_dec_idle(&d, 200) == MO_UNKNOWN, "8 dits: unknown");
        CHECK(mo_dec_idle(&d, 200) == 0, "given once");
        CHECK(mo_dec_idle(&d, 400) == ' ', "then a space");
        CHECK(mo_dec_idle(&d, 900) == 0, "one space");
        mo_dec_init(&d, 60, 60);
        mo_dec_mark(&d, 2);
        CHECK(mo_dec_pending(&d) == 1, "a 2 ms bounce is no mark");
        mo_dec_mark(&d, 60);
        mo_dec_mark(&d, 180);
        CHECK(mo_dec_ratio10(&d) == 30, "dah:dit 3.0 (%u)", mo_dec_ratio10(&d));
        CHECK(mo_dec_wpm(&d) >= 19 && mo_dec_wpm(&d) <= 21, "hears 20 WPM (%u)", mo_dec_wpm(&d));
    }
}

/* ------------------------------------------------------------------- keyer --- */
/* the keyer's output from paddle scripts (dit/dah held from..to ms), decoded */
static void keyer_run(int mode, const uint32_t (*dit)[2], uint32_t ndit, const uint32_t (*dah)[2], uint32_t ndah,
                      uint32_t ms, char *out, uint32_t *marks)
{
    mo_keyer_t k;
    mo_dec_t d;
    uint32_t t, i, n = 0, run = 0;
    int last = 0;
    char c[2];
    mo_keyer_init(&k, mode, 60);
    mo_dec_init(&d, 60, 60);
    *marks = 0;
    for (t = 0; t < ms; t++) {
        int a = 0, b = 0, o;
        for (i = 0; i < ndit; i++)
            a |= t >= dit[i][0] && t < dit[i][1];
        for (i = 0; i < ndah; i++)
            b |= t >= dah[i][0] && t < dah[i][1];
        o = mo_keyer_tick(&k, a, b);
        if (o == last) {
            run++;
            continue;
        }
        if (last) {
            mo_dec_mark(&d, run);
            (*marks)++;
        } else if (run) {
            uint32_t g = mo_dec_gap(&d, run, c), j;
            for (j = 0; j < g; j++)
                out[n++] = c[j];
        }
        last = o;
        run = 1;
    }
    if (last) {
        mo_dec_mark(&d, run);
        (*marks)++;
    }
    {
        uint32_t g = mo_dec_gap(&d, 100000u, c), j;
        for (j = 0; j < g; j++)
            out[n++] = c[j];
    }
    while (n && out[n - 1] == ' ')
        n--;
    out[n] = 0;
}

static void test_keyer(void)
{
    char out[64];
    uint32_t marks;
    static const uint32_t HOLD_DIT[][2] = {{0, 290}};
    static const uint32_t SQ_DIT[][2] = {{0, 150}}, SQ_DAH[][2] = {{30, 150}};
    static const uint32_t BOTH[][2] = {{0, 400}};
    static const uint32_t A_DAH[][2] = {{0, 100}}, A_DIT[][2] = {{200, 250}};   /* T then E by hand: a dah, a dit */
    mo_keyer_t k;
    keyer_run(MO_IAMBIC_B, HOLD_DIT, 1, 0, 0, 1000, out, &marks);
    CHECK(!strcmp(out, "S") && marks == 3, "dit held 290 ms at 60: three dits, S (%s, %u)", out, marks);
    /* the squeeze let go during the dah: B adds the dit it remembered (R), A stops (A) */
    keyer_run(MO_IAMBIC_B, SQ_DIT, 1, SQ_DAH, 1, 1000, out, &marks);
    CHECK(!strcmp(out, "R"), "iambic B squeeze: R (%s)", out);
    keyer_run(MO_IAMBIC_A, SQ_DIT, 1, SQ_DAH, 1, 1000, out, &marks);
    CHECK(!strcmp(out, "A"), "iambic A squeeze: A (%s)", out);
    /* squeezed from the start, let go at 400 ms (inside the third element's gap... before it is chosen at 360: a dit
     * starts; at 480 nothing is held): dit dah dit, R */
    keyer_run(MO_IAMBIC_A, BOTH, 1, BOTH, 1, 1000, out, &marks);
    CHECK(!strcmp(out, "R") && marks == 3, "squeezed 400 ms: .-. (%s, %u)", out, marks);
    /* a dah, then a separate dit tap: N (-.) */
    keyer_run(MO_IAMBIC_B, A_DIT, 1, A_DAH, 1, 1000, out, &marks);
    CHECK(!strcmp(out, "N"), "dah then dit: N (%s)", out);
    mo_keyer_init(&k, MO_STRAIGHT, 60);
    CHECK(mo_keyer_tick(&k, 1, 0) == 1 && mo_keyer_tick(&k, 0, 1) == 1 && mo_keyer_tick(&k, 0, 0) == 0, "straight");
}

/* ------------------------------------------------------------------- learn --- */
static void test_learn(void)
{
    ln_state_t s;
    ln_round_t r;
    const ln_card_t *c;
    uint32_t i, rounds, opened = 0;
    ln_init(&s);
    CHECK(ln_valid(&s), "a new record is valid");
    CHECK(s.open == 5 && ln_new_count(&s) == 5 && ln_due_count(&s) == 0, "five open, all new");
    CHECK(ln_char(0) == 'K' && ln_char(1) == 'M' && ln_index('x') == 40 && ln_index('#') == -1, "Koch order");
    ln_build(&s, &r, 10, 0u, 7);
    CHECK(r.card[0].type == CARD_NEW && r.card[1].type == CARD_NEW && r.card[2].type == CARD_NEW,
          "three new characters shown first");
    CHECK(r.card[0].ch == 0 && r.card[1].ch == 1 && r.card[2].ch == 2, "K M U");
    CHECK(r.card[3].type != CARD_NEW, "then asked");
    for (i = 3; i < r.n; i++) {
        uint32_t j, k, hit = 0;
        c = &r.card[i];
        CHECK(c->dir == LN_REC && c->type == CARD_HEAR_CHOOSE, "new ones: hear and choose");
        for (j = 0; j < 4; j++) {
            hit += c->opt[j] == c->ch;
            for (k = j + 1; k < 4; k++)
                CHECK(c->opt[j] != c->opt[k], "four different choices");
            CHECK(c->opt[j] < s.open, "choices among the open ones");
        }
        CHECK(hit == 1 && c->opt[c->right] == c->ch, "the right one there once");
    }
    CHECK(r.n == 6, "a first round: 3 shown, 3 asked (%u)", r.n);
    /* all right */
    while ((c = ln_card(&r)) != 0)
        ln_answer(&s, &r, 1);
    CHECK(ln_finish(&s, &r) == 0 && s.round == 1 && s.streak == 1, "round counted, streak 1");
    CHECK(s.box[0][LN_REC] == 1 && s.last[0][LN_REC] == 0, "K in box 1");
    CHECK(ln_due(&s, 0, LN_REC) == 0, "box 1 not due one round later");
    CHECK(ln_due(&s, 0, LN_SEND) == 1, "sending K is due once it was recognised");
    CHECK(ln_new_count(&s) == 2, "R E still new");
    /* muted: nothing that needs sound; by sound the lamp's rhythm, by sight the drawn code */
    ln_build(&s, &r, 20, LN_MUTED, 9);
    for (i = 0; i < r.n; i++)
        CHECK(r.card[i].type == CARD_NEW || r.card[i].type == CARD_LIGHT_CHOOSE || r.card[i].type == CARD_SEE_KEY,
              "muted by sound: card %u type %u", i, r.card[i].type);
    ln_build(&s, &r, 20, LN_MUTED | LN_VISUAL, 9);
    for (i = 0; i < r.n; i++)
        CHECK(r.card[i].type == CARD_NEW || r.card[i].type == CARD_CODE_CHOOSE || r.card[i].type == CARD_SEE_KEY,
              "muted by sight: card %u type %u", i, r.card[i].type);
    /* by sound, never a drawn code; by sight, the low boxes see it drawn */
    ln_build(&s, &r, 20, 0, 13);
    for (i = 0; i < r.n; i++)
        CHECK(r.card[i].type != CARD_CODE_CHOOSE && r.card[i].type != CARD_LIGHT_CHOOSE, "by sound: card %u heard", i);
    ln_build(&s, &r, 20, LN_VISUAL, 13);
    for (i = 0; i < r.n; i++)
        CHECK(r.card[i].dir != LN_REC || r.card[i].type == CARD_NEW || r.card[i].type == CARD_CODE_CHOOSE,
              "by sight, box 0-1: card %u drawn", i);
    /* a miss: box 0, asked again at the end; right then does not move it up */
    ln_build(&s, &r, 20, 0, 11);
    while ((c = ln_card(&r)) != 0 && c->type == CARD_NEW)
        ln_answer(&s, &r, 1);
    {
        uint32_t n0 = r.n, ch = c->ch, dir = c->dir;
        ln_answer(&s, &r, 0);
        CHECK(r.n == n0 + 1 && r.card[n0].ch == ch && r.card[n0].again, "asked again at the end");
        CHECK(s.box[ch][dir] == 0, "back to box 0");
        while (r.at < n0)
            ln_answer(&s, &r, 1);
        ln_answer(&s, &r, 1);
        CHECK(s.box[ch][dir] == 0, "right after a miss: still box 0");
        CHECK(ln_finish(&s, &r) == 0 && s.streak == 2, "streak 2 (one miss in a round is over 80 %%)");
    }
    /* answering everything right opens the next groups, round by round */
    for (rounds = 0; rounds < 200 && s.open < LN_N; rounds++) {
        ln_build(&s, &r, 20, 0, rounds + 100);
        while (ln_card(&r))
            ln_answer(&s, &r, 1);
        opened += (uint32_t)ln_finish(&s, &r);
    }
    CHECK(s.open == LN_N, "all 41 open after %u rounds (open %u)", rounds, s.open);
    CHECK(opened == 8, "eight groups opened (%u)", opened);
    CHECK(ln_valid(&s), "still valid");
    CHECK(s.right == s.answered - 1, "one miss counted (%u of %u)", s.right, s.answered);
    for (i = 0; i < 400 && s.box[40][LN_REC] < LN_TOP; i++) {
        ln_build(&s, &r, 24, 0, i + 1000);
        while (ln_card(&r))
            ln_answer(&s, &r, 1);
        ln_finish(&s, &r);
    }
    CHECK(s.box[40][LN_REC] == LN_TOP && ln_level(&s, 40) >= 4, "X reaches the top box");
    s.box[3][LN_SEND] = 9;
    CHECK(!ln_valid(&s), "a box over 5 is not valid");
    /* a round left before any answer counts nothing */
    ln_init(&s);
    ln_build(&s, &r, 10, 0, 3);
    ln_answer(&s, &r, 1);
    CHECK(ln_finish(&s, &r) == 0 && s.round == 0, "only shown: not a round");
}

int main(void)
{
    test_table();
    test_timing();
    test_player_queue();
    test_decoder();
    test_keyer();
    test_learn();
    printf("%d checks, %d failed\n", checks, fails);
    return fails != 0;
}
