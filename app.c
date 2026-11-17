/* SPDX-License-Identifier: MIT */
/* Morse: see app.h. Part of fm1-base's unity build, after the platform (gfx.c, announce.c in scope).
 * The 1 kHz tick does what needs the millisecond: the key and the keyer, the two players, MIDI in and out, the
 * sidetone's gates; it leaves key-down / key-up events with their times in a ring. The main loop decodes them, runs
 * the pages and the flash cards, talks to the host and draws.
 * Protocol with the host: host/morse/PROTOCOL.md (the numbers below match host/morse/protocol.py). */
#include "fastmath.h"
#include "hostlink.c"
#include "ledfx.c"
#include "morse.c"
#include "learn.c"
#include "tone.c"

#define FW_MAJOR 0u
#define FW_MINOR 1u

/* ---------------------------------------------------------------- protocol --- */
#define MO_VER 1u
enum { T_HELLO_ACK = 0x01, T_TEXT, T_STOP };
enum { T_HELLO = 0x41, T_HEARTBEAT, T_CHAR };
/* Vail's MIDI keying (vail-adapter docs/MIDI_INTEGRATION_SPEC.md): channel 1, note 0 a timed key-down */
#define MIDI_KEY_ON (0x09u | 0x90u << 8 | 0x00u << 16 | 0x7Fu << 24)
#define MIDI_KEY_OFF (0x08u | 0x80u << 8 | 0x00u << 16 | 0x00u << 24)

/* the debounce keeps a key down ~8.8 ms after it opens and sees it ~1.6 ms after it closes: a straight key's marks
 * read ~7 ms long on the FM-1 (fm1-base bsp/boards/fm1/fm1_input.h); the host simulator has no debounce */
#ifdef PLAT_HOST
#define KEY_BIAS_MS 0u
#else
#define KEY_BIAS_MS 7u
#endif

/* ---------------------------------------------------------------- settings --- */
#define REC_MAGIC 0x4D4F5231u            /* "MOR1"; another magic, or a record that does not make sense: defaults */
#define REC_VER 1u
enum { LM_SOUND, LM_VISUAL };            /* how to learn: by sound (recommended), by sight */
typedef struct {
    uint8_t wpm;                         /* character speed 5..40 */
    uint8_t eff;                         /* effective speed (Farnsworth) 5..wpm */
    uint8_t keyer;                       /* MO_STRAIGHT, MO_IAMBIC_A, MO_IAMBIC_B */
    uint8_t swap;                        /* paddles: 0 left half dit, 1 left half dah */
    uint8_t mute;
    uint8_t cards;                       /* per round 10..30 */
    uint8_t learn;                       /* LM_* */
    uint8_t rsv;
    uint16_t pitch;                      /* Hz 300..1200 */
    uint16_t rsv2;
} set_t;
typedef struct {
    uint32_t magic;
    uint16_t ver, size;
    set_t set;
    ln_state_t ln;
} rec_t;
static rec_t rec;
#define SET (rec.set)

static void set_defaults(void)
{
    SET.wpm = 20;                        /* Koch: characters at full speed from the start, */
    SET.eff = 10;                        /* the gaps stretched (Farnsworth) */
    SET.keyer = MO_STRAIGHT;
    SET.swap = 0;
    SET.mute = 0;
    SET.cards = 20;
    SET.learn = LM_SOUND;
    SET.rsv = 0;
    SET.pitch = 600;
    SET.rsv2 = 0;
}

static int set_valid(const set_t *s)
{
    return s->wpm >= 5u && s->wpm <= 40u && s->eff >= 5u && s->eff <= s->wpm && s->keyer < MO_NKEYER && s->swap <= 1u
           && s->mute <= 1u && s->cards >= 10u && s->cards <= 30u && s->learn <= LM_VISUAL && s->pitch >= 300u
           && s->pitch <= 1200u;
}

static uint32_t saves;                   /* records written since boot */
static uint32_t dirty_at;                /* a setting changed at (0: saved) */

static void rec_save(void)
{
    rec.magic = REC_MAGIC;
    rec.ver = REC_VER;
    rec.size = (uint16_t)sizeof rec;
    if (plat_store_save(OBJ_MORSE, &rec, sizeof rec) == 0)
        saves++;
    dirty_at = 0;
}

/* ------------------------------------------------------------------- state --- */
enum { SRC_OWN, SRC_RX, SRC_N };         /* what this FM-1 sends; what it receives (MIDI in, the host's text) */
enum { PG_MENU, PG_PRACTICE, PG_KEY, PG_TYPE, PG_CHART, PG_SET, PG_N };
enum { PS_START, PS_CARD, PS_RESULT, PS_DONE };

/* the tick's: the key, the keyer, two players (ptx: this FM-1's own playing, prompts and typed letters; prx: the
 * host's text, as received), and what they leave for the main loop */
static mo_keyer_t keyer;
static mo_player_t ptx, prx;
static volatile uint8_t key_input;       /* the keys are the Morse key now */
static volatile uint8_t own_midi;        /* own keying goes out as MIDI (KEY) */
static volatile uint8_t ptx_midi;        /* ptx's playing goes out as MIDI (TYPE) */
static uint8_t own_down, ptx_down, rx_midi, rx_down, midi_down;
typedef struct {
    uint8_t src, down;
    uint32_t t;
} ev_t;
#define EV_N 64u
static ev_t ev[EV_N];
static volatile uint32_t ev_w, ev_r;
#define LEFT_KEYS ((1u << 13) - 1u)      /* F3..F4: the left paddle */
#ifdef PLAT_HOST
static mo_player_t host_hand;            /* "sendkeys": a scripted hand on a straight key */
#endif

/* the main loop's */
static hostlink_t link;
static struct {
    mo_dec_t dec;
    uint32_t t_down, t_up;
    uint8_t down, seen;
} dsrc[SRC_N];

#define TG_COLS 24u
#define TG_ROWS 5u
#define TG_N (TG_COLS * TG_ROWS)
static struct {
    char c[TG_N];
    uint8_t who[TG_N];
    uint32_t n;
} grid;                                  /* the KEY page's text, own and received in their colours */
#define TXT_N 96u
static char txt[3][TXT_N + 1];           /* own decoded, received decoded, typed (the simulator's expect_text) */

static struct {
    uint8_t page, back, sel;             /* sel: the menu's / the settings' cursor */
    uint8_t csel;                        /* the chart's */
    uint8_t redraw;                      /* bits: R_* */
    uint32_t keys, btn;
    char msg[2][24];
    uint32_t msg_until;
    uint8_t shift;                       /* TYPE: OCT- held */
    char line[TG_COLS * 2u + 1u];        /* TYPE: the line typed */
    char last;                           /* KEY: the last character read */
    uint8_t host_was, lamp_was, pat_was;
} ui;
enum { R_HEAD = 1, R_MAIN = 2, R_LAMP = 4, R_FOOT = 8, R_ALL = 15 };

static struct {
    ln_round_t r;
    uint8_t ps;                          /* PS_* */
    uint8_t pick;                        /* the choice under SELECT */
    uint8_t right;                       /* the last answer */
    uint8_t opened;                      /* the round opened a group */
    uint8_t answer_ok;                   /* keying is taken as the answer (the prompt has played) */
    char got;                            /* what was keyed */
    uint32_t ratio10;                    /* the keyed answer's dah:dit */
    uint32_t until;                      /* PS_RESULT: the next card at */
    uint8_t active;                      /* a round under way */
} pr;

static const uint8_t WHITE[16] = PLAT_WHITE_KEYS;
static const uint8_t BLACK[11] = PLAT_BLACK_KEYS;
static const uint8_t CHOICE_KEY[4] = {11, 12, 14, 16};   /* E4 F4 G4 A4: the four answers */
static const char TYPE_W[2][17] = {"ABCDEFGHIJKLMNOP", "1234567890.,?/=+"};
static const char TYPE_B[2][12] = {"QRSTUVWXYZ ", "-()'\":;@!&$"};

static void zero(void *p, uint32_t n)
{
    uint8_t *b = (uint8_t *)p;
    while (n--)
        *b++ = 0;
}

static void text_cat(char *d, const char *s)
{
    while (*d)
        d++;
    while (*s)
        *d++ = *s++;
    *d = 0;
}

static void int_text(int32_t v, char *b)
{
    char t[12];
    int n = 0;
    uint32_t u = (uint32_t)(v < 0 ? -v : v);
    do
        t[n++] = (char)('0' + u % 10u);
    while ((u /= 10u) && n < 11);
    if (v < 0)
        *b++ = '-';
    while (n)
        *b++ = t[--n];
    *b = 0;
}

static void int_cat(char *b, int32_t v)
{
    while (*b)
        b++;
    int_text(v, b);
}

static void note_name(uint32_t key, char *b)   /* key 0 is F3 */
{
    static const char *const N[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    uint32_t note = 53u + key;
    b[0] = 0;
    text_cat(b, N[note % 12u]);
    int_cat(b, (int32_t)(note / 12u) - 1);
}

void app_say(const char *a, const char *b)
{
    uint32_t i;
    char t[48];
    for (i = 0; i < 23u && a && a[i]; i++)
        ui.msg[0][i] = a[i];
    ui.msg[0][i] = 0;
    for (i = 0; i < 23u && b && b[i]; i++)
        ui.msg[1][i] = b[i];
    ui.msg[1][i] = 0;
    ui.msg_until = plat_ms() + 1500u;
    ui.redraw |= R_HEAD;
    if (ann_on()) {
        t[0] = 0;
        text_cat(t, ui.msg[0]);
        if (ui.msg[1][0]) {
            text_cat(t, " ");
            text_cat(t, ui.msg[1]);
        }
        ann_say(t, 0, ANN_NOW);
    }
}

static int visual(void) { return SET.learn == LM_VISUAL; }

/* ------------------------------------------------------------- the timing --- */
/* the speeds into the keyer, the players and the decoders (main loop; the tick locked out) */
static void timing_apply(void)
{
    uint32_t dit = mo_dit_ms(SET.wpm), gap = mo_gap_ms(SET.wpm, SET.eff), f = plat_lock(), s;
    keyer.mode = SET.keyer;
    keyer.dit = dit;
    ptx.dit = prx.dit = dit;
    ptx.gap = prx.gap = gap;
#ifdef PLAT_HOST
    host_hand.dit = dit;
    host_hand.gap = dit;                 /* a hand keys without Farnsworth */
#endif
    plat_unlock(f);
    for (s = 0; s < SRC_N; s++)
        mo_dec_init(&dsrc[s].dec, dit, s == SRC_OWN ? dit : gap);   /* a hand keys without Farnsworth */
    tone_pitch(TONE_TX, (float)SET.pitch);
    tone_pitch(TONE_RX, (float)SET.pitch * 1.26f);   /* received a major third up */
    tone_mute(SET.mute);
}

static void set_changed(void)
{
    timing_apply();
    dirty_at = plat_ms() | 1u;
    ui.redraw |= R_ALL;
}

static void play(mo_player_t *p, const char *s)   /* main loop: queue text on a player */
{
    uint32_t f = plat_lock();
    while (*s)
        mo_player_push(p, *s++);
    plat_unlock(f);
}

static void play_char(mo_player_t *p, char c)
{
    char s[2] = {c, 0};
    play(p, s);
}

static void play_stop(mo_player_t *p)
{
    uint32_t f = plat_lock();
    mo_player_stop(p);
    plat_unlock(f);
}

static int playing(const mo_player_t *p)
{
    uint32_t f = plat_lock();
    int b = mo_player_busy(p);
    plat_unlock(f);
    return b;
}

/* --------------------------------------------------------------- the tick --- */
static void ev_push(uint32_t src, int down, uint32_t t)
{
    uint32_t next = (ev_w + 1u) % EV_N;
    if (next == ev_r)
        return;                          /* the main loop is behind: drop (it re-syncs on the next edge) */
    ev[ev_w].src = (uint8_t)src;
    ev[ev_w].down = (uint8_t)down;
    ev[ev_w].t = t;
    ev_w = next;
}

static void midi_key(void)               /* own keying and ptx's playing, as one key on MIDI */
{
    int down = (own_down && own_midi) || (ptx_down && ptx_midi);
    if (down == midi_down)
        return;
    midi_down = (uint8_t)down;
    plat_midi_send(down ? MIDI_KEY_ON : MIDI_KEY_OFF, TO_USB);
}

void app_tick(void)
{
    uint32_t now = plat_ms(), keys = plat_keys(), pkt;
    int dit = 0, dah = 0, own, t, rx, port;
    if (key_input && keys) {
        if (keyer.mode == MO_STRAIGHT)
            dit = 1;
        else {
            int lo = (keys & LEFT_KEYS) != 0, hi = (keys & ~LEFT_KEYS) != 0;
            dit = SET.swap ? hi : lo;
            dah = SET.swap ? lo : hi;
        }
    }
    own = mo_keyer_tick(&keyer, dit, dah);
#ifdef PLAT_HOST
    if (mo_player_busy(&host_hand))
        own = mo_player_tick(&host_hand);
#endif
    if (own != own_down) {
        own_down = (uint8_t)own;
        ev_push(SRC_OWN, own, now);
    }
    t = mo_player_tick(&ptx);
    ptx_down = (uint8_t)t;
    midi_key();
    tone_gate(TONE_TX, own || t);
    /* MIDI in: a key-down on channel 1, any note (Vail sends 0, a DAW whatever it plays); velocity 0 is a release */
    while (plat_midi_in(&pkt, &port)) {
        uint32_t st = pkt >> 8 & 0xFFu, vel = pkt >> 24 & 0x7Fu;
        if (st == 0x90u && vel)
            rx_midi = 1;
        else if (st == 0x80u || st == 0x90u)
            rx_midi = 0;
    }
    rx = rx_midi || mo_player_tick(&prx);
    if (rx != rx_down) {
        rx_down = (uint8_t)rx;
        ev_push(SRC_RX, rx, now);
    }
    tone_gate(TONE_RX, rx);
}

/* ------------------------------------------------------------- the reading --- */
static void txt_add(uint32_t which, char c)
{
    char *t = txt[which];
    uint32_t n = 0, i;
    while (t[n])
        n++;
    if (n >= TXT_N) {
        for (i = 0; i + 1u < TXT_N; i++)
            t[i] = t[i + 1u];
        n = TXT_N - 1u;
    }
    t[n] = c;
    t[n + 1u] = 0;
}

static void grid_add(char c, uint32_t who)
{
    uint32_t i;
    if (c == ' ' && (grid.n % TG_COLS == 0u || (grid.n && grid.c[grid.n - 1u] == ' ')))
        return;
    if (grid.n >= TG_N) {
        for (i = 0; i < TG_N - TG_COLS; i++) {
            grid.c[i] = grid.c[i + TG_COLS];
            grid.who[i] = grid.who[i + TG_COLS];
        }
        grid.n -= TG_COLS;
    }
    grid.c[grid.n] = c;
    grid.who[grid.n] = (uint8_t)who;
    grid.n++;
}

static void send_char(char c)            /* to the host, which may type it on the computer */
{
    uint8_t p[1];
    if (!hl_host(&link, plat_ms()) || c < 0x20 || c > 0x7E)
        return;
    p[0] = (uint8_t)c;
    hl_send(&link, T_CHAR, p, 1);
}

static void practice_keyed(char c);

static void emit(uint32_t src, char c)
{
    if (src == SRC_RX) {
        txt_add(1, c);
        if (ui.page == PG_KEY) {
            grid_add(c, SRC_RX);
            ui.redraw |= R_MAIN;
        }
        return;
    }
    txt_add(0, c);
    if (ui.page == PG_KEY) {
        grid_add(c, SRC_OWN);
        if (c != ' ')
            ui.last = c;
        send_char(c);
        ui.redraw |= R_MAIN;
    } else if (ui.page == PG_PRACTICE && c != ' ')
        practice_keyed(c);
}

static void read_events(void)            /* the tick's events into the decoders */
{
    uint32_t now = plat_ms(), s, f;
    char out[2];
    for (;;) {
        ev_t e;
        uint32_t n = 0, i;
        f = plat_lock();
        if (ev_r == ev_w) {
            plat_unlock(f);
            break;
        }
        e = ev[ev_r];
        ev_r = (ev_r + 1u) % EV_N;
        plat_unlock(f);
        if (e.src >= SRC_N)
            continue;
        {
            uint32_t bias = e.src == SRC_OWN && SET.keyer == MO_STRAIGHT ? KEY_BIAS_MS : 0u;
            if (e.down) {
                if (dsrc[e.src].seen && !dsrc[e.src].down)
                    n = mo_dec_gap(&dsrc[e.src].dec, e.t - dsrc[e.src].t_up + bias, out);
                dsrc[e.src].t_down = e.t;
            } else if (dsrc[e.src].down) {
                uint32_t mark = e.t - dsrc[e.src].t_down;
                mo_dec_mark(&dsrc[e.src].dec, mark > bias ? mark - bias : mark);
                dsrc[e.src].t_up = e.t;
            }
        }
        dsrc[e.src].down = e.down;
        dsrc[e.src].seen = 1;
        for (i = 0; i < n; i++)
            emit(e.src, out[i]);
        if (e.src == SRC_OWN && visual())   /* the code so far is drawn */
            ui.redraw |= R_MAIN;
    }
    for (s = 0; s < SRC_N; s++)          /* silence ends a character, then a word */
        if (dsrc[s].seen && !dsrc[s].down) {
            char c = mo_dec_idle(&dsrc[s].dec, now - dsrc[s].t_up);
            if (c)
                emit(s, c);
        }
}

/* ---------------------------------------------------------------- the host --- */
static int host_frame(const uint8_t *p, uint32_t n, int transport)
{
    uint8_t t;
    uint32_t len, i, now = plat_ms();
    const uint8_t *d = hl_match(&link, p, n, &t, &len);
    if (!d)
        return 0;
    hl_seen(&link, now);
    switch (t) {
    case T_HELLO_ACK:
        hl_acked(&link, now, transport);
        ui.redraw |= R_HEAD;
        break;
    case T_TEXT:                         /* the computer's text, played as received */
        for (i = 0; i < len; i++) {
            char c = (char)d[i];
            if (c >= 'a' && c <= 'z')
                c = (char)(c - 32);
            if (c == ' ' || mo_code(c))
                play_char(&prx, c);
        }
        break;
    case T_STOP:
        play_stop(&prx);
        break;
    default:
        break;
    }
    return 1;
}

int app_sysex(const uint8_t *p, uint32_t n) { return host_frame(p, n, HL_USB); }

/* ------------------------------------------------------------- the pages --- */
static void inputs_for_page(void)        /* who the keys and MIDI belong to on this page */
{
    key_input = ui.page == PG_KEY || (ui.page == PG_PRACTICE && pr.ps == PS_CARD && pr.answer_ok);
    own_midi = ui.page == PG_KEY;
    ptx_midi = ui.page == PG_TYPE;
}

static void practice_leave(void);

static void set_page(uint32_t p)
{
    if (ui.page == PG_PRACTICE && p != PG_PRACTICE)
        practice_leave();
    if (p != PG_MENU)
        ui.back = (uint8_t)p;
    ui.page = (uint8_t)(p % PG_N);
    ui.sel = (uint8_t)(ui.page == PG_MENU && ui.back ? ui.back - 1u : 0u);
    inputs_for_page();
    ui.redraw |= R_ALL;
}

/* -------------------------------------------------------------- practice --- */
static int card_key(const ln_card_t *c) { return c->type == CARD_HEAR_KEY || c->type == CARD_SEE_KEY; }
static int card_sounds(const ln_card_t *c)   /* its prompt is played (or flashed, muted) */
{
    return c->type == CARD_NEW || c->type == CARD_HEAR_CHOOSE || c->type == CARD_HEAR_KEY
           || c->type == CARD_LIGHT_CHOOSE;
}

static void card_present(void)
{
    const ln_card_t *c = ln_card(&pr.r);
    char ch;
    pr.answer_ok = 0;
    pr.got = 0;
    pr.pick = 0;
    if (!c) {
        pr.opened = (uint8_t)ln_finish(&rec.ln, &pr.r);
        pr.active = 0;
        pr.ps = PS_DONE;
        rec_save();
        if (pr.opened)
            app_say("NEW LETTERS", "");
        inputs_for_page();
        ui.redraw |= R_ALL;
        return;
    }
    ch = ln_char(c->ch);
    pr.ps = PS_CARD;
    if (c->type == CARD_NEW) {
        char s[4] = {ch, ' ', ch, 0};
        play(&ptx, s);
    } else if (card_sounds(c))
        play_char(&ptx, ch);
    mo_dec_init(&dsrc[SRC_OWN].dec, mo_dit_ms(SET.wpm), mo_dit_ms(SET.wpm));
    inputs_for_page();
    ui.redraw |= R_ALL;
}

static void practice_start(void)
{
    uint32_t flags = (SET.mute ? LN_MUTED : 0u) | (visual() ? LN_VISUAL : 0u);
    ln_build(&rec.ln, &pr.r, SET.cards, flags, plat_ms() * 2654435761u + rec.ln.round);
    pr.active = 1;
    pr.opened = 0;
    card_present();
}

static void practice_answer(int right)
{
    const ln_card_t *c = ln_card(&pr.r);
    if (!c || pr.ps != PS_CARD)
        return;
    if (c->type == CARD_NEW) {           /* only shown: on to the next */
        ln_answer(&rec.ln, &pr.r, 1);
        play_stop(&ptx);
        card_present();
        return;
    }
    pr.right = (uint8_t)(right != 0);
    pr.ratio10 = card_key(c) ? mo_dec_ratio10(&dsrc[SRC_OWN].dec) : 0u;
    ln_answer(&rec.ln, &pr.r, right);
    pr.ps = PS_RESULT;
    pr.answer_ok = 0;
    play_stop(&ptx);
    if (!right)
        play_char(&ptx, ln_char(c->ch));   /* how it should have gone */
    pr.until = plat_ms() + (right ? 900u : 1800u);
    inputs_for_page();
    ui.redraw |= R_ALL;
}

static void practice_keyed(char c)       /* the own decoder read a character during a card */
{
    const ln_card_t *k = ln_card(&pr.r);
    if (pr.ps != PS_CARD || !k || !card_key(k) || !pr.answer_ok)
        return;
    pr.got = c;
    practice_answer(c == ln_char(k->ch));
}

static void practice_choose(uint32_t i)
{
    const ln_card_t *c = ln_card(&pr.r);
    if (pr.ps != PS_CARD || !c || !ln_choose(c->type) || i > 3u)
        return;
    pr.got = ln_char(c->opt[i]);
    practice_answer(i == c->right);
}

static void practice_leave(void)
{
    if (pr.active) {
        pr.active = 0;
        ln_finish(&rec.ln, &pr.r);
        rec_save();
    }
    play_stop(&ptx);
    pr.ps = PS_START;
}

static void practice_step(uint32_t now)
{
    const ln_card_t *c = ln_card(&pr.r);
    if (pr.ps == PS_CARD && c && card_key(c) && !pr.answer_ok && !playing(&ptx)) {
        pr.answer_ok = 1;                /* the prompt has played: now keying answers */
        mo_dec_init(&dsrc[SRC_OWN].dec, mo_dit_ms(SET.wpm), mo_dit_ms(SET.wpm));
        inputs_for_page();
        ui.redraw |= R_FOOT;
    }
    if (pr.ps == PS_RESULT && now >= pr.until && !playing(&ptx))
        card_present();
}

/* ------------------------------------------------------------------ input --- */
static void settings_step(int32_t d);

static void type_key(uint32_t k)         /* TYPE: a key is a letter */
{
    uint32_t i, n;
    char c = 0;
    for (i = 0; i < 16u; i++)
        if (WHITE[i] == k)
            c = TYPE_W[ui.shift][i];
    for (i = 0; i < 11u; i++)
        if (BLACK[i] == k)
            c = TYPE_B[ui.shift][i];
    if (!c)
        return;
    n = 0;
    while (ui.line[n])
        n++;
    if (n >= sizeof ui.line - 1u) {
        for (i = 0; i + 1u < n; i++)
            ui.line[i] = ui.line[i + 1u];
        n--;
    }
    ui.line[n] = c;
    ui.line[n + 1u] = 0;
    play_char(&ptx, c);
    txt_add(2, c);
    send_char(c);
    ui.redraw |= R_MAIN;
}

static void page_key(uint32_t k)         /* a piano key went down */
{
    uint32_t i;
    if (ui.page == PG_TYPE)
        type_key(k);
    else if (ui.page == PG_PRACTICE && pr.ps == PS_CARD) {
        const ln_card_t *c = ln_card(&pr.r);
        if (c && c->type == CARD_NEW)
            practice_answer(1);
        else if (c && ln_choose(c->type))
            for (i = 0; i < 4u; i++)
                if (CHOICE_KEY[i] == k)
                    practice_choose(i);
    } else if (ui.page == PG_PRACTICE && pr.ps == PS_RESULT)
        pr.until = 0;                    /* on without waiting */
}

static void page_play(void)              /* PLAY: start, again */
{
    const ln_card_t *c;
    switch (ui.page) {
    case PG_PRACTICE:
        if (pr.ps == PS_START || pr.ps == PS_DONE)
            practice_start();
        else if (pr.ps == PS_CARD && (c = ln_card(&pr.r)) != 0 && card_sounds(c) && !playing(&ptx)) {
            play_char(&ptx, ln_char(c->ch));
            if (card_key(c))
                pr.answer_ok = 0;        /* taken again once it has played */
            inputs_for_page();
        }
        break;
    case PG_TYPE:
        play(&ptx, " ");
        play(&ptx, ui.line);
        break;
    case PG_CHART:
        play_char(&ptx, ln_char(ui.csel));
        break;
    case PG_MENU:
        set_page(ui.sel + 1u);
        break;
    default:
        break;
    }
}

static void page_sel(void)               /* SEL: choose, skip */
{
    const ln_card_t *c;
    switch (ui.page) {
    case PG_MENU:
        set_page(ui.sel + 1u);
        break;
    case PG_PRACTICE:
        if (pr.ps == PS_CARD && (c = ln_card(&pr.r)) != 0) {
            if (ln_choose(c->type))
                practice_choose(pr.pick);
            else
                practice_answer(c->type == CARD_NEW);   /* a card to key: don't know */
        } else if (pr.ps == PS_RESULT)
            pr.until = 0;
        else
            practice_start();
        break;
    case PG_CHART:
        play_char(&ptx, ln_char(ui.csel));
        break;
    case PG_SET:
        settings_step(1);
        break;
    default:
        break;
    }
}

static void page_select(int32_t e)       /* SELECT turned */
{
    int32_t d = e > 0 ? 1 : -1;
    const ln_card_t *c;
    switch (ui.page) {
    case PG_MENU:
        ui.sel = (uint8_t)((ui.sel + PG_N - 1u + (uint32_t)(d + (int32_t)(PG_N - 1u))) % (PG_N - 1u));
        break;
    case PG_PRACTICE:
        if (pr.ps == PS_CARD && (c = ln_card(&pr.r)) != 0 && ln_choose(c->type))
            pr.pick = (uint8_t)((pr.pick + 4u + (uint32_t)(d + 4)) % 4u);
        break;
    case PG_CHART:
        ui.csel = (uint8_t)((ui.csel + LN_N + (uint32_t)(d + (int32_t)LN_N)) % LN_N);
        break;
    case PG_SET:
        ui.sel = (uint8_t)((ui.sel + 8u + (uint32_t)(d + 8)) % 8u);
        break;
    default:
        break;
    }
    ui.redraw |= R_MAIN | R_FOOT;
}

static void speed(int32_t d)
{
    int32_t w = clamp((int32_t)SET.wpm + d, 5, 40);
    if (w == SET.wpm)
        return;
    if (SET.eff == SET.wpm || SET.eff > w)   /* no Farnsworth stays none; stretched stays at most the speed */
        SET.eff = (uint8_t)w;
    SET.wpm = (uint8_t)w;
    set_changed();
}

static void spacing(int32_t d)
{
    int32_t e = clamp((int32_t)SET.eff + d, 5, SET.wpm);
    if (e == SET.eff)
        return;
    SET.eff = (uint8_t)e;
    set_changed();
}

static void pitch(int32_t d)
{
    int32_t p = clamp((int32_t)SET.pitch + d * 10, 300, 1200);
    if (p == SET.pitch)
        return;
    SET.pitch = (uint16_t)p;
    set_changed();
}

static void mute_toggle(void)
{
    SET.mute = (uint8_t)!SET.mute;
    set_changed();
    app_say("SOUND", SET.mute ? "OFF: WATCH THE LIGHTS" : "ON");
}

/* the settings page: SELECT moves, OCT- / OCT+ (or SEL, K4) change */
static const char *const SET_NAME[8] = {"SPEED", "SPACING", "PITCH", "KEYER", "PADDLES", "SOUND", "CARDS", "LEARN BY"};
static void settings_step(int32_t d)
{
    switch (ui.sel) {
    case 0: speed(d); break;
    case 1: spacing(d); break;
    case 2: pitch(d); break;
    case 3:
        SET.keyer = (uint8_t)((SET.keyer + MO_NKEYER + (uint32_t)(d + MO_NKEYER)) % MO_NKEYER);
        set_changed();
        break;
    case 4:
        SET.swap = (uint8_t)!SET.swap;
        set_changed();
        break;
    case 5: mute_toggle(); break;
    case 6:
        SET.cards = (uint8_t)clamp((int32_t)SET.cards + d * 5, 10, 30);
        set_changed();
        break;
    case 7:
        SET.learn = (uint8_t)!SET.learn;
        set_changed();
        if (visual())
            app_say("TIP", "SOUND FIRST IS FASTER LATER");
        break;
    default:
        break;
    }
}

static void input(void)
{
    uint32_t keys = plat_keys(), btn = plat_buttons(), k;
    int32_t e;
    for (k = 0; k < NKEYS; k++)
        if ((keys >> k & 1u) && !(ui.keys >> k & 1u))
            page_key(k);
    for (k = 0; k < NB; k++) {
        if (!(btn >> k & 1u) || (ui.btn >> k & 1u))
            continue;
        switch (k) {
        case B_HOME: set_page(ui.page == PG_MENU ? ui.back : PG_MENU); break;
        case B_FX: mute_toggle(); break;
        case B_PLAY: page_play(); break;
        case B_SEL: page_sel(); break;
        case B_OCTDN:
            if (ui.page == PG_KEY) {
                grid.n = 0;
                ui.last = 0;
                ui.redraw |= R_MAIN;
            } else if (ui.page == PG_SET)
                settings_step(-1);
            break;
        case B_OCTUP:
            if (ui.page == PG_TYPE) {
                uint32_t n = 0;
                while (ui.line[n])
                    n++;
                if (n)
                    ui.line[n - 1u] = 0;
                ui.redraw |= R_MAIN;
            } else if (ui.page == PG_SET)
                settings_step(1);
            break;
        default: break;
        }
    }
    if (ui.page == PG_TYPE && (uint8_t)(btn >> B_OCTDN & 1u) != ui.shift) {
        ui.shift = (uint8_t)(btn >> B_OCTDN & 1u);
        ui.redraw |= R_MAIN | R_FOOT;
    }
    if ((e = plat_enc(EN_SELECT)) != 0)
        page_select(e);
    if ((e = plat_enc(EN_ALGO)) != 0)
        set_page((uint32_t)((int32_t)ui.page + PG_N + (e > 0 ? 1 : -1)) % PG_N);
    if ((e = plat_enc(EN_K1)) != 0)
        speed(e > 0 ? 1 : -1);
    if ((e = plat_enc(EN_K2)) != 0)
        spacing(e > 0 ? 1 : -1);
    if ((e = plat_enc(EN_K3)) != 0)
        pitch(e > 0 ? 1 : -1);
    if ((e = plat_enc(EN_K4)) != 0 && ui.page == PG_SET)
        settings_step(e > 0 ? 1 : -1);
    (void)plat_enc(EN_PRESET);
    if (btn != ui.btn)
        ui.redraw |= R_FOOT;
    ui.keys = keys;
    ui.btn = btn;
}

void app_input_only(void)
{
    input();
    read_events();
}

/* ----------------------------------------------------------------- lights --- */
static void lights(uint32_t now)
{
    ledfx_t kb = {0, 0, 0}, bb = {0, 0, 0};
    uint32_t k, i;
    int tx = tone_gated(TONE_TX), rx = tone_gated(TONE_RX);
    const ln_card_t *c = ln_card(&pr.r);
    for (k = 0; k < NB; k++)
        if (k != B_PLAY)
            ledfx_set(&bb, k, FX_GLOW, now);
    if (SET.mute)
        ledfx_set(&bb, B_FX, FX_LIT, now);
    if (ui.page == PG_PRACTICE && pr.ps == PS_CARD && c && ln_choose(c->type)) {
        for (i = 0; i < 4u; i++)
            ledfx_set(&kb, CHOICE_KEY[i], i == pr.pick ? FX_BLINK : FX_LIT, now);
    } else
        for (k = 0; k < NKEYS; k++)      /* the lamp: every key lights with the keying (the way to read it muted) */
            ledfx_set(&kb, k, tx ? FX_LIT : rx ? FX_MID : FX_GLOW, now);
    if (hl_host(&link, now))
        bb.lit |= LED_PLAY_GREEN;
    ledfx_show(&bb, &kb);
}

/* ----------------------------------------------------------------- screen --- */
#define C_OWN C_WHITE
#define C_RX RGB(255, 176, 0)
#define C_OK RGB(80, 220, 120)
#define C_BAD RGB(240, 70, 60)
#define C_BLUE RGB(70, 140, 255)

static uint16_t level_rgb(uint32_t lv)   /* the chart's colours, box 0 .. 5 */
{
    static const uint16_t L[6] = {RGB(70, 40, 40), RGB(140, 60, 40), RGB(190, 120, 30), RGB(170, 170, 40),
                                  RGB(90, 180, 70), RGB(60, 210, 120)};
    return L[lv > 5u ? 5u : lv];
}

static void draw_code(uint8_t code, int32_t cx, int32_t y, int32_t u, uint16_t c)   /* centred on cx */
{
    uint32_t n = mo_len(code), i;
    int32_t x = cx - (int32_t)mo_units(code) * u / 2;
    for (i = 0; i < n; i++) {
        int32_t w = mo_elem(code, i) ? 3 * u : u;
        cv_rect(x, y, w, u, c);
        x += w + u;
    }
}

static void text_c(int32_t cx, int32_t y, const felucca_font_t *f, const char *s, uint16_t c)   /* centred */
{
    cv_text(cx - text_w(f, s) / 2, y, f, s, c);
}

static void draw_header(uint32_t now)
{
    static const char *const PAGE[PG_N] = {"MENU", "PRACTICE", "KEY", "TYPE", "CHART", "SETTINGS"};
    char t[24];
    int32_t x;
    cv_begin(240, 32, C_BLACK);
    if (now < ui.msg_until) {
        x = cv_text(8, 7, &FONT_B, ui.msg[0], C_WHITE);
        cv_text(x + 8, 9, &FONT_XS, ui.msg[1], C_RX);
    } else {
        cv_text(8, 7, &FONT_B, PAGE[ui.page], C_WHITE);
        t[0] = 0;
        int_text(SET.wpm, t);
        if (SET.eff < SET.wpm) {
            text_cat(t, "/");
            int_cat(t, SET.eff);
        }
        text_cat(t, " WPM");
        x = 232 - text_w(&FONT_XS, t);
        cv_text(x, 10, &FONT_XS, t, C_GRAY);
        if (SET.mute) {
            x -= 8 + text_w(&FONT_XS, "MUTE");
            cv_text(x, 10, &FONT_XS, "MUTE", C_RX);
        }
        if (hl_host(&link, now)) {
            x -= 8 + text_w(&FONT_XS, "PC");
            cv_text(x, 10, &FONT_XS, "PC", C_BLUE);
        }
    }
    cv_rect(0, 31, 240, 1, C_LINE);
    cv_blit(0, 0);
}

/* the main area, y 32..207 (176 high), in strips no taller than the canvas */
static void draw_lamp(int32_t y)         /* the keying as a light: white sending, amber receiving */
{
    int tx = tone_gated(TONE_TX), rx = tone_gated(TONE_RX);
    cv_begin(240, 10, C_BLACK);
    cv_rect(20, 2, 200, 6, tx ? C_OWN : rx ? C_RX : C_LINE);
    cv_blit(0, y);
}

static void key_page(void)
{
    char t[32];
    uint32_t r, i;
    uint8_t pend = mo_dec_pending(&dsrc[SRC_OWN].dec);
    cv_begin(240, 56, C_BLACK);
    if (visual() && pend > 1u)
        draw_code(pend, 90, 22, 8, C_OWN);
    if (ui.last) {
        t[0] = ui.last, t[1] = 0;
        text_c(200, 6, &FONT_L, t, C_OWN);
    }
    if (dsrc[SRC_OWN].dec.marks >= 4u) {  /* how it is going: the speed it reads, the dah:dit (3.0 the book) */
        uint32_t q = mo_dec_ratio10(&dsrc[SRC_OWN].dec);
        t[0] = 0;
        text_cat(t, "YOU ");
        int_cat(t, (int32_t)mo_dec_wpm(&dsrc[SRC_OWN].dec));
        text_cat(t, " WPM");
        if (q) {
            text_cat(t, "  DAH:DIT ");
            int_cat(t, (int32_t)(q / 10u));
            text_cat(t, ".");
            int_cat(t, (int32_t)(q % 10u));
        }
        cv_text(8, 40, &FONT_XS, t, C_GRAY);
    }
    cv_blit(0, 32);
    draw_lamp(88);
    cv_begin(240, 110, C_BLACK);
    for (r = 0; r < TG_ROWS; r++)
        for (i = 0; i < TG_COLS; i++) {
            uint32_t k = r * TG_COLS + i;
            if (k < grid.n) {
                t[0] = grid.c[k], t[1] = 0;
                cv_text(4 + (int32_t)i * 10, 2 + (int32_t)r * 22, &FONT_S, t, grid.who[k] == SRC_RX ? C_RX : C_OWN);
            }
        }
    if (!grid.n)
        cv_text(8, 40, &FONT_XS, keyer.mode == MO_STRAIGHT ? "any key is the key" : "left half dit, right half dah",
                C_DIM);
    cv_blit(0, 98);
}

static void type_page(void)
{
    char t[2] = {0, 0};
    uint32_t i, n = 0;
    char cur = ptx.cur;
    while (ui.line[n])
        n++;
    cv_begin(240, 80, C_BLACK);
    for (i = 0; i < n; i++) {
        t[0] = ui.line[i] == ' ' ? '_' : ui.line[i];
        cv_text(4 + (int32_t)(i % TG_COLS) * 10, 4 + (int32_t)(i / TG_COLS) * 22, &FONT_S, t,
                ui.line[i] == ' ' ? C_DIM : C_OWN);
    }
    if (cur && visual())
        draw_code(mo_code(cur), 120, 62, 6, C_RX);
    cv_blit(0, 32);
    draw_lamp(112);
    /* the keyboard: 16 white keys 15 px each, the black ones over their gaps */
    cv_begin(240, 86, C_BLACK);
    for (i = 0; i < 16u; i++) {
        char c = TYPE_W[ui.shift][i];
        cv_rect((int32_t)i * 15, 0, 14, 84, c == cur ? C_RX : RGB(200, 200, 206));
        t[0] = c;
        text_c((int32_t)i * 15 + 7, 64, &FONT_XS, t, C_BLACK);
    }
    for (i = 0; i < 11u; i++) {
        uint32_t w = 0, k = BLACK[i];
        char c = TYPE_B[ui.shift][i];
        while (w < 16u && WHITE[w] < k)
            w++;
        cv_rect((int32_t)w * 15 - 6, 0, 12, 46, c == cur ? C_RX : RGB(40, 40, 46));
        t[0] = c == ' ' ? '_' : c;
        text_c((int32_t)w * 15, 26, &FONT_XS, t, C_WHITE);
    }
    cv_blit(0, 122);
}

static void choices(int32_t y, const ln_card_t *c, int32_t oy)
{
    char t[8];
    uint32_t i;
    for (i = 0; i < 4u; i++) {
        int32_t x = 4 + (int32_t)i * 59;
        cv_rect(x, y - oy, 55, 44, i == pr.pick ? C_LINE : RGB(24, 24, 28));
        t[0] = ln_char(c->opt[i]), t[1] = 0;
        text_c(x + 27, y + 6 - oy, &FONT_M, t, C_WHITE);
        note_name(CHOICE_KEY[i], t);
        text_c(x + 27, y + 46 - oy, &FONT_XS, t, C_DIM);
    }
}

static void practice_at(int32_t oy)      /* drawn twice, each half with the rows above it taken off */
{
    char t[40];
    const ln_card_t *c = ln_card(&pr.r);
    const ln_state_t *s = &rec.ln;
    if (pr.ps == PS_START || pr.ps == PS_DONE) {
        int32_t y = 8;
        if (pr.ps == PS_DONE) {
            t[0] = 0;
            text_cat(t, "ROUND ");
            int_cat(t, s->round);
            text_cat(t, " DONE");
            text_c(120, y - oy, &FONT_B, t, C_WHITE);
            t[0] = 0;
            int_cat(t, pr.r.ok);
            text_cat(t, " RIGHT  ");
            int_cat(t, pr.r.miss);
            text_cat(t, " MISSED  +");
            int_cat(t, pr.r.up);
            text_c(120, y + 26 - oy, &FONT_XS, t, C_GRAY);
            y += 48;
            if (pr.opened) {
                uint32_t i;
                t[0] = 0;
                text_cat(t, "NEW: ");
                for (i = s->open - LN_GROUP; i < s->open; i++) {
                    char ch[3] = {ln_char(i), ' ', 0};
                    text_cat(t, ch);
                }
                text_c(120, y - oy, &FONT_B, t, C_OK);
                y += 26;
            }
        } else {
            t[0] = 0;
            text_cat(t, "ROUND ");
            int_cat(t, s->round + 1u);
            text_c(120, y - oy, &FONT_M, t, C_WHITE);
            y += 40;
            t[0] = 0;
            int_cat(t, (int32_t)ln_due_count(s));
            text_cat(t, " DUE   ");
            int_cat(t, (int32_t)ln_new_count(s));
            text_cat(t, " NEW   ");
            int_cat(t, s->open);
            text_cat(t, " OPEN");
            text_c(120, y - oy, &FONT_XS, t, C_GRAY);
            y += 26;
        }
        t[0] = 0;
        text_cat(t, "STREAK ");
        int_cat(t, s->streak);
        text_c(120, y - oy, &FONT_XS, t, s->streak ? C_OK : C_DIM);
        text_c(120, y + 40 - oy, &FONT_B, "PLAY: START", C_RX);
        text_c(120, y + 64 - oy, &FONT_XS, visual() ? "learning by sight" : "learning by sound", C_DIM);
        return;
    }
    if (!c && pr.ps != PS_RESULT)
        return;
    t[0] = 0;
    int_cat(t, pr.r.at + (pr.ps == PS_RESULT ? 0u : 1u));
    text_cat(t, " / ");
    int_cat(t, pr.r.n);
    cv_text(8, 2 - oy, &FONT_XS, t, C_DIM);
    if (pr.ps == PS_RESULT) {
        const ln_card_t *a = &pr.r.card[pr.r.at - 1u];
        char ch = ln_char(a->ch);
        text_c(120, 18 - oy, &FONT_L, pr.right ? "RIGHT" : "NO", pr.right ? C_OK : C_BAD);
        t[0] = ch, t[1] = 0;
        text_c(120, 64 - oy, &FONT_L, t, C_WHITE);
        if (visual())
            draw_code(mo_code(ch), 120, 108 - oy, 8, C_GRAY);
        if (!pr.right && pr.got) {
            t[0] = 0;
            text_cat(t, "YOU: ");
            t[5] = pr.got, t[6] = 0;
            text_c(120, 126 - oy, &FONT_XS, t, C_BAD);
        }
        if (pr.ratio10 && card_key(a)) {
            t[0] = 0;
            text_cat(t, "DAH:DIT ");
            int_cat(t, (int32_t)(pr.ratio10 / 10u));
            text_cat(t, ".");
            int_cat(t, (int32_t)(pr.ratio10 % 10u));
            text_cat(t, pr.ratio10 < 25u ? "  dahs short" : pr.ratio10 > 40u ? "  dahs long" : "");
            text_c(120, 146 - oy, &FONT_XS, t, C_GRAY);
        }
        return;
    }
    {
        static const char *const ASK[CARD_NTYPES] = {"NEW LETTER", "WHICH ONE?", "KEY WHAT YOU HEAR", "KEY IT",
                                                    "WHICH ONE?", "WHICH ONE?"};
        char ch = ln_char(c->ch);
        text_c(120, 18 - oy, &FONT_B, ASK[c->type], c->type == CARD_NEW ? C_OK : C_GRAY);
        t[0] = ch, t[1] = 0;
        switch (c->type) {
        case CARD_NEW:
            text_c(120, 50 - oy, &FONT_L, t, C_WHITE);
            if (visual())
                draw_code(mo_code(ch), 120, 100 - oy, 8, C_GRAY);
            text_c(120, 128 - oy, &FONT_XS, SET.mute ? "watch it, then a key" : "listen, then any key", C_DIM);
            break;
        case CARD_HEAR_CHOOSE:
        case CARD_LIGHT_CHOOSE:
            text_c(120, 48 - oy, &FONT_M, c->type == CARD_LIGHT_CHOOSE ? "( lamp )" : "?", C_DIM);
            choices(92, c, oy);
            break;
        case CARD_CODE_CHOOSE:
            draw_code(mo_code(ch), 120, 58 - oy, 8, C_WHITE);
            choices(92, c, oy);
            break;
        case CARD_HEAR_KEY:
            text_c(120, 54 - oy, &FONT_L, "?", C_DIM);
            break;
        case CARD_SEE_KEY:
            text_c(120, 50 - oy, &FONT_L, t, C_WHITE);
            break;
        default:
            break;
        }
        if (card_key(c)) {
            uint8_t pend = mo_dec_pending(&dsrc[SRC_OWN].dec);
            if (visual() && pend > 1u)
                draw_code(pend, 120, 112 - oy, 8, C_OWN);
            text_c(120, 136 - oy, &FONT_XS, pr.answer_ok ? "your turn" : "listen", pr.answer_ok ? C_OK : C_DIM);
        }
    }
}

static void chart_at(int32_t oy)         /* the course like a kana chart: 7 x 6, coloured by box */
{
    char t[2] = {0, 0};
    uint32_t i;
    for (i = 0; i < LN_N; i++) {
        int32_t x = 2 + (int32_t)(i % 7u) * 34, y = 2 + (int32_t)(i / 7u) * 28 - oy;
        int open = i < rec.ln.open;
        uint16_t bg = open ? level_rgb(ln_level(&rec.ln, i)) : RGB(20, 20, 24);
        if (i == ui.csel)
            cv_rect(x - 2, y - 2, 36, 30, C_WHITE);
        cv_rect(x, y, 32, 26, bg);
        t[0] = ln_char(i);
        text_c(x + 16, y + 5, &FONT_B, t, open ? C_WHITE : C_DIM);
    }
}

static void set_at(int32_t oy)
{
    static const char *const KEYER[MO_NKEYER] = {"STRAIGHT", "IAMBIC A", "IAMBIC B"};
    char t[24];
    uint32_t i;
    for (i = 0; i < 8u; i++) {
        int32_t y = 2 + (int32_t)i * 21 - oy;
        if (i == ui.sel)
            cv_rect(0, y - 2, 240, 20, C_LINE);
        cv_text(8, y, &FONT_XS, SET_NAME[i], C_GRAY);
        t[0] = 0;
        switch (i) {
        case 0: int_cat(t, SET.wpm); text_cat(t, " WPM"); break;
        case 1:
            int_cat(t, SET.eff);
            text_cat(t, SET.eff < SET.wpm ? " WPM (FARNSWORTH)" : " WPM");
            break;
        case 2: int_cat(t, SET.pitch); text_cat(t, " HZ"); break;
        case 3: text_cat(t, KEYER[SET.keyer]); break;
        case 4: text_cat(t, SET.swap ? "LEFT DAH" : "LEFT DIT"); break;
        case 5: text_cat(t, SET.mute ? "OFF (LIGHTS)" : "ON"); break;
        case 6: int_cat(t, SET.cards); text_cat(t, " A ROUND"); break;
        case 7: text_cat(t, visual() ? "SIGHT" : "SOUND (BEST)"); break;
        default: break;
        }
        cv_text(100, y, &FONT_XS, t, i == ui.sel ? C_WHITE : C_RX);
    }
}

static void menu_at(int32_t oy)
{
    static const char *const NAME[PG_N - 1] = {"PRACTICE", "KEY", "TYPE", "CHART", "SETTINGS"};
    static const char *const WHAT[PG_N - 1] = {"flash cards, a few minutes", "send, it reads you",
                                               "the keys are letters", "how far you are", "speed, pitch, keyer"};
    uint32_t i;
    for (i = 0; i < PG_N - 1u; i++) {
        int32_t y = 6 + (int32_t)i * 34 - oy;
        if (i == ui.sel)
            cv_rect(0, y - 4, 240, 32, C_LINE);
        cv_text(12, y, &FONT_B, NAME[i], i == ui.sel ? C_WHITE : C_GRAY);
        cv_text(12, y + 16, &FONT_XS, WHAT[i], C_DIM);
    }
}

#define MAIN_H 176
static void main_area(void (*fn)(int32_t oy))
{
    cv_begin(240, MAIN_H / 2, C_BLACK);
    fn(0);
    cv_blit(0, 32);
    cv_begin(240, MAIN_H / 2, C_BLACK);
    fn(MAIN_H / 2);
    cv_blit(0, 32 + MAIN_H / 2);
}

static void draw_main(void)
{
    switch (ui.page) {
    case PG_KEY: key_page(); break;
    case PG_TYPE: type_page(); break;
    case PG_PRACTICE: main_area(practice_at); break;
    case PG_CHART: main_area(chart_at); break;
    case PG_SET: main_area(set_at); break;
    default: main_area(menu_at); break;
    }
}

static void draw_footer(void)
{
    const char *s = "";
    char t[40];
    const ln_card_t *c = ln_card(&pr.r);
    cv_begin(240, 32, C_BLACK);
    cv_rect(0, 0, 240, 1, C_LINE);
    switch (ui.page) {
    case PG_MENU: s = "SELECT move  SEL open  HOME back"; break;
    case PG_KEY: s = "K1 speed  K3 pitch  FX mute  OCT- clear"; break;
    case PG_TYPE: s = ui.shift ? "now: digits and signs" : "OCT- hold: 123  OCT+ del  PLAY line"; break;
    case PG_SET: s = "SELECT item  OCT- / OCT+ change"; break;
    case PG_CHART:
        t[0] = 0;
        text_cat(t, "PLAY hear ");
        t[10] = ln_char(ui.csel), t[11] = 0;
        if (visual()) {
            char p[8];
            mo_pattern(mo_code(ln_char(ui.csel)), p);
            text_cat(t, "  ");
            text_cat(t, p);
        }
        s = t;
        break;
    case PG_PRACTICE:
        if (pr.ps == PS_CARD && c)
            s = ln_choose(c->type) ? "keys E4-A4, or SELECT + SEL" : card_key(c) ? "PLAY again  SEL don't know"
                                                                                : "any key: next";
        else if (pr.ps == PS_RESULT)
            s = "any key: next";
        else
            s = "PLAY start  HOME menu  FX mute";
        break;
    default: break;
    }
    cv_text(8, 9, &FONT_XS, s, C_DIM);
    cv_blit(0, 208);
}

static int32_t lamp_y(void)              /* where this page shows the lamp, 0 none */
{
    if (ui.page == PG_KEY)
        return 88;
    if (ui.page == PG_TYPE)
        return 112;
    if (ui.page == PG_PRACTICE && (pr.ps == PS_CARD || pr.ps == PS_RESULT))
        return 196;                      /* under the card (practice_at keeps above 196) */
    return 0;
}

static void draw(uint32_t now)
{
    uint8_t r = ui.redraw;
    ui.redraw = 0;
    if (r & R_HEAD)
        draw_header(now);
    if (r & R_MAIN)
        draw_main();
    if ((r & (R_MAIN | R_LAMP)) && lamp_y() && (ui.page == PG_PRACTICE || !(r & R_MAIN)))
        draw_lamp(lamp_y());
    if (r & R_FOOT)
        draw_footer();
}

/* ------------------------------------------------------------------ audio --- */
static float gain_now;

void app_render(int32_t *lr, uint32_t n, uint32_t master_q12)
{
    uint32_t i;
    float gain = (float)master_q12 / 4096.0f * 8300000.0f;
    gain_now += (gain - gain_now) * 0.5f;
    for (i = 0; i < 2u * n; i++)
        lr[i] = 0;
    tone_render(lr, n, gain_now);
}

void app_panic(void) { tone_stop(); }

/* ------------------------------------------------------------ the contract --- */
void app_init(void)
{
    /* everything from nothing: the host simulator's reboot runs this again over the old RAM */
    zero(&ui, sizeof ui);
    zero(&pr, sizeof pr);
    zero(&grid, sizeof grid);
    zero(txt, sizeof txt);
    zero(dsrc, sizeof dsrc);
    ev_w = ev_r = 0;
    own_down = ptx_down = rx_midi = rx_down = midi_down = 0;
    key_input = own_midi = ptx_midi = 0;
    saves = 0;
    dirty_at = 0;
    if (plat_store_load(OBJ_MORSE, &rec, sizeof rec) != (int)sizeof rec || rec.magic != REC_MAGIC
        || rec.ver != REC_VER || rec.size != sizeof rec || !set_valid(&rec.set) || !ln_valid(&rec.ln)) {
        zero(&rec, sizeof rec);
        set_defaults();
        ln_init(&rec.ln);
    }
    mo_keyer_init(&keyer, SET.keyer, mo_dit_ms(SET.wpm));
    mo_player_init(&ptx, mo_dit_ms(SET.wpm), mo_gap_ms(SET.wpm, SET.eff));
    mo_player_init(&prx, mo_dit_ms(SET.wpm), mo_gap_ms(SET.wpm, SET.eff));
#ifdef PLAT_HOST
    mo_player_init(&host_hand, mo_dit_ms(SET.wpm), mo_dit_ms(SET.wpm));
#endif
    tone_stop();
    timing_apply();
    hl_init(&link, 0x4D, 0x4F, MO_VER);
    ui.page = PG_PRACTICE;               /* straight into today's practice */
    ui.back = PG_PRACTICE;
    pr.ps = PS_START;
}

void app_ui_init(void)
{
    lcd_fill(0, 0, 240, 240, C_BLACK);
    ui.redraw = R_ALL;
}

void app_frame(void)
{
    static const uint8_t HELLO[3] = {MO_VER, FW_MAJOR, FW_MINOR};
    static uint32_t last_msg;
    uint32_t now = plat_ms();
    uint8_t host, lamp, pat;
    input();
    read_events();
    if (ui.page == PG_PRACTICE)
        practice_step(now);
    hl_tick(&link, now, T_HELLO, T_HEARTBEAT, HELLO, 3);
    host = (uint8_t)hl_host(&link, now);
    if (host != ui.host_was) {
        ui.host_was = host;
        ui.redraw |= R_HEAD;
    }
    if ((ui.msg_until && now >= ui.msg_until) != last_msg) {
        last_msg = ui.msg_until && now >= ui.msg_until;
        ui.redraw |= R_HEAD;
    }
    lamp = (uint8_t)(tone_gated(TONE_TX) | tone_gated(TONE_RX) << 1);
    if (lamp != ui.lamp_was) {
        ui.lamp_was = lamp;
        ui.redraw |= R_LAMP;
    }
    pat = (uint8_t)ptx.cur;
    if (ui.page == PG_TYPE && pat != ui.pat_was) {
        ui.pat_was = pat;
        ui.redraw |= R_MAIN;
    }
    if (dirty_at && now - dirty_at > 3000u)   /* settings saved once the knobs rest */
        rec_save();
    lights(now);
    ann_tick(now);
    if (ui.redraw)
        draw(now);
}

/* --------------------------------------------------------- host simulator --- */
#ifdef PLAT_HOST
static int host_fails;

int app_expect(const char *what, int *got)
{
    if (!strcmp(what, "page"))
        *got = ui.page;
    else if (!strcmp(what, "mute"))
        *got = SET.mute;
    else if (!strcmp(what, "wpm"))
        *got = SET.wpm;
    else if (!strcmp(what, "eff"))
        *got = SET.eff;
    else if (!strcmp(what, "pitch"))
        *got = SET.pitch;
    else if (!strcmp(what, "keyer"))
        *got = SET.keyer;
    else if (!strcmp(what, "learn"))
        *got = SET.learn;
    else if (!strcmp(what, "host"))
        *got = hl_host(&link, plat_ms());
    else if (!strcmp(what, "ps"))
        *got = pr.ps;
    else if (!strcmp(what, "card"))
        *got = pr.r.at;
    else if (!strcmp(what, "cards"))
        *got = pr.r.n;
    else if (!strcmp(what, "type"))
        *got = ln_card(&pr.r) ? ln_card(&pr.r)->type : -1;
    else if (!strcmp(what, "right"))
        *got = pr.right;
    else if (!strcmp(what, "ok"))
        *got = pr.r.ok;
    else if (!strcmp(what, "miss"))
        *got = pr.r.miss;
    else if (!strcmp(what, "round"))
        *got = rec.ln.round;
    else if (!strcmp(what, "open"))
        *got = rec.ln.open;
    else if (!strcmp(what, "streak"))
        *got = rec.ln.streak;
    else if (!strcmp(what, "answered"))
        *got = (int)rec.ln.answered;
    else if (!strcmp(what, "saves"))
        *got = (int)saves;
    else if (!strcmp(what, "key_input"))
        *got = key_input;
    else if (!strcmp(what, "tx"))
        *got = tone_gated(TONE_TX);
    else if (!strcmp(what, "rx"))
        *got = tone_gated(TONE_RX);
    else
        return 0;
    return 1;
}

static void host_text(const char *a, char *out, uint32_t max)   /* a script word: _ is a space */
{
    uint32_t i;
    for (i = 0; a[i] && i < max - 1u; i++)
        out[i] = a[i] == '_' ? ' ' : a[i];
    out[i] = 0;
}

static void host_send(const char *s)     /* a hand on a straight key keys s */
{
    while (*s)
        mo_player_push(&host_hand, *s++);
}

int app_host_cmd(const char *cmd, const char *a, const char *b, const char *c, const char *d)
{
    char t[128];
    (void)c, (void)d;
    if (!strcmp(cmd, "expect_text")) {   /* expect_text own|rx|typed TEXT (_ a space; trailing spaces ignored) */
        int w = !strcmp(a, "rx") ? 1 : !strcmp(a, "typed") ? 2 : 0;
        char g[TXT_N + 1];
        uint32_t n;
        host_text(b, t, sizeof t);
        for (n = 0; txt[w][n]; n++)
            g[n] = txt[w][n];
        while (n && g[n - 1u] == ' ')
            n--;
        g[n] = 0;
        if (strcmp(g, t)) {
            printf("  FAIL expect_text %s: \"%s\", got \"%s\"\n", a, t, g);
            host_fails++;
        } else
            printf("  ok text %s \"%s\"\n", a, t);
        return 1;
    }
    if (!strcmp(cmd, "clear_text")) {
        zero(txt, sizeof txt);
        return 1;
    }
    if (!strcmp(cmd, "sendkeys")) {      /* sendkeys TEXT: keyed by hand at the set speed (_ a space) */
        host_text(a, t, sizeof t);
        host_send(t);
        return 1;
    }
    if (!strcmp(cmd, "answer_all")) {    /* answer_all right: the rest of the round at once (keyed cards answered
                                          * without the keying; "answer" keys them for real) */
        const ln_card_t *k;
        int right = !strcmp(a, "right");
        while (pr.ps == PS_CARD && (k = ln_card(&pr.r)) != 0) {
            if (k->type == CARD_NEW)
                practice_answer(1);
            else {
                if (ln_choose(k->type))
                    practice_choose(right ? k->right : (k->right + 1u) % 4u);
                else
                    practice_answer(right);
                play_stop(&ptx);
                card_present();
            }
        }
        return 1;
    }
    if (!strcmp(cmd, "answer")) {        /* answer right|wrong: the card asked now, as a player would */
        const ln_card_t *k = ln_card(&pr.r);
        int right = !strcmp(a, "right");
        if (pr.ps != PS_CARD || !k) {
            printf("  FAIL answer: no card asked\n");
            host_fails++;
        } else if (k->type == CARD_NEW)
            practice_answer(1);
        else if (ln_choose(k->type))
            practice_choose(right ? k->right : (k->right + 1u) % 4u);
        else if (!pr.answer_ok) {
            printf("  FAIL answer: the prompt is still playing\n");
            host_fails++;
        } else {
            char s[3] = {right ? ln_char(k->ch) : (ln_char(k->ch) == 'E' ? 'T' : 'E'), ' ', 0};
            host_send(s);
        }
        return 1;
    }
    return 0;
}

int app_host_fails(void) { return host_fails; }
#endif
