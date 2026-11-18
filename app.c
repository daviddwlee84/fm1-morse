/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Morse: see app.h. Part of fm1-base's unity build, after the platform (gfx.c, announce.c in scope).
 * The 1 kHz tick does what needs the millisecond: the key and the keyer, the two players, MIDI in and out, the
 * sidetone's gates; it leaves key-down / key-up events with their times in a ring. The main loop decodes them, runs
 * the pages (flash cards, listening, rhythm drills), talks to the host and draws.
 * Protocol with the host: PROTOCOL.md in github.com/daviddwlee84/M-VAVE_FM-1 host/morse (the numbers below match its host/morse/protocol.py). */
#include "fastmath.h"
#include "hostlink.c"
#include "ledfx.c"
#include "morse.c"
#include "learn.c"
#include "content.c"
#include "tone.c"

#define FW_MAJOR 0u
#define FW_MINOR 6u

/* ---------------------------------------------------------------- protocol --- */
#define MO_VER 1u
enum { T_HELLO_ACK = 0x01, T_TEXT, T_STOP, T_PROG_REQ, T_PROG_LOAD };   /* PROG: firmware 0.4 and later */
enum { T_HELLO = 0x41, T_HEARTBEAT, T_CHAR, T_PROG, T_PROG_DONE };
/* a progress backup on the wire: a format byte, ln_blob's bytes, a CRC-16/CCITT of both; cut into chunks of
 * PROG_CHUNK bytes, each frame <index> <count> and the chunk with its high bits gathered (8 bytes for each 7) */
#define PROG_FORMAT 1u
#define PROG_N (1u + LN_BLOB_N + 2u)
#define PROG_CHUNK 49u
#define PROG_FRAMES ((PROG_N + PROG_CHUNK - 1u) / PROG_CHUNK)
enum { PD_LOADED, PD_DECLINED, PD_BAD };   /* T_PROG_DONE's answer to a load */
/* Vail's MIDI keying (vail-adapter docs/MIDI_INTEGRATION_SPEC.md): channel 1, note 0 a timed key-down */
#define MIDI_KEY_ON (0x09u | 0x90u << 8 | 0x00u << 16 | 0x7Fu << 24)
#define MIDI_KEY_OFF (0x08u | 0x80u << 8 | 0x00u << 16 | 0x00u << 24)

/* the debounce keeps a key down ~8.8 ms after it opens and sees it ~1.6 ms after it closes: a straight key's marks
 * read ~7 ms long on the FM-1 (fm1-base bsp/boards/fm1/fm1_input.h; fm1-emu shows +3 / +10 ms); the host simulator
 * has no debounce */
#ifdef PLAT_HOST
#define KEY_BIAS_MS 0u
#else
#define KEY_BIAS_MS 7u
#endif

/* ---------------------------------------------------------------- settings --- */
#define REC_MAGIC 0x4D4F5231u            /* "MOR1"; another magic, or a record that does not make sense: defaults */
#define REC_VER 1u                       /* 0.2 kept 0.1's layout: its spare bytes became look, send, prog */
enum { LM_SOUND, LM_VISUAL };            /* how to learn: by sound (recommended), by sight */
enum { KV_TEXT, KV_CODE, KV_TIMING, KV_N };   /* the KEY page's view */
enum { TL_ALPHA, TL_PIANO, TL_KOCH, TL_N };   /* the TYPE page's letters on the keys */
typedef struct {
    uint8_t wpm;                         /* hearing: character speed 5..40 */
    uint8_t eff;                         /* hearing: effective speed (Farnsworth) 5..wpm */
    uint8_t keyer;                       /* MO_STRAIGHT, MO_IAMBIC_A, MO_IAMBIC_B */
    uint8_t swap;                        /* paddles: 0 left half dit, 1 left half dah */
    uint8_t mute;                        /* bit 0 the sound off (the lights keep the rhythm), bit 1 Bluetooth off
                                          * (0.5; a 0.4 record has it 0: on) */
    uint8_t cards;                       /* per round 10..30 */
    uint8_t learn;                       /* LM_* */
    uint8_t look;                        /* bits 0-1 KEY's view (KV_*), bit 2 TYPE's keys show the code, bits 3-4
                                          * TYPE's layout (TL_*), bit 5 LISTEN hides the text, bit 6 TYPE composes
                                          * (a line sent with PLAY) instead of sending each key, bit 7 PRACTICE
                                          * judges keyed answers strictly (by the send speed, as 0.3 did) */
    uint16_t pitch;                      /* Hz 300..1200 */
    uint8_t send;                        /* sending: the keyer's speed and where the decoder starts, 5..40 (0 in
                                          * a 0.1 record: the default) */
    uint8_t prog;                        /* LISTEN's program (LP_*) */
} set_t;
typedef struct {
    uint32_t magic;
    uint16_t ver, size;
    set_t set;
    ln_state_t ln;
} rec_t;
static rec_t rec;
#define SET (rec.set)
#define SEND_DEFAULT 10u                 /* a beginner's hand: a 120 ms dit (the START preset) */

static uint32_t kview(void) { return SET.look & 3u; }
static uint32_t tlabel(void) { return SET.look >> 2 & 1u; }
static uint32_t tlayout(void) { return SET.look >> 3 & 3u; }
static uint32_t lhide(void) { return SET.look >> 5 & 1u; }
static uint32_t tcompose(void) { return SET.look >> 6 & 1u; }
static uint32_t strict(void) { return SET.look >> 7 & 1u; }
static uint32_t muted(void) { return SET.mute & 1u; }
static uint32_t bt_off(void) { return SET.mute >> 1 & 1u; }
static void look_set(uint32_t shift, uint32_t mask, uint32_t v)
{
    SET.look = (uint8_t)((SET.look & ~(mask << shift)) | (v & mask) << shift);
}

static void set_defaults(void)          /* the START preset: Koch's characters at 20, gaps stretched to 8 (Farnsworth) */
{
    SET.wpm = 20;
    SET.eff = 8;
    SET.keyer = MO_STRAIGHT;
    SET.swap = 0;
    SET.mute = 0;
    SET.cards = 20;
    SET.learn = LM_SOUND;
    SET.look = 0;
    SET.pitch = 600;
    SET.send = SEND_DEFAULT;
    SET.prog = LP_KOCH;
}

static int set_valid(const set_t *s)
{
    return s->wpm >= 5u && s->wpm <= 40u && s->eff >= 5u && s->eff <= s->wpm && s->keyer < MO_NKEYER && s->swap <= 1u
           && s->mute <= 3u && s->cards >= 10u && s->cards <= 30u && s->learn <= LM_VISUAL && s->pitch >= 300u
           && s->pitch <= 1200u && (s->look & 3u) < KV_N && (s->look >> 3 & 3u) < TL_N
           && s->send >= 5u && s->send <= 40u && s->prog < LP_N;
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
enum { PG_MENU, PG_PRACTICE, PG_LISTEN, PG_KEY, PG_RHYTHM, PG_TYPE, PG_CHART, PG_SET, PG_N };
enum { PS_START, PS_CARD, PS_RESULT, PS_DONE };

/* the tick's: the key, the keyer, two players (ptx: this FM-1's own playing, prompts, typed letters and listening;
 * prx: the host's text, as received), and what they leave for the main loop */
static mo_keyer_t keyer;
static mo_player_t ptx, prx;
static volatile uint8_t key_input;       /* the keys are the Morse key now */
static volatile uint8_t own_midi;        /* own keying goes out as MIDI (KEY) */
static volatile uint8_t ptx_midi;        /* ptx's playing goes out as MIDI (TYPE) */
static uint8_t own_down, ptx_down, prx_down, rx_midi, rx_down, midi_down;
typedef struct {
    uint8_t src, down;
    uint32_t t;
} ev_t;
#define EV_N 64u
static ev_t ev[EV_N];
static volatile uint32_t ev_w, ev_r;
#if FM1B_BLE
#define BX_N 256u                        /* Bluetooth's SysEx packets, from the tick to the main loop (a 640-byte frame
                                          * is 214 of them; the host sends 64-byte ones) */
static uint32_t bx[BX_N];
static volatile uint32_t bx_w, bx_r;
#endif
static uint32_t rx_t;                    /* when the received key last changed (Bluetooth: when the sender keyed it) */
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
    uint8_t pat[TG_N];                   /* the elements read for it (a code; 0 none or too long) */
    uint32_t n;
} grid;                                  /* the KEY page's text, own and received in their colours */
#define TXT_N 96u
static char txt[3][TXT_N + 1];           /* own decoded, received decoded, typed (the simulator's expect_text) */

/* the KEY page's TIMING view: each letter's marks and the gaps inside it, as keyed */
#define TR_N 4u
#define TR_EL 7u
typedef struct {
    char c;
    uint8_t n, dah;                      /* elements, and which were read as dahs (bit i) */
    uint16_t mark[TR_EL], gap[TR_EL];    /* ms; gap[i] the one before element i (gap[0] 0) */
} trow_t;
static struct {
    trow_t row[TR_N], live;
    uint8_t head, count;
    uint16_t next_gap;                   /* the gap before the next mark, if it is inside a letter */
    uint16_t next_gap_rh;                /* the gap before the next mark, whatever it was (RHYTHM) */
} tim;

static struct {
    uint8_t page, back, sel;             /* sel: the menu's / the settings' cursor */
    uint8_t csel;                        /* the chart's */
    uint8_t redraw;                      /* bits: R_* */
    uint32_t keys, btn;
    char msg[2][24];
    uint32_t msg_until;
    uint8_t tpage;                       /* TYPE: 0 letters, 1 digits and signs, 2 phrases */
    char line[TG_COLS * 2u + 1u];        /* TYPE: the line typed */
    const char *gloss;                   /* TYPE: what the last phrase means */
    uint32_t rec_t;                      /* TYPE: REC went down at (0: up, or its hold already cleared the line) */
    char last;                           /* KEY: the last character read */
    uint8_t host_was, ble_was, lamp_was, pat_was;
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
    uint8_t res;                         /* LN_RIGHT / LN_WRONG / LN_ALMOST: the last answer */
    uint8_t test;                        /* the start page's choice: 0 a round, 1 a placement test */
    uint8_t hand;                        /* the own decoder holds this hand's speed (carried from card to card) */
    uint32_t unit0;                      /* the hand's dit when the answer began, ms */
    /* the keyed answer as keyed: marks and the gaps before them, ms */
    uint16_t am[8], ag[8];
    uint8_t an, has_j;
    uint8_t peek;                        /* SEQ: the code shown on this new letter / answer (learning by sound) */
    uint16_t pend;                       /* the gap before the mark now down */
    uint32_t a_up;                       /* the last mark ended at */
    char read[4];                        /* what the decoder read meanwhile */
    mo_judge_t j;
} pr;

/* a question on the screen that only PLAY (yes) or HOME (no) answers: changes to the progress never happen by a
 * stray key */
enum { ASK_NONE, ASK_SAVE, ASK_LOAD, ASK_PLACE };
static struct {
    uint8_t ask;
    uint8_t edit, dirty;                 /* CHART: editing a copy, changed */
    uint32_t sel_t;                      /* CHART edit: SEL went down at (held: reset), 0 up */
    ln_state_t draft;                    /* CHART's copy; ASK_LOAD's backup */
} pg;

static struct {                          /* a progress backup going out, coming in */
    uint8_t tx_on, tx_i;
    uint8_t rx_next;                     /* the chunk expected next */
    uint8_t buf[PROG_FRAMES * PROG_CHUNK];
} prog;

static const uint8_t WHITE[16] = PLAT_WHITE_KEYS;
static const uint8_t BLACK[11] = PLAT_BLACK_KEYS;
static const uint8_t CHOICE_KEY[4] = {11, 12, 14, 16};   /* E4 F4 G4 A4: the four answers */

static void zero(void *p, uint32_t n)
{
    uint8_t *b = (uint8_t *)p;
    while (n--)
        *b++ = 0;
}

static uint32_t slen(const char *s)
{
    uint32_t n = 0;
    while (s[n])
        n++;
    return n;
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

static void tenths_cat(char *b, uint32_t v)   /* 13 -> "1.3" */
{
    char t[2] = {0, 0};
    int_cat(b, (int32_t)(v / 10u));
    text_cat(b, ".");
    t[0] = (char)('0' + v % 10u);
    text_cat(b, t);
}

static void note_name(uint32_t key, char *b)   /* key 0 is F3 */
{
    static const char *const N[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    uint32_t note = 53u + key;
    b[0] = 0;
    text_cat(b, N[note % 12u]);
    int_cat(b, (int32_t)(note / 12u) - 1);
}

static uint32_t wrap(uint32_t v, int32_t d, uint32_t n)
{
    return (uint32_t)(((int32_t)v + d % (int32_t)n + (int32_t)n) % (int32_t)n);
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
    ui.msg_until = plat_ms() + 2000u;
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
/* the speeds into the keyer, the players and the decoders (main loop; the tick locked out). Hearing (what this
 * FM-1 plays) and sending (what you key) have their own: Koch wants the characters heard fast, a beginner's hand is
 * slow */
static struct {
    uint8_t wpm, eff, send;
} applied;                               /* the speeds the decoders were started at */

static void timing_apply(void)
{
    uint32_t dit = mo_dit_ms(SET.wpm), gap = mo_gap_ms(SET.wpm, SET.eff), sdit = mo_dit_ms(SET.send);
    uint32_t f = plat_lock();
    keyer.mode = SET.keyer;
    keyer.dit = sdit;
    ptx.dit = prx.dit = dit;
    ptx.gap = prx.gap = gap;
#ifdef PLAT_HOST
    host_hand.dit = sdit;
    host_hand.gap = sdit;                /* a hand keys without Farnsworth */
#endif
    plat_unlock(f);
    if (applied.send != SET.send)        /* a new speed starts the decoder there; muting or a pitch keeps its state */
        mo_dec_init(&dsrc[SRC_OWN].dec, sdit, sdit);
    if (applied.wpm != SET.wpm || applied.eff != SET.eff)
        mo_dec_init(&dsrc[SRC_RX].dec, dit, gap);
    applied.wpm = SET.wpm, applied.eff = SET.eff, applied.send = SET.send;
    tone_pitch(TONE_TX, (float)SET.pitch);
    tone_pitch(TONE_RX, (float)SET.pitch * 1.26f);   /* received a major third up */
    tone_mute((int)muted());
}

static void set_changed(void)
{
    timing_apply();
    dirty_at = plat_ms() | 1u;
    ui.redraw |= R_ALL;
}

static void look_changed(void)          /* a view changed: saved like a setting, nothing retimed */
{
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

static uint32_t queued(const mo_player_t *p)   /* characters waiting */
{
    uint32_t f = plat_lock(), n = (p->head + MO_PLAY_Q - p->tail) % MO_PLAY_Q;
    plat_unlock(f);
    return n;
}

static int took_start(mo_player_t *p)    /* a character began playing since the last look (once) */
{
    uint32_t f = plat_lock();
    int s = p->started;
    p->started = 0;
    plat_unlock(f);
    return s;
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
    plat_midi_send(down ? MIDI_KEY_ON : MIDI_KEY_OFF, TO_USB | (FM1B_BLE && !bt_off() ? TO_BLE_HOST : 0u));
}

/* a key change from MIDI at the time it was made: Bluetooth's by the sender's BLE-MIDI timestamp (ble_midi_time),
 * the connection's bunching taken out; never before the last one, never ahead of now */
static void rx_at(int port, uint32_t now)
{
    uint32_t t = now;
    if (port == PORT_BLE) {
        int32_t ago = (int32_t)(plat_ticks24() - plat_midi_in_t24()) / (int32_t)(PLAT_TICKS_PER_US * 1000u);
        if (ago > 0)
            t = now - (uint32_t)(ago < 500 ? ago : 500);
    }
    if ((int32_t)(t - rx_t) < 0)
        t = rx_t;
    rx_t = t;
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
        uint32_t st = pkt >> 8 & 0xFFu, vel = pkt >> 24 & 0x7Fu, cin = pkt & 0x0Fu;
#if FM1B_BLE
        if (port == PORT_BLE && cin >= 4u && cin <= 7u) {   /* the host's frames, put together in the main loop */
            uint32_t next = (bx_w + 1u) % BX_N;
            if (next != bx_r) {
                bx[bx_w] = pkt;
                bx_w = next;
            }
            continue;
        }
#else
        (void)cin;
#endif
        if (st == 0x90u || st == 0x80u) {
            /* each change goes in as it comes: over Bluetooth a short dit's down and up can come in one tick,
             * each at its own time */
            rx_midi = (uint8_t)(st == 0x90u && vel);
            rx = rx_midi || prx_down;
            if (rx != rx_down) {
                rx_down = (uint8_t)rx;
                rx_at(port, now);
                ev_push(SRC_RX, rx, rx_t);
            }
        }
    }
    prx_down = (uint8_t)mo_player_tick(&prx);
    rx = rx_midi || prx_down;
    if (rx != rx_down) {                 /* the computer's text played here */
        rx_down = (uint8_t)rx;
        rx_at(PORT_USB, now);
        ev_push(SRC_RX, rx, rx_t);
    }
    tone_gate(TONE_RX, rx);
}

/* ------------------------------------------------------------- the reading --- */
static void txt_add(uint32_t which, char c)
{
    char *t = txt[which];
    uint32_t n = slen(t), i;
    if (n >= TXT_N) {
        for (i = 0; i + 1u < TXT_N; i++)
            t[i] = t[i + 1u];
        n = TXT_N - 1u;
    }
    t[n] = c;
    t[n + 1u] = 0;
}

static void grid_add(char c, uint32_t who, uint8_t pat)
{
    uint32_t i;
    if (c == ' ' && (grid.n % TG_COLS == 0u || (grid.n && grid.c[grid.n - 1u] == ' ')))
        return;
    if (grid.n >= TG_N) {
        for (i = 0; i < TG_N - TG_COLS; i++) {
            grid.c[i] = grid.c[i + TG_COLS];
            grid.who[i] = grid.who[i + TG_COLS];
            grid.pat[i] = grid.pat[i + TG_COLS];
        }
        grid.n -= TG_COLS;
    }
    grid.c[grid.n] = c;
    grid.who[grid.n] = (uint8_t)who;
    grid.pat[grid.n] = c == ' ' ? 0u : pat;
    grid.n++;
}

static void send_text(const char *s)     /* to the host (the first HOSTLINK_MAX characters), which may type it */
{
    uint8_t p[HOSTLINK_MAX];
    uint32_t n = 0;
    if (!hl_host(&link, plat_ms()))
        return;
    for (; *s && n < HOSTLINK_MAX; s++)
        if (*s >= 0x20 && *s < 0x7F)
            p[n++] = (uint8_t)*s;
    if (n)
        hl_send(&link, T_CHAR, p, n);
}

static void send_char(char c)
{
    char s[2] = {c, 0};
    send_text(s);
}

static void tim_push(char c)             /* a letter read: its row for the TIMING view */
{
    uint8_t pat = dsrc[SRC_OWN].dec.last_pat;
    trow_t *r = &tim.row[tim.head];
    uint32_t i;
    if (!tim.live.n)
        return;
    *r = tim.live;
    r->c = c;
    r->dah = 0;
    for (i = 0; i < r->n; i++)           /* as the decoder read them in the end (it may have read one again) */
        if (pat > 1u && mo_len(pat) == r->n ? mo_elem(pat, i) : mo_dec_is_dah(&dsrc[SRC_OWN].dec, r->mark[i]))
            r->dah |= (uint8_t)(1u << i);
    tim.head = (uint8_t)((tim.head + 1u) % TR_N);
    if (tim.count < TR_N)
        tim.count++;
    tim.live.n = 0;
}

static void practice_keyed(char c);
static void practice_edge(int down, uint32_t t, uint32_t ms);
static void rhythm_mark(uint32_t mark, uint32_t gap);

static void emit(uint32_t src, char c)
{
    uint8_t pat = c == ' ' ? 0u : dsrc[src].dec.last_pat;
    if (src == SRC_RX) {
        txt_add(1, c);
        if (ui.page == PG_KEY) {
            grid_add(c, SRC_RX, pat);
            ui.redraw |= R_MAIN;
        }
        return;
    }
    txt_add(0, c);
    if (c != ' ')
        tim_push(c);
    if (ui.page == PG_KEY) {
        grid_add(c, SRC_OWN, pat);
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
        uint32_t n = 0, i, bias, gap = 0;
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
        bias = e.src == SRC_OWN && SET.keyer == MO_STRAIGHT ? KEY_BIAS_MS : 0u;
        if (e.down) {
            if (dsrc[e.src].seen && !dsrc[e.src].down) {
                gap = e.t - dsrc[e.src].t_up + bias;
                n = mo_dec_gap(&dsrc[e.src].dec, gap, out);
            }
            if (e.src == SRC_OWN)
                practice_edge(1, e.t, gap);
            dsrc[e.src].t_down = e.t;
        } else if (dsrc[e.src].down) {
            uint32_t mark = e.t - dsrc[e.src].t_down;
            mark = mark > bias ? mark - bias : mark;
            mo_dec_mark(&dsrc[e.src].dec, mark);
            dsrc[e.src].t_up = e.t;
            if (e.src == SRC_OWN) {
                practice_edge(0, e.t, mark);
                if (tim.live.n < TR_EL) {
                    tim.live.mark[tim.live.n] = (uint16_t)(mark > 0xFFFFu ? 0xFFFFu : mark);
                    tim.live.gap[tim.live.n] = tim.live.n ? tim.next_gap : 0u;
                    tim.live.n++;
                }
                if (ui.page == PG_RHYTHM)
                    rhythm_mark(mark, tim.next_gap_rh);
            }
        }
        dsrc[e.src].down = e.down;
        dsrc[e.src].seen = 1;
        for (i = 0; i < n; i++)
            emit(e.src, out[i]);
        if (e.src == SRC_OWN && e.down) {
            /* the gap just over was inside a letter if the letter is still open after it */
            tim.next_gap = (uint16_t)(mo_dec_pending(&dsrc[SRC_OWN].dec) != 1u && gap < 0xFFFFu ? gap : 0u);
            tim.next_gap_rh = (uint16_t)(gap > 0xFFFFu ? 0xFFFFu : gap);
        }
        if (e.src == SRC_OWN && (visual() || kview() != KV_TEXT || ui.page == PG_PRACTICE))
            ui.redraw |= R_MAIN;         /* the code so far is drawn */
    }
    for (s = 0; s < SRC_N; s++)          /* silence ends a character, then a word */
        if (dsrc[s].seen && !dsrc[s].down) {
            char c = mo_dec_idle(&dsrc[s].dec, now - dsrc[s].t_up);
            if (c)
                emit(s, c);
        }
}

/* ---------------------------------------------------------------- the host --- */
static void prog_in(const uint8_t *d, uint32_t len);
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
    case T_TEXT:                         /* the computer's text, played as received (<..> a prosign) */
        for (i = 0; i < len; i++) {
            char c = (char)d[i];
            if (c >= 'a' && c <= 'z')
                c = (char)(c - 32);
            if (c == ' ' || c == '<' || c == '>' || mo_code(c))
                play_char(&prx, c);
        }
        break;
    case T_STOP:
        play_stop(&prx);
        break;
    case T_PROG_REQ:                     /* the progress, out (nothing changes here) */
        prog.tx_on = 1;
        prog.tx_i = 0;
        break;
    case T_PROG_LOAD:
        prog_in(d, len);
        break;
    default:
        break;
    }
    return 1;
}

int app_sysex(const uint8_t *p, uint32_t n) { return host_frame(p, n, HL_USB); }

#if FM1B_BLE
/* Bluetooth: the own stack hands SysEx over as USB-MIDI packets on PORT_BLE; the tick queues them (bx) */
static hl_sx_t ble_rx;
static void ble_in(void)
{
    const uint8_t *f;
    uint32_t n;
    while (bx_r != bx_w) {
        uint32_t pkt = bx[bx_r];
        bx_r = (bx_r + 1u) % BX_N;
        if (hl_sx_packet(&ble_rx, pkt, &f, &n))
            host_frame(f, n, HL_BLE);
    }
}

/* nothing of Bluetooth's is written: the own stack makes its address from the flash part's unique ID (the same at
 * every start) and there is no pairing to keep (macOS and iOS connect without one), as in Conductor */
int app_ble_load(struct plat_ble_rec *r)
{
    (void)r;
    return 0;
}
int app_ble_save(const struct plat_ble_rec *r)
{
    (void)r;
    return 0;
}
#endif

/* APP_BLE_AT_BOOT: the device asks before app_init, so it reads the record itself, into its own copy */
int morse_ble_off(void)
{
    static rec_t r;
    if (plat_store_load(OBJ_MORSE, &r, sizeof r) != (int)sizeof r || r.magic != REC_MAGIC || r.ver != REC_VER
        || r.size != sizeof r || r.set.mute > 3u)
        return 0;
    return r.set.mute >> 1 & 1;
}

static void ble_toggle(void)
{
    SET.mute ^= 2u;
    set_changed();
#if FM1B_BLE
    plat_ble_enable(!bt_off());
#endif
    app_say("BLUETOOTH", bt_off() ? "OFF" : "ON");
}

/* ------------------------------------------------------------- the pages --- */
static void inputs_for_page(void)        /* who the keys and MIDI belong to on this page */
{
    key_input = !pg.ask && (ui.page == PG_KEY || ui.page == PG_RHYTHM
                            || (ui.page == PG_PRACTICE && pr.ps == PS_CARD && pr.answer_ok));
    own_midi = ui.page == PG_KEY;
    ptx_midi = ui.page == PG_TYPE;
}

static void practice_leave(void);
static void listen_stop(void);

static void set_page(uint32_t p)
{
    if (ui.page == PG_PRACTICE && p != PG_PRACTICE)
        practice_leave();
    if (ui.page == PG_LISTEN && p != PG_LISTEN)
        listen_stop();
    if (ui.page == PG_TYPE && p != PG_TYPE)
        play_stop(&ptx);
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

static void own_reset(void)              /* the decoder for a keyed answer, at the send speed */
{
    mo_dec_init(&dsrc[SRC_OWN].dec, mo_dit_ms(SET.send), mo_dit_ms(SET.send));
}

/* the keying is the answer from now: strictly, the decoder at the send speed, as 0.3 did; else the hand's speed
 * carried from the last card (the first card of a round: between the hearing and the send speed, since a hand
 * copies what it hears) */
static void hand_ready(void)
{
    mo_dec_t *d = &dsrc[SRC_OWN].dec;
    if (strict())
        own_reset();
    else if (!pr.hand) {
        uint32_t dit = (mo_dit_ms(SET.wpm) + mo_dit_ms(SET.send)) / 2u;
        mo_dec_init(d, dit, dit);
        pr.hand = 1;
    } else
        mo_dec_restart(d);
    pr.unit0 = mo_dec_unit(d);
    pr.an = 0;
    pr.has_j = 0;
    pr.read[0] = 0;
}

static int capturing(void)               /* the keys are taken as a keyed answer now */
{
    const ln_card_t *c = ln_card(&pr.r);
    return ui.page == PG_PRACTICE && pr.ps == PS_CARD && c && card_key(c) && pr.answer_ok;
}

static void practice_edge(int down, uint32_t t, uint32_t ms)   /* own key: down after a gap of ms, up after ms */
{
    if (!capturing())
        return;
    if (down)
        pr.pend = (uint16_t)(pr.an ? (ms > 0xFFFFu ? 0xFFFFu : ms) : 0u);
    else {
        if (pr.an < 8u) {
            pr.am[pr.an] = (uint16_t)(ms > 0xFFFFu ? 0xFFFFu : ms);
            pr.ag[pr.an] = pr.pend;
            pr.an++;
        }
        pr.a_up = t;
    }
}

static void card_present(void)
{
    const ln_card_t *c = ln_card(&pr.r);
    char ch;
    pr.answer_ok = 0;
    pr.got = 0;
    pr.pick = 0;
    pr.an = 0;
    pr.has_j = 0;
    pr.peek = 0;
    if (!c) {
        pr.active = 0;
        pr.ps = PS_DONE;
        if (pr.r.place) {                /* a placement test: only what it found, and only if you say so */
            uint32_t n = ln_place_count(&pr.r);
            if (n)
                pg.ask = ASK_PLACE;
            pr.opened = 0;
        } else {
            pr.opened = (uint8_t)ln_finish(&rec.ln, &pr.r);
            rec_save();
            if (pr.opened)
                app_say("NEW LETTERS", "");
        }
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
    inputs_for_page();
    ui.redraw |= R_ALL;
}

static void practice_start(void)
{
    uint32_t flags = (muted() ? LN_MUTED : 0u) | (visual() ? LN_VISUAL : 0u);
    if (pr.test) {
        if (rec.ln.open >= LN_N) {
            app_say("ALL OPEN", "NOTHING TO TEST");
            return;
        }
        flags |= LN_PLACE;
    }
    ln_build(&rec.ln, &pr.r, SET.cards, flags, plat_ms() * 2654435761u + rec.ln.round);
    pr.active = 1;
    pr.opened = 0;
    pr.hand = 0;
    card_present();
}

static void practice_answer(int res)
{
    const ln_card_t *c = ln_card(&pr.r);
    uint32_t wait;
    if (!c || pr.ps != PS_CARD)
        return;
    if (c->type == CARD_NEW) {           /* only shown: on to the next */
        ln_answer(&rec.ln, &pr.r, LN_RIGHT);
        play_stop(&ptx);
        card_present();
        return;
    }
    pr.res = (uint8_t)res;
    pr.right = (uint8_t)(res == LN_RIGHT);
    pr.peek = 0;
    pr.has_j = (uint8_t)(card_key(c) && pr.an);
    if (pr.has_j)
        pr.j = mo_judge(pr.am, pr.ag, pr.an, mo_code(ln_char(c->ch)), pr.unit0);
    pr.ratio10 = card_key(c) ? mo_dec_ratio10(&dsrc[SRC_OWN].dec) : 0u;
    ln_answer(&rec.ln, &pr.r, res);
    pr.ps = PS_RESULT;
    pr.answer_ok = 0;
    play_stop(&ptx);
    if (res != LN_RIGHT)
        play_char(&ptx, ln_char(c->ch));   /* how it should have gone */
    wait = res != LN_RIGHT ? 2800u : pr.has_j && pr.j.score < 85u ? 1800u : 900u;
    pr.until = plat_ms() + wait;
    inputs_for_page();
    ui.redraw |= R_ALL;
}

static void practice_judge(void)         /* the keyed answer is over: judged by its own rhythm */
{
    const ln_card_t *c = ln_card(&pr.r);
    uint8_t want;
    mo_judge_t j;
    if (!c)
        return;
    want = mo_code(ln_char(c->ch));
    j = mo_judge(pr.am, pr.ag, pr.an, want, pr.unit0);
    if (j.verdict != MJ_WRONG)
        mo_dec_teach(&dsrc[SRC_OWN].dec, pr.am, pr.an, want);   /* the hand's speed, read right */
    practice_answer(j.verdict == MJ_RIGHT ? LN_RIGHT : j.verdict == MJ_ALMOST ? LN_ALMOST : LN_WRONG);
}

static void practice_keyed(char c)       /* the own decoder read a character during a card */
{
    const ln_card_t *k = ln_card(&pr.r);
    uint32_t n = slen(pr.read);
    if (pr.ps != PS_CARD || !k || !card_key(k) || !pr.answer_ok)
        return;
    if (n < sizeof pr.read - 1u) {
        pr.read[n] = c;
        pr.read[n + 1u] = 0;
    }
    pr.got = c;
    if (strict())                        /* 0.3's way: what the decoder read at the send speed */
        practice_answer(c == ln_char(k->ch) ? LN_RIGHT : LN_WRONG);
}

static void practice_choose(uint32_t i)
{
    const ln_card_t *c = ln_card(&pr.r);
    if (pr.ps != PS_CARD || !c || !ln_choose(c->type) || i > 3u)
        return;
    pr.got = ln_char(c->opt[i]);
    practice_answer(i == c->right ? LN_RIGHT : LN_WRONG);
}

static void practice_leave(void)
{
    if (pr.active) {
        pr.active = 0;
        if (!pr.r.place) {
            ln_finish(&rec.ln, &pr.r);
            rec_save();
        }
    }
    play_stop(&ptx);
    pr.ps = PS_START;
}

static void practice_step(uint32_t now)
{
    const ln_card_t *c = ln_card(&pr.r);
    if (pr.ps == PS_CARD && c && card_key(c) && !pr.answer_ok && !playing(&ptx)) {
        pr.answer_ok = 1;                /* the prompt has played: now keying answers */
        hand_ready();
        inputs_for_page();
        ui.redraw |= R_FOOT | R_MAIN;
    }
    /* the answer ends with a silence: a letter's gap once it has its marks, longer while it is short of them (a
     * long gap inside it should not cut it) */
    if (!strict() && capturing() && pr.an && !dsrc[SRC_OWN].down) {
        uint32_t len = mo_len(mo_code(ln_char(c->ch))), u = pr.unit0, wait;
        if (pr.an < len)
            wait = 6u * u > 900u ? 6u * u : 900u;
        else {
            uint32_t du = 2u * mo_dec_unit(&dsrc[SRC_OWN].dec) + 20u;
            wait = 3u * u > du ? 3u * u : du;
        }
        if ((int32_t)(now - pr.a_up) >= (int32_t)wait)
            practice_judge();
    }
    if (pr.ps == PS_RESULT && (int32_t)(now - pr.until) >= 0 && !playing(&ptx))
        card_present();
}

/* ------------------------------------------------------- progress by hand --- */
/* CHART's edit works on a copy; PLAY asks, and only PLAY again writes it. A placement test's finds and a backup
 * from the computer are asked the same way */
static ln_state_t *chart_state(void) { return pg.edit ? &pg.draft : &rec.ln; }

static void chart_edit_on(void)
{
    pg.draft = rec.ln;
    pg.edit = 1;
    pg.dirty = 0;
    pg.sel_t = 0;
    app_say("EDIT", "PLAY SAVES");
    ui.redraw |= R_ALL;
}

static void chart_edit_off(const char *why)
{
    pg.edit = 0;
    pg.dirty = 0;
    pg.sel_t = 0;
    if (why)
        app_say(why, "");
    ui.redraw |= R_ALL;
}

static void chart_changed(const char *a, const char *b)
{
    pg.dirty = 1;
    app_say(a, b);
    ui.redraw |= R_MAIN | R_FOOT;
}

static void chart_level(int32_t d)       /* the letter under the cursor a level up or down (both its boxes) */
{
    ln_state_t *s = &pg.draft;
    uint32_t i = ui.csel;
    int32_t lv = (i < s->open ? (int32_t)ln_level(s, i) : -1) + d;
    char a[16];
    lv = clamp(lv, 0, (int32_t)LN_TOP);
    ln_set_level(s, i, (uint32_t)lv);
    a[0] = ln_char(i), a[1] = 0;
    text_cat(a, " LEVEL ");
    int_cat(a, lv);
    chart_changed(a, lv ? "" : "NEW AGAIN");
}

static void chart_groups(int32_t d)      /* open a group more or less */
{
    char a[16];
    ln_open_to(&pg.draft, (uint32_t)clamp((int32_t)pg.draft.open + d * (int32_t)LN_GROUP, (int32_t)LN_GROUP,
                                          (int32_t)LN_N));
    a[0] = 0;
    int_cat(a, pg.draft.open);
    text_cat(a, " OPEN");
    chart_changed(a, d > 0 ? "NEW ONES AT THE LINE" : "REST NEW AGAIN");
}

static void chart_save_or_leave(void)    /* PLAY / REC / HOME in the edit */
{
    if (pg.dirty)
        pg.ask = ASK_SAVE;
    else
        chart_edit_off("EDIT DONE");
    inputs_for_page();
    ui.redraw |= R_ALL;
}

static void prog_done(uint8_t code)      /* the answer to a backup sent to load */
{
    uint8_t b[1] = {code};
    hl_send(&link, T_PROG_DONE, b, 1);
}

static void ask_yes(void)
{
    uint32_t n;
    char a[24];
    switch (pg.ask) {
    case ASK_SAVE:
        rec.ln = pg.draft;
        rec_save();
        chart_edit_off("SAVED");
        break;
    case ASK_LOAD:
        if (pr.active || pr.ps != PS_START)
            practice_leave();
        rec.ln = pg.draft;
        rec_save();
        app_say("PROGRESS LOADED", "");
        prog_done(PD_LOADED);
        break;
    case ASK_PLACE:
        n = ln_place_apply(&rec.ln, &pr.r);
        rec_save();
        a[0] = 0;
        int_cat(a, (int32_t)n);
        text_cat(a, " MARKED KNOWN");
        app_say(a, "");
        break;
    default:
        break;
    }
    pg.ask = ASK_NONE;
    inputs_for_page();
    ui.redraw |= R_ALL;
}

static void ask_no(void)
{
    switch (pg.ask) {
    case ASK_SAVE: chart_edit_off("NOT SAVED"); break;
    case ASK_LOAD:
        app_say("NOT LOADED", "");
        prog_done(PD_DECLINED);
        break;
    case ASK_PLACE: app_say("NOTHING CHANGED", ""); break;
    default: break;
    }
    pg.ask = ASK_NONE;
    inputs_for_page();
    ui.redraw |= R_ALL;
}

/* -------------------------------------------------------- progress backup --- */
static uint16_t crc16(const uint8_t *p, uint32_t n)   /* CRC-16/CCITT-FALSE */
{
    uint16_t c = 0xFFFFu;
    uint32_t k;
    while (n--) {
        c ^= (uint16_t)(*p++ << 8);
        for (k = 0; k < 8u; k++)
            c = (uint16_t)(c & 0x8000u ? (c << 1) ^ 0x1021u : c << 1);
    }
    return c;
}

static void prog_fill(uint8_t *b)        /* PROG_N bytes: the format, the record, the CRC */
{
    uint16_t c;
    b[0] = PROG_FORMAT;
    ln_blob(&rec.ln, b + 1);
    c = crc16(b, 1u + LN_BLOB_N);
    b[1u + LN_BLOB_N] = (uint8_t)c;
    b[2u + LN_BLOB_N] = (uint8_t)(c >> 8);
}

static uint32_t pack7(const uint8_t *in, uint32_t n, uint8_t *out)   /* each 7 bytes: their high bits, then them */
{
    uint32_t i, j, k = 0;
    for (i = 0; i < n; i += 7u) {
        uint32_t m = n - i < 7u ? n - i : 7u;
        uint8_t hi = 0;
        for (j = 0; j < m; j++)
            hi |= (uint8_t)((in[i + j] >> 7) << j);
        out[k++] = hi;
        for (j = 0; j < m; j++)
            out[k++] = in[i + j] & 0x7Fu;
    }
    return k;
}

static uint32_t unpack7(const uint8_t *in, uint32_t n, uint8_t *out, uint32_t max)
{
    uint32_t i = 0, j, k = 0;
    while (i < n) {
        uint8_t hi = in[i++];
        for (j = 0; j < 7u && i < n && k < max; j++)
            out[k++] = (uint8_t)(in[i++] | ((hi >> j) & 1u) << 7);
    }
    return k;
}

static void prog_pump(void)              /* the backup out, a frame each main-loop pass while the USB takes them */
{
    uint8_t b[PROG_N], f[2u + PROG_CHUNK + PROG_CHUNK / 7u + 1u];
    uint32_t off, m, k;
    if (!prog.tx_on)
        return;
    prog_fill(b);
    off = prog.tx_i * PROG_CHUNK;
    m = PROG_N - off < PROG_CHUNK ? PROG_N - off : PROG_CHUNK;
    f[0] = prog.tx_i;
    f[1] = (uint8_t)PROG_FRAMES;
    k = 2u + pack7(b + off, m, f + 2);
    if (hl_send(&link, T_PROG, f, k) == 0 && ++prog.tx_i >= PROG_FRAMES)
        prog.tx_on = 0;
}

static void prog_in(const uint8_t *d, uint32_t len)   /* a chunk of a backup to load */
{
    uint32_t i, off;
    uint16_t c;
    if (len < 3u || d[1] != PROG_FRAMES || d[0] >= PROG_FRAMES) {
        prog_done(PD_BAD);
        return;
    }
    if (d[0] == 0u)
        prog.rx_next = 0;
    if (d[0] != prog.rx_next) {          /* one missed: start again */
        prog.rx_next = 0;
        prog_done(PD_BAD);
        return;
    }
    off = d[0] * PROG_CHUNK;
    (void)unpack7(d + 2, len - 2u, prog.buf + off, PROG_N - off < PROG_CHUNK ? PROG_N - off : PROG_CHUNK);
    prog.rx_next++;
    if (prog.rx_next < PROG_FRAMES)
        return;
    prog.rx_next = 0;
    c = crc16(prog.buf, 1u + LN_BLOB_N);
    if (pg.ask && pg.ask != ASK_LOAD) {  /* another question is up: answer that first */
        prog_done(PD_DECLINED);
        return;
    }
    if (pg.edit)
        chart_edit_off("EDIT DROPPED");
    i = prog.buf[0] == PROG_FORMAT && prog.buf[1u + LN_BLOB_N] == (uint8_t)c
        && prog.buf[2u + LN_BLOB_N] == (uint8_t)(c >> 8) && ln_unblob(&pg.draft, prog.buf + 1);
    if (!i) {
        prog_done(PD_BAD);
        app_say("BACKUP REFUSED", "IT DOES NOT CHECK OUT");
        return;
    }
    pg.ask = ASK_LOAD;
    inputs_for_page();
    ui.redraw |= R_ALL;
}

/* ----------------------------------------------------------------- listen --- */
/* lines from content.c played one after another; the one playing shown as it goes (or hidden: copy it in your
 * head, it shows once the next one starts) */
#define LS_N 2u
static struct {
    uint8_t on;
    struct {
        char t[CT_LINE_MAX + 1];
        const char *gloss;
        uint8_t nplay, played;
    } l[LS_N];
    uint8_t cur, have;                   /* the line playing; lines generated and not done (1 or 2) */
    uint8_t feed, pos;                   /* the line being queued, and its next character */
    char prev[CT_LINE_MAX + 1];          /* the line before, shown whole */
    const char *prev_gloss;
    uint32_t count;                      /* lines heard */
    ct_gen_t gen;
} ls;

static int playable(char c) { return c != ' ' && c != '<' && c != '>' && mo_code(c); }

static uint32_t playable_n(const char *s)
{
    uint32_t n = 0;
    for (; *s; s++)
        n += (uint32_t)playable(*s);
    return n;
}

static void listen_new(uint32_t i)
{
    ct_line(&ls.gen, SET.prog, rec.ln.open, ls.l[i].t, &ls.l[i].gloss);
    ls.l[i].nplay = (uint8_t)playable_n(ls.l[i].t);
    ls.l[i].played = 0;
}

static void listen_stop(void)
{
    if (ls.on)
        play_stop(&ptx);
    ls.on = 0;
    ls.have = 0;
    ui.redraw |= R_MAIN | R_FOOT;
}

static void listen_start(void)
{
    play_stop(&ptx);
    (void)took_start(&ptx);
    ct_init(&ls.gen, plat_ms() * 2654435761u + 1u);
    ls.on = 1;
    ls.cur = ls.feed = 0;
    ls.pos = 0;
    listen_new(0);
    ls.have = 1;
    ls.prev[0] = 0;
    ls.prev_gloss = 0;
    ui.redraw |= R_MAIN | R_FOOT;
}

static void listen_step(void)
{
    uint32_t guard = 0;
    if (!ls.on)
        return;
    /* keep the player fed, a word at a time, a line ahead */
    while (queued(&ptx) < 24u && guard++ < 8u) {
        const char *t = ls.l[ls.feed].t;
        char w[CT_LINE_MAX + 2];
        uint32_t n = 0;
        if (!t[ls.pos]) {                /* this line is queued: the next one, after a pause */
            if (ls.have >= LS_N)
                break;
            ls.feed = (uint8_t)((ls.feed + 1u) % LS_N);
            listen_new(ls.feed);
            ls.have++;
            ls.pos = 0;
            play(&ptx, "  ");
            continue;
        }
        while (t[ls.pos] && t[ls.pos] != ' ' && n < CT_LINE_MAX)
            w[n++] = t[ls.pos++];
        if (t[ls.pos] == ' ')
            w[n++] = t[ls.pos++];
        w[n] = 0;
        play(&ptx, w);
    }
    /* what started: the line playing moves on */
    if (took_start(&ptx)) {
        uint32_t c = ls.cur;
        if (ls.l[c].played >= ls.l[c].nplay && ls.have > 1u) {   /* this one is done: the next began */
            uint32_t i;
            for (i = 0; i <= CT_LINE_MAX; i++)
                ls.prev[i] = ls.l[c].t[i];
            ls.prev_gloss = ls.l[c].gloss;
            ls.count++;
            ls.cur = (uint8_t)((c + 1u) % LS_N);
            ls.have--;
            c = ls.cur;
        }
        ls.l[c].played++;
        ui.redraw |= R_MAIN;
    }
}

/* ----------------------------------------------------------------- rhythm --- */
/* drills of short and long presses, each measured against the send speed's dit: the box is the target, the bar what
 * you pressed, and a hint from the last presses */
static const struct {
    const char *name, *pat;
} DRILL[] = {
    {"DITS", "....."}, {"DAHS", "-----"}, {"DIT DAH", ".-.-.-"}, {"SOS", "...---..."}, {"FREE", ""},
};
#define RH_DRILLS (sizeof DRILL / sizeof DRILL[0])
#define RH_MAX 9u
#define RH_HIST 16u
enum { RJ_NONE, RJ_GOOD, RJ_OFF, RJ_WRONG };
enum { H_START, H_DITS_LONG, H_DAHS_SHORT, H_DAHS_LONG, H_GAPS_LONG, H_UNEVEN, H_DITS_SHORT, H_GOOD };
static struct {
    uint8_t drill, n, done;
    uint16_t sets, good_sets;
    uint16_t q[RH_MAX], g[RH_MAX];       /* this set: each press and the gap before it, tenths of a dit */
    uint8_t res[RH_MAX], kind[RH_MAX];   /* RJ_*, and dit 0 / dah 1 (wanted, or as judged in FREE) */
    uint16_t hq[RH_HIST];                /* the last presses, tenths of a dit, */
    uint8_t hk[RH_HIST], hn, hi;         /* and dit or dah */
    uint16_t hg[RH_HIST];                /* the last gaps inside a set */
    uint8_t gn, gi;
    uint8_t hint, sugg;                  /* H_*, a send speed to try (0 none) */
    uint16_t last_ms;
} rh;

static uint32_t rh_len(void) { return slen(DRILL[rh.drill].pat); }

static void rhythm_reset(void)
{
    uint8_t d = rh.drill;
    zero(&rh, sizeof rh);
    rh.drill = d;
}

static void rhythm_hint(void)
{
    uint32_t i, dn = 0, an = 0, ds = 0, as = 0, gs = 0, dev = 0, dq, aq, gq, dit = mo_dit_ms(SET.send);
    for (i = 0; i < rh.hn; i++) {
        if (rh.hk[i]) {
            an++;
            as += rh.hq[i];
        } else {
            dn++;
            ds += rh.hq[i];
        }
    }
    for (i = 0; i < rh.gn; i++)
        gs += rh.hg[i];
    dq = dn ? ds / dn : 0;
    aq = an ? as / an : 0;
    gq = rh.gn ? gs / rh.gn : 0;
    for (i = 0; i < rh.hn; i++) {        /* how even: each press's distance from its kind's average, in dit terms */
        uint32_t avg = rh.hk[i] ? aq : dq, v = rh.hq[i];
        dev += (v > avg ? v - avg : avg - v) / (rh.hk[i] ? 3u : 1u);
    }
    rh.sugg = 0;
    if (dn >= 3u && dq > 16u) {
        rh.hint = H_DITS_LONG;
        rh.sugg = (uint8_t)clamp((int32_t)(1200u * 10u / (dq * dit)), 5, 40);
    } else if (an >= 3u && aq < 22u)
        rh.hint = H_DAHS_SHORT;
    else if (an >= 3u && aq > 45u)
        rh.hint = H_DAHS_LONG;
    else if (rh.gn >= 3u && gq > 18u)
        rh.hint = H_GAPS_LONG;
    else if (rh.hn >= 5u && dev * 100u > 10u * 30u * rh.hn)   /* off by more than 0.3 dit on average */
        rh.hint = H_UNEVEN;
    else if (dn >= 3u && dq < 6u) {
        rh.hint = H_DITS_SHORT;
        rh.sugg = (uint8_t)clamp((int32_t)(1200u * 10u / ((dq ? dq : 1u) * dit)), 5, 40);
    } else if (rh.hn >= 5u)
        rh.hint = H_GOOD;
    else
        rh.hint = H_START;
}

static void rhythm_mark(uint32_t mark, uint32_t gap)
{
    uint32_t dit = mo_dit_ms(SET.send), len = rh_len(), q, g, want, res, i;
    q = (mark * 10u + dit / 2u) / dit;
    g = (gap * 10u + dit / 2u) / dit;
    if (rh.done || (rh.n && g > 50u)) {  /* a set done, or a pause: a new set */
        rh.n = 0;
        rh.done = 0;
    }
    if (!len && rh.n >= RH_MAX) {        /* FREE: the last ones, scrolling */
        for (i = 0; i + 1u < RH_MAX; i++) {
            rh.q[i] = rh.q[i + 1u], rh.g[i] = rh.g[i + 1u];
            rh.res[i] = rh.res[i + 1u], rh.kind[i] = rh.kind[i + 1u];
        }
        rh.n = RH_MAX - 1u;
    }
    want = len ? (uint32_t)(DRILL[rh.drill].pat[rh.n] == '-') : q >= 20u;
    if (!len)
        res = q >= 16u && q <= 21u ? RJ_OFF : RJ_GOOD;   /* in between: neither */
    else if (!want)
        res = q >= 5u && q <= 15u ? RJ_GOOD : q <= 20u ? RJ_OFF : RJ_WRONG;
    else
        res = q >= 22u && q <= 40u ? RJ_GOOD : q < 18u ? RJ_WRONG : RJ_OFF;
    rh.q[rh.n] = (uint16_t)(q > 0xFFFFu ? 0xFFFFu : q);
    rh.g[rh.n] = (uint16_t)(rh.n ? (g > 0xFFFFu ? 0xFFFFu : g) : 0u);
    rh.res[rh.n] = (uint8_t)res;
    rh.kind[rh.n] = (uint8_t)want;
    if (rh.n && g <= 50u) {
        rh.hg[rh.gi] = (uint16_t)g;
        rh.gi = (uint8_t)((rh.gi + 1u) % RH_HIST);
        if (rh.gn < RH_HIST)
            rh.gn++;
    }
    rh.hq[rh.hi] = (uint16_t)(q > 0xFFFFu ? 0xFFFFu : q);
    rh.hk[rh.hi] = (uint8_t)want;
    rh.hi = (uint8_t)((rh.hi + 1u) % RH_HIST);
    if (rh.hn < RH_HIST)
        rh.hn++;
    rh.last_ms = (uint16_t)(mark > 0xFFFFu ? 0xFFFFu : mark);
    rh.n++;
    if (len && rh.n >= len) {
        uint32_t good = 1;
        for (i = 0; i < len; i++)
            good &= rh.res[i] == RJ_GOOD;
        rh.done = 1;
        rh.sets++;
        rh.good_sets = (uint16_t)(rh.good_sets + good);
    }
    rhythm_hint();
    ui.redraw |= R_MAIN;
}

/* ------------------------------------------------------------------ input --- */
static void settings_step(int32_t d);
static void listen_reveal(void);

/* TYPE: the character on key k (0 none) on page pg, in the layout set */
static char type_char(uint32_t k, uint32_t pg)
{
    static const char ALPHA_W[2][17] = {"ABCDEFGHIJKLMNOP", "1234567890.,?/=+"};
    static const char ALPHA_B[2][12] = {"QRSTUVWXYZ ", "-()'\":;@!&$"};
    static const char PIANO[2][28] = {"ABCDEFGHIJKLMNOPQRSTUVWXYZ ", "1234567890.,?/=+-()'\":;@!&$"};
    static const char KOCH2[28] = "92H38B?47C1D60X'!()&:;+-\"$@";   /* the Koch order's last 15, then the rest */
    uint32_t i;
    if (k >= NKEYS || pg > 1u)
        return 0;
    switch (tlayout()) {
    case TL_PIANO:
        return PIANO[pg][k];
    case TL_KOCH:
        if (pg)
            return KOCH2[k];
        return k < 26u ? ln_char(k) : ' ';
    default:
        for (i = 0; i < 16u; i++)
            if (WHITE[i] == k)
                return ALPHA_W[pg][i];
        for (i = 0; i < 11u; i++)
            if (BLACK[i] == k)
                return ALPHA_B[pg][i];
        return 0;
    }
}

static const char *type_phrase(uint32_t k)
{
    uint32_t i;
    for (i = 0; i < 16u; i++)
        if (WHITE[i] == k)
            return CT_PHRASE_WHITE[i];
    for (i = 0; i < 11u; i++)
        if (BLACK[i] == k)
            return CT_PHRASE_BLACK[i];
    return 0;
}

static void line_add(const char *s)      /* TYPE: onto the line typed, the oldest dropped */
{
    uint32_t n = slen(ui.line), i;
    for (; *s; s++) {
        if (n >= sizeof ui.line - 1u) {
            for (i = 0; i + 1u < n; i++)
                ui.line[i] = ui.line[i + 1u];
            n--;
        }
        ui.line[n++] = *s;
        ui.line[n] = 0;
    }
}

static void type_key(uint32_t k)         /* TYPE: a key is a letter, or a phrase */
{
    char s[CT_LINE_MAX + 2];
    uint32_t i;
    if (ui.tpage == 2u) {
        const char *p = type_phrase(k);
        if (!p)
            return;
        s[0] = 0;
        text_cat(s, p);
        text_cat(s, " ");
        ui.gloss = ct_gloss(p);
    } else {
        s[0] = type_char(k, ui.tpage);
        s[1] = 0;
        if (!s[0])
            return;
        ui.gloss = 0;
    }
    line_add(s);
    for (i = 0; s[i]; i++)
        txt_add(2, s[i]);
    if (!tcompose()) {                   /* LIVE: each key plays and goes out at once */
        play(&ptx, s);
        send_text(s);
    }
    ui.redraw |= R_MAIN;
}

static void type_play(void)              /* PLAY on TYPE: the line (COMPOSE: sent now), or stop what plays */
{
    uint32_t i, n = slen(ui.line);
    if (playing(&ptx)) {
        play_stop(&ptx);
        return;
    }
    play(&ptx, " ");
    play(&ptx, ui.line);
    if (tcompose())
        for (i = 0; i < n; i += HOSTLINK_MAX)
            send_text(ui.line + i);
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

static void page_play(void)              /* PLAY: start, again, stop */
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
    case PG_LISTEN:
        if (ls.on)
            listen_stop();
        else
            listen_start();
        break;
    case PG_TYPE:
        type_play();
        break;
    case PG_CHART:
        play_char(&ptx, ln_char(ui.csel));
        break;
    case PG_RHYTHM:
        rhythm_reset();
        rhythm_hint();
        ui.redraw |= R_MAIN;
        break;
    case PG_MENU:
        set_page(ui.sel + 1u);
        break;
    default:
        break;
    }
}

static void page_sel(void)               /* SEL: choose, skip, switch */
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
    case PG_LISTEN:
        listen_reveal();
        break;
    case PG_TYPE:
        look_set(6, 1u, !tcompose());
        look_changed();
        app_say(tcompose() ? "COMPOSE" : "LIVE", tcompose() ? "PLAY SENDS THE LINE" : "EACH KEY GOES OUT");
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

#define SET_ITEMS 13u
static void page_select(int32_t e)       /* SELECT turned */
{
    int32_t d = e > 0 ? 1 : -1;
    const ln_card_t *c;
    switch (ui.page) {
    case PG_MENU:
        ui.sel = (uint8_t)wrap(ui.sel, d, PG_N - 1u);
        break;
    case PG_PRACTICE:
        if (pr.ps == PS_CARD && (c = ln_card(&pr.r)) != 0 && ln_choose(c->type))
            pr.pick = (uint8_t)wrap(pr.pick, d, 4u);
        else if (pr.ps == PS_START || pr.ps == PS_DONE) {   /* a round, or a placement test */
            pr.test = (uint8_t)!pr.test;
            if (pr.ps == PS_DONE && pr.r.place)
                pr.ps = PS_START;
            ui.redraw |= R_ALL;
        }
        break;
    case PG_LISTEN:
        SET.prog = (uint8_t)wrap(SET.prog, d, LP_N);
        look_changed();
        if (ls.on)
            listen_start();
        break;
    case PG_RHYTHM:
        rh.drill = (uint8_t)wrap(rh.drill, d, RH_DRILLS);
        rhythm_reset();
        rhythm_hint();
        break;
    case PG_CHART:
        ui.csel = (uint8_t)wrap(ui.csel, d, LN_N);
        break;
    case PG_SET:
        ui.sel = (uint8_t)wrap(ui.sel, d, SET_ITEMS);
        break;
    default:
        break;
    }
    ui.redraw |= R_MAIN | R_FOOT;
}

static void say_speed(const char *what, uint32_t wpm)   /* a speed, and what it means in ms */
{
    char a[24], b[24];
    uint32_t dit = mo_dit_ms(wpm);
    a[0] = 0;
    text_cat(a, what);
    int_cat(a, (int32_t)wpm);
    text_cat(a, " WPM");
    b[0] = 0;
    text_cat(b, "DIT ");
    int_cat(b, (int32_t)dit);
    text_cat(b, "MS DAH ");
    int_cat(b, (int32_t)(3u * dit));
    text_cat(b, "MS");
    app_say(a, b);
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
    say_speed("HEAR ", SET.wpm);
}

static void spacing(int32_t d)
{
    char a[24], b[24];
    int32_t e = clamp((int32_t)SET.eff + d, 5, SET.wpm);
    if (e == SET.eff)
        return;
    SET.eff = (uint8_t)e;
    set_changed();
    a[0] = 0;
    text_cat(a, "SPACING ");
    int_cat(a, e);
    text_cat(a, " WPM");
    b[0] = 0;
    text_cat(b, "LETTER GAP ");
    int_cat(b, (int32_t)(3u * mo_gap_ms(SET.wpm, SET.eff)));
    text_cat(b, "MS");
    app_say(a, b);
}

static void send_speed(int32_t d)
{
    int32_t w = clamp((int32_t)SET.send + d, 5, 40);
    if (w == SET.send)
        return;
    SET.send = (uint8_t)w;
    set_changed();
    say_speed("SEND ", SET.send);
}

static void pitch(int32_t d)
{
    char a[24];
    int32_t p = clamp((int32_t)SET.pitch + d * 10, 300, 1200);
    if (p == SET.pitch)
        return;
    SET.pitch = (uint16_t)p;
    set_changed();
    a[0] = 0;
    text_cat(a, "PITCH ");
    int_cat(a, p);
    text_cat(a, " HZ");
    app_say(a, "");
}

static void mute_toggle(void)
{
    SET.mute ^= 1u;
    set_changed();
    app_say("SOUND", muted() ? "OFF: WATCH THE LIGHTS" : "ON");
}

/* speeds that go together, from a beginner's to a fluent operator's: Koch's characters at 20 WPM or more with the
 * gaps stretched (Farnsworth) to give time to think, and a hand that sends what it can keep even */
static const struct {
    const char *name, *what;
    uint8_t wpm, eff, send;
} PRESET[] = {
    {"START", "HEAR 20/8 SEND 10", 20, 8, 10},
    {"BUILDING", "HEAR 20/13 SEND 13", 20, 13, 13},
    {"QSO", "HEAR 22/18 SEND 18", 22, 18, 18},
    {"FAST", "HEAR 25/25 SEND 22", 25, 25, 22},
};
#define NPRESET (sizeof PRESET / sizeof PRESET[0])

static int preset_now(void)              /* the preset the speeds match, -1 none */
{
    uint32_t i;
    for (i = 0; i < NPRESET; i++)
        if (SET.wpm == PRESET[i].wpm && SET.eff == PRESET[i].eff && SET.send == PRESET[i].send)
            return (int)i;
    return -1;
}

static void preset_step(int32_t d)
{
    int p = preset_now();
    uint32_t i = p < 0 ? (d > 0 ? 0u : NPRESET - 1u) : wrap((uint32_t)p, d, NPRESET);
    SET.wpm = PRESET[i].wpm;
    SET.eff = PRESET[i].eff;
    SET.send = PRESET[i].send;
    set_changed();
    app_say(PRESET[i].name, PRESET[i].what);
}

/* the settings page: SELECT moves, OCT- / OCT+ (or SEL) change */
static const char *const SET_NAME[SET_ITEMS] = {"PRESET", "HEAR SPEED", "SPACING", "SEND SPEED", "PITCH",    "KEYER",
                                                "PADDLES", "SOUND",     "CARDS",   "LEARN BY",   "JUDGING",
                                                "TYPE KEYS", "BLUETOOTH"};
static void settings_step(int32_t d)
{
    switch (ui.sel) {
    case 0: preset_step(d); break;
    case 1: speed(d); break;
    case 2: spacing(d); break;
    case 3: send_speed(d); break;
    case 4: pitch(d); break;
    case 5:
        SET.keyer = (uint8_t)wrap(SET.keyer, d, MO_NKEYER);
        set_changed();
        break;
    case 6:
        SET.swap = (uint8_t)!SET.swap;
        set_changed();
        break;
    case 7: mute_toggle(); break;
    case 8:
        SET.cards = (uint8_t)clamp((int32_t)SET.cards + d * 5, 10, 30);
        set_changed();
        break;
    case 9:
        SET.learn = (uint8_t)!SET.learn;
        set_changed();
        if (visual())
            app_say("TIP", "SOUND FIRST IS FASTER LATER");
        break;
    case 10:
        look_set(7, 1u, !strict());
        look_changed();
        app_say(strict() ? "STRICT" : "BY RHYTHM", strict() ? "AT THE SEND SPEED" : "ANY SPEED, 1:3");
        break;
    case 11:
        look_set(3, 3u, wrap(tlayout(), d, TL_N));
        look_changed();
        break;
    case 12: ble_toggle(); break;
    default:
        break;
    }
}

static void input(void)
{
    uint32_t keys = plat_keys(), btn = plat_buttons(), k;
    int32_t e;
    if (pg.ask) {                        /* a question: PLAY yes, HOME no, nothing else */
        for (k = 0; k < NB; k++)
            if ((btn >> k & 1u) && !(ui.btn >> k & 1u)) {
                if (k == B_PLAY)
                    ask_yes();
                else if (k == B_HOME)
                    ask_no();
            }
        for (k = 0; k < NE; k++)
            (void)plat_enc(k);
        if (btn != ui.btn)
            ui.redraw |= R_FOOT;
        ui.keys = keys;
        ui.btn = btn;
        return;
    }
    for (k = 0; k < NKEYS; k++)
        if ((keys >> k & 1u) && !(ui.keys >> k & 1u))
            page_key(k);
    for (k = 0; k < NB; k++) {
        if (!(btn >> k & 1u) || (ui.btn >> k & 1u))
            continue;
        if (pg.edit && ui.page == PG_CHART) {   /* CHART's edit takes these */
            if (k == B_HOME || k == B_REC || k == B_PLAY) {
                chart_save_or_leave();
                continue;
            }
            if (k == B_FX) {
                ln_step_back(&pg.draft);
                chart_changed("ONE BOX BACK", "ALL COME SOONER");
                continue;
            }
            if (k == B_SEL) {
                pg.sel_t = plat_ms() | 1u;
                continue;
            }
            if (k == B_OCTDN || k == B_OCTUP) {
                chart_level(k == B_OCTUP ? 1 : -1);
                continue;
            }
        }
        switch (k) {
        case B_HOME: set_page(ui.page == PG_MENU ? ui.back : PG_MENU); break;
        case B_FX: mute_toggle(); break;
        case B_PLAY: page_play(); break;
        case B_SEL: page_sel(); break;
        case B_SEQ:                      /* a page's view */
            if (ui.page == PG_KEY)
                look_set(0, 3u, wrap(kview(), 1, KV_N));
            else if (ui.page == PG_TYPE)
                look_set(2, 1u, !tlabel());
            else if (ui.page == PG_LISTEN)
                look_set(5, 1u, !lhide());
            else if (ui.page == PG_PRACTICE && !visual()) {   /* a peek at the code: a new letter, an answer */
                const ln_card_t *pc = ln_card(&pr.r);
                if ((pr.ps == PS_CARD && pc && pc->type == CARD_NEW) || (pr.ps == PS_RESULT && !pr.has_j)) {
                    pr.peek = (uint8_t)!pr.peek;
                    if (pr.ps == PS_RESULT)
                        pr.until = plat_ms() + 2500u;   /* time to look */
                    ui.redraw |= R_MAIN;
                }
                break;
            } else
                break;
            look_changed();
            break;
        case B_REC:                      /* TYPE: the last character off the line (on release; held: all of it) */
            if (ui.page == PG_TYPE)
                ui.rec_t = plat_ms() | 1u;
            else if (ui.page == PG_CHART)   /* CHART: edit the progress */
                chart_edit_on();
            break;
        case B_OCTDN:
        case B_OCTUP:
            if (ui.page == PG_KEY && k == B_OCTDN) {
                grid.n = 0;
                tim.count = 0;
                ui.last = 0;
                ui.redraw |= R_MAIN;
            } else if (ui.page == PG_TYPE) {   /* the next page of keys: letters, signs, phrases */
                ui.tpage = (uint8_t)wrap(ui.tpage, k == B_OCTUP ? 1 : -1, 3u);
                ui.redraw |= R_MAIN | R_FOOT;
            } else if (ui.page == PG_SET)
                settings_step(k == B_OCTUP ? 1 : -1);
            break;
        default: break;
        }
    }
    if (ui.rec_t && ui.page == PG_TYPE) {
        if (!(btn >> B_REC & 1u)) {      /* let go soon: one character */
            uint32_t n = slen(ui.line);
            if (n)
                ui.line[n - 1u] = 0;
            ui.rec_t = 0;
            ui.redraw |= R_MAIN;
        } else if ((int32_t)(plat_ms() - ui.rec_t) >= 600) {   /* held: the whole line */
            ui.line[0] = 0;
            ui.gloss = 0;
            ui.rec_t = 0;
            play_stop(&ptx);
            app_say("LINE CLEARED", "");
            ui.redraw |= R_MAIN;
        }
    } else if (!(btn >> B_REC & 1u))
        ui.rec_t = 0;
    if (pg.sel_t && pg.edit && ui.page == PG_CHART) {   /* SEL in the edit: hear it; held 2 s: all back to new */
        if (!(btn >> B_SEL & 1u)) {
            play_char(&ptx, ln_char(ui.csel));
            pg.sel_t = 0;
        } else if ((int32_t)(plat_ms() - pg.sel_t) >= 2000) {
            ln_init(&pg.draft);
            pg.sel_t = 0;
            chart_changed("ALL RESET", "TILL SAVED");
        }
    } else if (!(btn >> B_SEL & 1u))
        pg.sel_t = 0;
    if ((e = plat_enc(EN_SELECT)) != 0)
        page_select(e);
    if ((e = plat_enc(EN_ALGO)) != 0) {
        if (pg.edit && ui.page == PG_CHART)
            app_say("EDITING", "REC: SAVE OR LEAVE");
        else
            set_page(wrap(ui.page, e > 0 ? 1 : -1, PG_N));
    }
    if (pg.edit && ui.page == PG_CHART) {   /* the edit's knobs: K1 the letter's level, K2 the groups open */
        if ((e = plat_enc(EN_K1)) != 0)
            chart_level(e > 0 ? 1 : -1);
        if ((e = plat_enc(EN_K2)) != 0)
            chart_groups(e > 0 ? 1 : -1);
        (void)plat_enc(EN_K3);
        (void)plat_enc(EN_K4);
    }
    if ((e = plat_enc(EN_K1)) != 0)
        speed(e > 0 ? 1 : -1);
    if ((e = plat_enc(EN_K2)) != 0)
        spacing(e > 0 ? 1 : -1);
    if ((e = plat_enc(EN_K3)) != 0)
        pitch(e > 0 ? 1 : -1);
    if ((e = plat_enc(EN_K4)) != 0)
        send_speed(e > 0 ? 1 : -1);
    (void)plat_enc(EN_PRESET);
    if (btn != ui.btn)
        ui.redraw |= R_FOOT;
    ui.keys = keys;
    ui.btn = btn;
}

void app_input_only(void)
{
    input();
#if FM1B_BLE
    ble_in();
#endif
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
    if (muted())
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
#define C_MEH RGB(230, 200, 60)
#define C_BLUE RGB(70, 140, 255)
#define C_BOX RGB(60, 60, 70)

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

static void draw_code_v(uint8_t code, int32_t cx, int32_t y, int32_t u, uint16_t c)   /* top down, on a key */
{
    uint32_t n = mo_len(code), i;
    for (i = 0; i < n; i++) {
        int32_t h = mo_elem(code, i) ? 3 * u : u;
        cv_rect(cx - u / 2 - 1, y, u + 1, h, c);
        y += h + u;
    }
}

static void text_c(int32_t cx, int32_t y, const felucca_font_t *f, const char *s, uint16_t c)   /* centred */
{
    cv_text(cx - text_w(f, s) / 2, y, f, s, c);
}

static void draw_header(uint32_t now)
{
    static const char *const PAGE[PG_N] = {"MENU", "PRACTICE", "LISTEN", "KEY", "RHYTHM", "TYPE", "CHART", "SETTINGS"};
    char t[24];
    int32_t x;
    cv_begin(240, 32, C_BLACK);
    if (now < ui.msg_until) {
        x = cv_text(8, 7, &FONT_B, ui.msg[0], C_WHITE);
        cv_text(x + 8, 9, &FONT_XS, ui.msg[1], C_RX);
    } else {
        cv_text(8, 7, &FONT_B, ui.page == PG_CHART && pg.edit ? (pg.dirty ? "EDIT *" : "EDIT") : PAGE[ui.page],
                ui.page == PG_CHART && pg.edit ? C_MEH : C_WHITE);
        t[0] = 0;
        if (ui.page == PG_KEY || ui.page == PG_RHYTHM) {   /* the speed this page goes by */
            text_cat(t, "SEND ");
            int_cat(t, SET.send);
        } else {
            int_cat(t, SET.wpm);
            if (SET.eff < SET.wpm) {
                text_cat(t, "/");
                int_cat(t, SET.eff);
            }
        }
        text_cat(t, " WPM");
        x = 232 - text_w(&FONT_XS, t);
        cv_text(x, 10, &FONT_XS, t, C_GRAY);
        if (muted()) {
            x -= 8 + text_w(&FONT_XS, "MUTE");
            cv_text(x, 10, &FONT_XS, "MUTE", C_RX);
        }
        if (hl_host(&link, now)) {
            x -= 8 + text_w(&FONT_XS, "PC");
            cv_text(x, 10, &FONT_XS, "PC", C_BLUE);
        }
        if (FM1B_BLE && !bt_off()) {    /* grey while it waits for a computer, blue once one is connected */
            x -= 8 + text_w(&FONT_XS, "BT");
            cv_text(x, 10, &FONT_XS, "BT", plat_ble_connected() ? C_BLUE : C_DIM);
        }
    }
    cv_rect(0, 31, 240, 1, C_LINE);
    cv_blit(0, 0);
}

static void draw_lamp(int32_t y)         /* the keying as a light: white sending, amber receiving */
{
    int tx = tone_gated(TONE_TX), rx = tone_gated(TONE_RX);
    cv_begin(240, 10, C_BLACK);
    cv_rect(20, 2, 200, 6, tx ? C_OWN : rx ? C_RX : C_LINE);
    cv_blit(0, y);
}

/* KEY: the top strip (the letter so far, the last one read, how it is going), then one of three views */
static void key_top(void)
{
    char t[40];
    uint8_t pend = mo_dec_pending(&dsrc[SRC_OWN].dec);
    const mo_dec_t *d = &dsrc[SRC_OWN].dec;
    cv_begin(240, 56, C_BLACK);
    if ((visual() || kview() != KV_TEXT) && pend > 1u)
        draw_code(pend, 90, 22, 8, C_OWN);
    if (ui.last) {
        t[0] = ui.last, t[1] = 0;
        text_c(200, 6, &FONT_L, t, C_OWN);
    }
    if (d->marks >= 4u) {                /* how it is going: speed, dah:dit (3.0 the book), gaps inside (1.0) */
        uint32_t q = mo_dec_ratio10(d), g = mo_dec_gap10(d);
        t[0] = 0;
        text_cat(t, "YOU ");
        int_cat(t, (int32_t)mo_dec_wpm(d));
        text_cat(t, " WPM");
        if (q) {
            text_cat(t, "  DAH:DIT ");
            tenths_cat(t, q);
        }
        if (g) {
            text_cat(t, "  GAP ");
            tenths_cat(t, g);
        }
        cv_text(8, 40, &FONT_XS, t, g > 18u || (q && (q < 22u || q > 42u)) ? C_RX : C_GRAY);
    }
    cv_blit(0, 32);
}

static void key_text(void)
{
    char t[2] = {0, 0};
    uint32_t r, i;
    cv_begin(240, 110, C_BLACK);
    for (r = 0; r < TG_ROWS; r++)
        for (i = 0; i < TG_COLS; i++) {
            uint32_t k = r * TG_COLS + i;
            if (k < grid.n) {
                t[0] = grid.c[k];
                cv_text(4 + (int32_t)i * 10, 2 + (int32_t)r * 22, &FONT_S, t, grid.who[k] == SRC_RX ? C_RX : C_OWN);
            }
        }
    if (!grid.n)
        cv_text(8, 40, &FONT_XS, keyer.mode == MO_STRAIGHT ? "any key is the key" : "left half dit, right half dah",
                C_DIM);
    cv_blit(0, 98);
}

#define CU 4                             /* the CODE view: px a unit */
static void key_code(void)               /* each letter over what was keyed for it */
{
    static uint8_t rows[TG_N];
    static int16_t xs[TG_N];
    char t[2] = {0, 0};
    int32_t x = 4, row = 0, first;
    uint32_t i;
    for (i = 0; i < grid.n; i++) {       /* lay them out: a letter is as wide as its code */
        int32_t w;
        if (grid.c[i] == ' ') {
            rows[i] = (uint8_t)row, xs[i] = (int16_t)x;
            x += 10;
            continue;
        }
        t[0] = grid.c[i];
        w = text_w(&FONT_B, t);
        if (grid.pat[i] > 1u && (int32_t)mo_units(grid.pat[i]) * CU > w)
            w = (int32_t)mo_units(grid.pat[i]) * CU;
        if (x + w > 236 && x > 4) {
            row++;
            x = 4;
        }
        rows[i] = (uint8_t)row, xs[i] = (int16_t)x;
        x += w + 8;
    }
    first = row > 3 ? row - 3 : 0;
    cv_begin(240, 110, C_BLACK);
    for (i = 0; i < grid.n; i++) {
        int32_t y = 2 + ((int32_t)rows[i] - first) * 27;
        uint16_t c = grid.who[i] == SRC_RX ? C_RX : grid.c[i] == MO_UNKNOWN ? C_BAD : C_OWN;
        if ((int32_t)rows[i] < first || grid.c[i] == ' ')
            continue;
        t[0] = grid.c[i];
        cv_text(xs[i], y, &FONT_B, t, c);
        if (grid.pat[i] > 1u) {
            uint32_t n = mo_len(grid.pat[i]), k;
            int32_t px = xs[i];
            for (k = 0; k < n; k++) {
                int32_t w = mo_elem(grid.pat[i], k) ? 3 * CU : CU;
                cv_rect(px, y + 19, w, CU, c);
                px += w + CU;
            }
        }
    }
    if (!grid.n)
        cv_text(8, 40, &FONT_XS, "each letter over its dits and dahs", C_DIM);
    cv_blit(0, 98);
}

#define TU 8                             /* the TIMING view and RHYTHM: px a dit (of the send speed) */
static void tim_row(const trow_t *r, int32_t y, int live)
{
    char t[2] = {0, 0};
    uint32_t dit = mo_dit_ms(SET.send), i;
    int32_t x = 28;
    t[0] = live ? '_' : r->c;
    cv_text(6, y, &FONT_B, t, r->c == MO_UNKNOWN ? C_BAD : live ? C_DIM : C_OWN);
    for (i = 0; i < r->n && x < 240; i++) {
        int32_t w;
        int dah = live ? mo_dec_is_dah(&dsrc[SRC_OWN].dec, r->mark[i]) : (int)(r->dah >> i & 1u);
        if (i) {
            int32_t g = (int32_t)(r->gap[i] * TU / dit);
            if (r->gap[i] * 10u > dit * 16u)   /* a long gap inside a letter: where it gets split */
                cv_rect(x, y + 13, g, 2, C_BAD);
            x += g;
        }
        w = (int32_t)(r->mark[i] * TU / dit);
        if (w < 2)
            w = 2;
        if (x + w > 240)
            w = 240 - x;
        cv_rect(x, y + 4, w, 8, dah ? C_RX : C_OWN);
        x += w;
    }
}

static void key_timing(void)             /* each letter as keyed, to scale: is a dit a dit? */
{
    char t[32];
    uint32_t i, dit = mo_dit_ms(SET.send);
    int32_t y = 14;
    cv_begin(240, 110, C_BLACK);
    cv_rect(28, 2, TU, 3, C_GRAY);       /* the ruler: a dit and a dah at the send speed */
    cv_rect(28 + 2 * TU, 2, 3 * TU, 3, C_GRAY);
    t[0] = 0;
    text_cat(t, "DIT ");
    int_cat(t, (int32_t)dit);
    text_cat(t, "  DAH ");
    int_cat(t, (int32_t)(3u * dit));
    text_cat(t, " MS");
    cv_text(96, 0, &FONT_T, t, C_GRAY);
    for (i = 0; i < tim.count; i++) {    /* oldest first */
        const trow_t *r = &tim.row[(tim.head + TR_N - tim.count + i) % TR_N];
        tim_row(r, y, 0);
        y += 19;
    }
    if (tim.live.n)
        tim_row(&tim.live, y, 1);
    if (!tim.count && !tim.live.n)
        cv_text(8, 40, &FONT_XS, "red: a gap that splits a letter", C_DIM);
    cv_blit(0, 98);
}

static void key_page(void)
{
    key_top();
    draw_lamp(88);
    if (kview() == KV_CODE)
        key_code();
    else if (kview() == KV_TIMING)
        key_timing();
    else
        key_text();
}

/* TYPE: the line typed over a keyboard of 16 white keys 15 px each, the black ones over their gaps. With the code
 * on the keys the keyboard grows (126 px instead of 86) and the line shrinks to its last row, so each key holds its
 * code down the key and its letter under it: the longest, 0 ----- and , --..--, are 38 px at 2 px a unit */
static int32_t type_lamp_y(void) { return tlabel() ? 72 : 112; }

static void type_page(void)
{
    static const char *const PAGE[3] = {"LETTERS", "DIGITS + SIGNS", "PHRASES"};
    char t[8];
    uint32_t i, n = slen(ui.line), first = 0;
    char cur = ptx.cur;
    int code = (int)tlabel();
    int32_t top_h = code ? 40 : 80, kb_h = code ? 126 : 86, wh = kb_h - 2, bh = code ? 62 : 46, ly = code ? 24 : 64;
    cv_begin(240, top_h, C_BLACK);
    if (code && n > TG_COLS)
        first = n - TG_COLS;
    for (i = first; i < n; i++) {
        uint32_t k = i - first;
        t[0] = ui.line[i] == ' ' ? '_' : ui.line[i];
        t[1] = 0;
        cv_text(4 + (int32_t)(k % TG_COLS) * 10, 2 + (int32_t)(k / TG_COLS) * 20, &FONT_S, t,
                ui.line[i] == ' ' ? C_DIM : C_OWN);
    }
    if (ui.tpage == 2u && ui.gloss)
        cv_text(4, code ? ly : 46, &FONT_XS, ui.gloss, C_RX);
    else if (cur && !code && visual())
        draw_code(mo_code(cur), 120, 52, 6, C_RX);
    if (!code || !(ui.tpage == 2u && ui.gloss)) {
        int32_t x = cv_text(4, ly, &FONT_T, PAGE[ui.tpage], C_GRAY);
        cv_text(x + 12, ly, &FONT_T, tcompose() ? "COMPOSE: PLAY SENDS" : "LIVE: EACH KEY GOES OUT",
                tcompose() ? C_RX : C_DIM);
    }
    cv_blit(0, 32);
    draw_lamp(type_lamp_y());
    cv_begin(240, kb_h, C_BLACK);
    for (i = 0; i < 16u; i++) {
        uint32_t k = WHITE[i];
        int32_t x = (int32_t)i * 15;
        char c = ui.tpage < 2u ? type_char(k, ui.tpage) : 0;
        cv_rect(x, 0, 14, wh, c && c == cur ? C_RX : RGB(200, 200, 206));
        if (ui.tpage == 2u) {            /* a phrase: its name down the key */
            const char *p = type_phrase(k);
            uint32_t j, m = 0;
            for (j = 0; p && p[j] && m < 4u; j++)
                if (p[j] != '<' && p[j] != '>' && p[j] != '?') {
                    t[0] = p[j], t[1] = 0;
                    text_c(x + 7, wh - 36 + (int32_t)m * 9, &FONT_T, t, C_BLACK);
                    m++;
                }
        } else {
            t[0] = c, t[1] = 0;
            if (code && c && c != ' ')   /* under the black keys' reach (they end at 62) */
                draw_code_v(mo_code(c), x + 7, 64, 2, C_BLACK);
            text_c(x + 7, wh - (code ? 18 : 16), &FONT_XS, c == ' ' ? "_" : t, C_BLACK);
        }
    }
    for (i = 0; i < 11u; i++) {
        uint32_t w = 0, k = BLACK[i];
        int32_t x;
        char c = ui.tpage < 2u ? type_char(k, ui.tpage) : 0;
        while (w < 16u && WHITE[w] < k)
            w++;
        x = (int32_t)w * 15;
        cv_rect(x - 6, 0, 12, bh, c && c == cur ? C_RX : RGB(40, 40, 46));
        if (ui.tpage == 2u) {
            const char *p = type_phrase(k);
            uint32_t j, m = 0;
            for (j = 0; p && p[j] && m < 3u; j++)
                if (p[j] != '<' && p[j] != '>' && p[j] != '?') {
                    t[0] = p[j], t[1] = 0;
                    text_c(x, 4 + (int32_t)m * 9, &FONT_T, t, C_WHITE);
                    m++;
                }
        } else {
            t[0] = c == ' ' ? '_' : c, t[1] = 0;
            if (code && c && c != ' ')   /* the code down the key, the letter under it */
                draw_code_v(mo_code(c), x, 3, 2, C_WHITE);
            text_c(x, code ? bh - 17 : 26, &FONT_XS, t, C_WHITE);
        }
    }
    cv_blit(0, type_lamp_y() + 10);
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

/* PRACTICE's result for a keyed answer: the grade, then the character as it goes (SHOULD, at the answer's own dit)
 * over the answer as keyed (YOU), to one scale, and what to work on */
static const char *const GRADE[4] = {"GREAT", "GOOD", "CLOSE", "OFF"};
static uint32_t grade_of(uint32_t score) { return score >= 85u ? 0u : score >= 65u ? 1u : score >= 40u ? 2u : 3u; }
static const char *const HINT[MH_NHINTS] = {
    "clean rhythm",
    "dahs short: hold a dah for 3 dits",
    "dits long: tap a dit short",
    "a mark too many",
    "a mark missing, or the letter split",
    "gaps inside long: run it together",
    "keep marks of a kind the same length",
    "dahs long: 3 dits is enough",
};

static void bars(const uint16_t *mark, const uint16_t *gap, uint32_t n, uint8_t want, uint32_t dit, int32_t y,
                 uint32_t den, int you)
{
    int32_t x = 52;
    uint32_t i;
    for (i = 0; i < n; i++) {
        int32_t w;
        int dah = want && mo_len(want) == n ? mo_elem(want, i) : mark[i] >= 2u * dit;
        uint16_t c = dah ? C_RX : C_OWN;
        if (i) {
            int32_t g = (int32_t)(gap[i] * 180u / den);
            if (you && gap[i] * 10u >= dit * 25u)
                cv_rect(x, y + 10, g, 2, C_BAD);   /* a gap a listener takes for a letter's end */
            x += g;
        }
        w = (int32_t)(mark[i] * 180u / den);
        if (w < 2)
            w = 2;
        if (x + w > 236)
            w = 236 - x > 2 ? 236 - x : 2;
        if (you && want && mo_len(want) == n && pr.res != LN_RIGHT) {
            /* the marks on the wrong side of the line between this answer's dits and dahs */
            int looks_dah = mark[i] * 10u >= dit * 20u;
            if (looks_dah != dah)
                c = C_BAD;
        }
        cv_rect(x, y + 2, w, 8, c);
        x += w;
    }
}

static void result_keyed(const ln_card_t *a, int32_t oy)
{
    char t[48];
    uint8_t want = mo_code(ln_char(a->ch));
    uint32_t n = mo_len(want), i, dit = pr.j.dit ? pr.j.dit : 100u, sh = 0, yo = 0, den, g = grade_of(pr.j.score);
    uint16_t sm[8], sg[8];
    for (i = 0; i < n && i < 8u; i++) {  /* how it goes, at this answer's speed */
        sm[i] = (uint16_t)(mo_elem(want, i) ? 3u * dit : dit);
        sg[i] = (uint16_t)(i ? dit : 0u);
        sh += sm[i] + sg[i];
    }
    for (i = 0; i < pr.an; i++)
        yo += pr.am[i] + pr.ag[i];
    den = sh > yo ? sh : yo;
    if (!den)
        den = 1;
    t[0] = 0;
    text_cat(t, GRADE[g]);
    text_cat(t, " ");
    int_cat(t, pr.j.score);
    cv_text(8, 62 - oy, &FONT_B, t, g == 0u ? C_OK : g == 1u ? C_OK : g == 2u ? C_MEH : C_BAD);
    if (pr.read[0]) {
        t[0] = 0;
        text_cat(t, "READ AS ");
        text_cat(t, pr.read);
        cv_text(232 - text_w(&FONT_XS, t), 66 - oy, &FONT_XS, t, C_GRAY);
    }
    cv_text(4, 88 - oy, &FONT_T, "SHOULD", C_DIM);
    bars(sm, sg, n, want, dit, 86 - oy, den, 0);
    cv_text(4, 108 - oy, &FONT_T, "YOU", C_DIM);
    bars(pr.am, pr.ag, pr.an, want, dit, 106 - oy, den, 1);
    cv_text(8, 128 - oy, &FONT_XS, HINT[pr.j.hint], pr.j.hint == MH_NONE ? C_OK : C_GRAY);
    if (pr.res == LN_ALMOST)
        cv_text(8, 146 - oy, &FONT_XS, "close: no box moved, asked again", C_DIM);
}

static void practice_at(int32_t oy)      /* drawn twice, each half with the rows above it taken off */
{
    char t[40];
    const ln_card_t *c = ln_card(&pr.r);
    const ln_state_t *s = &rec.ln;
    if (pr.ps == PS_DONE && pr.r.place) {   /* a placement test over */
        uint32_t i, k = 0;
        text_c(120, 8 - oy, &FONT_B, "TEST DONE", C_WHITE);
        t[0] = 0;
        int_cat(t, (int32_t)ln_place_count(&pr.r));
        text_cat(t, " OF ");
        int_cat(t, pr.r.n);
        text_cat(t, " KNOWN");
        text_c(120, 34 - oy, &FONT_XS, t, C_GRAY);
        t[0] = 0;
        for (i = 0; i < LN_N && k < 12u; i++)
            if (pr.r.known[i / 32u] >> (i % 32u) & 1u) {
                char c2[3] = {ln_char(i), ' ', 0};
                text_cat(t, c2);
                k++;
            }
        text_c(120, 56 - oy, &FONT_B, t, C_OK);
        text_c(120, 96 - oy, &FONT_B, "PLAY: START", C_RX);
        text_c(120, 120 - oy, &FONT_XS, "SELECT: a round or a test", C_DIM);
        return;
    }
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
        if (s->open < LN_N) {            /* what keeps the next group shut */
            uint32_t i, k = 0;
            t[0] = 0;
            text_cat(t, "NEXT GROUP WAITS ON ");
            for (i = 0; i < s->open; i++)
                if (s->box[i][LN_REC] < 2u || s->box[i][LN_SEND] < 1u) {
                    char c2[3] = {ln_char(i), ' ', 0};
                    if (k++ < 8u)
                        text_cat(t, c2);
                }
            if (k)
                text_c(120, y + 18 - oy, &FONT_T, t, C_GRAY);
        }
        if (pr.test) {
            text_c(120, y + 40 - oy, &FONT_B, "PLAY: TEST", C_RX);
            text_c(120, y + 64 - oy, &FONT_XS, "the next 10 letters: skip the ones you know", C_DIM);
        } else {
            text_c(120, y + 40 - oy, &FONT_B, "PLAY: START", C_RX);
            text_c(120, y + 64 - oy, &FONT_XS, visual() ? "learning by sight" : "learning by sound", C_DIM);
        }
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
        static const char *const RES[3] = {"NO", "RIGHT", "ALMOST"};
        uint16_t rc = pr.res == LN_RIGHT ? C_OK : pr.res == LN_ALMOST ? C_MEH : C_BAD;
        if (pr.has_j) {                  /* a keyed answer: how it went, against how it goes */
            cv_text(8, 16 - oy, &FONT_L, RES[pr.res], rc);
            t[0] = ch, t[1] = 0;
            cv_text(232 - text_w(&FONT_L, t), 16 - oy, &FONT_L, t, C_WHITE);
            result_keyed(a, oy);
            return;
        }
        text_c(120, 18 - oy, &FONT_L, RES[pr.res], rc);
        t[0] = ch, t[1] = 0;
        text_c(120, 64 - oy, &FONT_L, t, C_WHITE);
        if (visual() || pr.peek)
            draw_code(mo_code(ch), 120, 108 - oy, 8, C_GRAY);
        if (!pr.right && pr.got) {
            t[0] = 0;
            text_cat(t, "YOU: ");
            t[5] = pr.got, t[6] = 0;
            text_c(120, 126 - oy, &FONT_XS, t, C_BAD);
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
            if (visual() || pr.peek)
                draw_code(mo_code(ch), 120, 100 - oy, 8, C_GRAY);
            text_c(120, 128 - oy, &FONT_XS, muted() ? "watch it, then a key" : "listen, then any key", C_DIM);
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
            if ((visual() || kview() != KV_TEXT) && pend > 1u)
                draw_code(pend, 120, 112 - oy, 8, C_OWN);
            text_c(120, 136 - oy, &FONT_XS, pr.answer_ok ? "your turn" : "listen", pr.answer_ok ? C_OK : C_DIM);
        }
    }
}

static void listen_reveal(void)          /* SEL while hidden: the line playing, for a moment */
{
    if (ls.on && lhide())
        app_say("NOW:", ls.l[ls.cur].t);
}

/* s in cells of cw px, cols a row; the first `bright` playable characters lit, the rest dim (hidden: not shown); the
 * rows that hold the lit part kept in view */
static void wrap_text(const char *s, int32_t x, int32_t y, const felucca_font_t *f, int32_t cw, uint32_t cols,
                      uint32_t rows, uint32_t bright, int hide, int32_t oy)
{
    char t[2] = {0, 0};
    uint32_t i, k = 0, n = slen(s), at = 0, first = 0, lit_n = 0;
    for (i = 0; i < n; i++)
        if (k < bright && playable(s[i]))
            k++, at = i;
    if (at / cols >= rows)
        first = at / cols - rows + 1u;
    for (i = 0; i < n && i < (first + rows) * cols; i++) {
        uint32_t lit;
        lit_n += (uint32_t)playable(s[i]);
        lit = lit_n <= bright && s[i] != ' ';
        if (i < first * cols || (hide && !lit))
            continue;
        t[0] = hide ? '_' : s[i];
        cv_text(x + (int32_t)(i % cols) * cw, y + (int32_t)(i / cols - first) * 20 - oy, f, t, lit ? C_WHITE : C_DIM);
    }
}

static void listen_at(int32_t oy)
{
    char t[40];
    cv_text(8, 2 - oy, &FONT_B, LP_NAME[SET.prog], C_WHITE);
    t[0] = 0;
    if (ls.count) {
        int_cat(t, (int32_t)ls.count);
        text_cat(t, " LINES");
    }
    cv_text(232 - text_w(&FONT_XS, t), 4 - oy, &FONT_XS, t, C_DIM);
    if (!ls.on) {
        text_c(120, 50 - oy, &FONT_B, "PLAY: START", C_RX);
        text_c(120, 76 - oy, &FONT_XS, "SELECT: the program", C_DIM);
        text_c(120, 94 - oy, &FONT_XS, lhide() ? "hidden: copy it in your head" : "the text shown as it plays",
               C_DIM);
        return;
    }
    wrap_text(ls.l[ls.cur].t, 4, 28, &FONT_S, 10, TG_COLS, 3, ls.l[ls.cur].played, (int)lhide(), oy);
    if (!lhide() && ls.l[ls.cur].gloss)
        cv_text(4, 92 - oy, &FONT_XS, ls.l[ls.cur].gloss, C_RX);
    if (ls.prev[0]) {
        cv_text(4, 112 - oy, &FONT_T, lhide() ? "IT WAS:" : "BEFORE:", C_GRAY);
        wrap_text(ls.prev, 4, 126, &FONT_XS, 8, 29, 2, 0xFF, 0, oy);
        if (ls.prev_gloss)
            cv_text(120, 112 - oy, &FONT_T, ls.prev_gloss, C_RX);
    }
}

static void rhythm_at(int32_t oy)
{
    static const char *const H1[] = {"PRESS ANY KEY",        "DITS ARE LONG", "DAHS ARE SHORT",      "DAHS ARE LONG",
                                     "GAPS INSIDE ARE LONG", "LENGTHS VARY",  "DITS ARE VERY SHORT", "GOOD RHYTHM"};
    static const char *const H2[] = {"dit short, dah three dits", "tap and let go",
                                     "hold a dah about 3 dits",   "let go sooner: 3 dits",
                                     "one dit between, no more",  "slow down, keep it even",
                                     "fine if even",              "next drill, or K4 faster"};
    static const char *const WHY_DIT[] = {"", "GOOD", "A BIT LONG", "TOO LONG: A DAH"};
    static const char *const WHY_DAH[] = {"", "GOOD", "A BIT OFF", "TOO SHORT: A DIT"};
    char t[48];
    uint32_t len = rh_len(), i, n = len ? len : rh.n, dit = mo_dit_ms(SET.send);
    int32_t x = 8;
    cv_text(8, 0 - oy, &FONT_B, DRILL[rh.drill].name, C_WHITE);
    t[0] = 0;
    if (len) {
        text_cat(t, "SETS ");
        int_cat(t, rh.good_sets);
        text_cat(t, "/");
        int_cat(t, rh.sets);
        text_cat(t, " CLEAN");
    }
    cv_text(232 - text_w(&FONT_XS, t), 2 - oy, &FONT_XS, t, C_DIM);
    /* the set: a box for each target, the bar pressed over it */
    for (i = 0; i < n && i < RH_MAX; i++) {
        uint32_t want = len ? (uint32_t)(DRILL[rh.drill].pat[i] == '-') : rh.kind[i];
        int32_t tw = want ? 3 * TU : TU, bw;
        uint16_t c = rh.res[i] == RJ_GOOD ? C_OK : rh.res[i] == RJ_OFF ? C_MEH : C_BAD;
        cv_rect(x, 26 - oy, tw, 14, C_BOX);
        if (i < rh.n) {
            bw = (int32_t)(rh.q[i] * TU / 10u);
            if (bw < 2)
                bw = 2;
            if (bw > tw + 2 * TU)
                bw = tw + 2 * TU;
            cv_rect(x, 29 - oy, bw, 8, c);
        } else if (i == rh.n && !rh.done)
            cv_rect(x, 42 - oy, tw, 2, C_WHITE);   /* the next one */
        x += tw + TU + (len ? 0 : TU);
    }
    if (rh.n) {                          /* the last press, in dits of the send speed */
        uint32_t k = rh.n - 1u, q = rh.q[k];
        uint16_t c = rh.res[k] == RJ_GOOD ? C_OK : rh.res[k] == RJ_OFF ? C_MEH : C_BAD;
        t[0] = 0;
        text_cat(t, rh.kind[k] ? "DAH " : "DIT ");
        tenths_cat(t, q);
        x = cv_text(8, 50 - oy, &FONT_M, t, C_WHITE);
        t[0] = 0;
        int_cat(t, rh.last_ms);
        text_cat(t, " MS");
        cv_text(x + 8, 58 - oy, &FONT_XS, t, C_GRAY);
        if (len)
            cv_text(8, 80 - oy, &FONT_XS, rh.kind[k] ? WHY_DAH[rh.res[k]] : WHY_DIT[rh.res[k]], c);
        else if (rh.res[k] == RJ_OFF)
            cv_text(8, 80 - oy, &FONT_XS, "IN BETWEEN: NEITHER", C_MEH);
        if (k && rh.g[k] > 18u) {
            t[0] = 0;
            text_cat(t, "GAP BEFORE IT ");
            tenths_cat(t, rh.g[k]);
            cv_text(130, 80 - oy, &FONT_XS, t, C_RX);
        }
    } else {
        t[0] = 0;
        text_cat(t, "TARGET: DIT ");
        int_cat(t, (int32_t)dit);
        text_cat(t, " MS, DAH ");
        int_cat(t, (int32_t)(3u * dit));
        text_cat(t, " MS");
        cv_text(8, 56 - oy, &FONT_XS, t, C_GRAY);
        cv_text(8, 76 - oy, &FONT_XS, "K4 changes it (the send speed)", C_DIM);
    }
    cv_text(8, 106 - oy, &FONT_B, H1[rh.hint], rh.hint == H_GOOD ? C_OK : rh.hint == H_START ? C_GRAY : C_RX);
    t[0] = 0;
    text_cat(t, H2[rh.hint]);
    if (rh.sugg && rh.sugg != SET.send) {
        text_cat(t, ", or K4 to ");
        int_cat(t, rh.sugg);
    }
    cv_text(8, 126 - oy, &FONT_XS, t, C_GRAY);
}

static void chart_at(int32_t oy)         /* the course like a kana chart: 7 x 6, coloured by box */
{
    char t[2] = {0, 0};
    uint32_t i;
    for (i = 0; i < LN_N; i++) {
        int32_t x = 2 + (int32_t)(i % 7u) * 34, y = 2 + (int32_t)(i / 7u) * 28 - oy;
        const ln_state_t *st = chart_state();
        int open = i < st->open;
        uint16_t bg = open ? level_rgb(ln_level(st, i)) : RGB(20, 20, 24);
        if (i == ui.csel)
            cv_rect(x - 2, y - 2, 36, 30, pg.edit ? C_MEH : C_WHITE);
        cv_rect(x, y, 32, 26, bg);
        t[0] = ln_char(i);
        text_c(x + 16, y + 5, &FONT_B, t, open ? C_WHITE : C_DIM);
    }
}

static void set_at(int32_t oy)
{
    static const char *const KEYER[MO_NKEYER] = {"STRAIGHT", "IAMBIC A", "IAMBIC B"};
    static const char *const LAYOUT[TL_N] = {"A-P WHITE, Q-Z BLACK", "A-Z LEFT TO RIGHT", "KOCH ORDER"};
    char t[32];
    uint32_t i;
    int32_t top = ui.sel > 11u ? (int32_t)(ui.sel - 11u) * 14 : 0;   /* twelve rows fit: scrolled for the rest */
    for (i = 0; i < SET_ITEMS; i++) {
        int32_t y = 1 + (int32_t)i * 14 - oy - top;
        if (i == ui.sel)
            cv_rect(0, y - 1, 240, 14, C_LINE);
        cv_text(8, y, &FONT_XS, SET_NAME[i], C_GRAY);
        t[0] = 0;
        switch (i) {
        case 0: {
            int p = preset_now();
            text_cat(t, p < 0 ? "CUSTOM" : PRESET[p].name);
            if (p == 0)
                text_cat(t, " (TO BEGIN)");
            break;
        }
        case 1:
        case 3:
            int_cat(t, i == 3 ? SET.send : SET.wpm);
            text_cat(t, " WPM  DIT ");
            int_cat(t, (int32_t)mo_dit_ms(i == 3 ? SET.send : SET.wpm));
            text_cat(t, " MS");
            break;
        case 2:
            int_cat(t, SET.eff);
            text_cat(t, SET.eff < SET.wpm ? " WPM (FARNSWORTH)" : " WPM");
            break;
        case 4: int_cat(t, SET.pitch); text_cat(t, " HZ"); break;
        case 5: text_cat(t, KEYER[SET.keyer]); break;
        case 6: text_cat(t, SET.swap ? "LEFT DAH" : "LEFT DIT"); break;
        case 7: text_cat(t, muted() ? "OFF (LIGHTS)" : "ON"); break;
        case 8: int_cat(t, SET.cards); text_cat(t, " A ROUND"); break;
        case 9: text_cat(t, visual() ? "SIGHT" : "SOUND (BEST)"); break;
        case 10: text_cat(t, strict() ? "STRICT (SEND SPEED)" : "BY RHYTHM (EASY)"); break;
        case 11: text_cat(t, LAYOUT[tlayout()]); break;
        case 12:
            text_cat(t, !FM1B_BLE ? "NOT IN THIS BUILD" : bt_off() ? "OFF" : !plat_ble_connected() ? "ON (NOT CONNECTED)"
                        : hl_host(&link, plat_ms()) && link.transport == HL_BLE ? "ON (TALKING)" : "ON (CONNECTED)");
            break;
        default: break;
        }
        cv_text(96, y, &FONT_XS, t, i == ui.sel ? C_WHITE : C_RX);
    }
}

static void menu_at(int32_t oy)
{
    static const char *const NAME[PG_N - 1] = {"PRACTICE", "LISTEN", "KEY", "RHYTHM", "TYPE", "CHART", "SETTINGS"};
    static const char *const WHAT[PG_N - 1] = {"flash cards",    "play on and on",   "send, it reads you",
                                               "short and long", "keys are letters", "how far you are",
                                               "speeds, keyer, ..."};
    uint32_t i;
    for (i = 0; i < PG_N - 1u; i++) {
        int32_t y = 4 + (int32_t)i * 24 - oy;
        if (i == ui.sel)
            cv_rect(0, y - 3, 240, 23, C_LINE);
        cv_text(12, y, &FONT_B, NAME[i], i == ui.sel ? C_WHITE : C_GRAY);
        cv_text(112, y + 2, &FONT_XS, WHAT[i], C_DIM);
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

static void ask_at(int32_t oy)           /* a question about the progress */
{
    static const char *const Q[4] = {"", "SAVE CHANGES?", "LOAD THIS BACKUP?", "MARK THESE KNOWN?"};
    const ln_state_t *s = pg.ask == ASK_PLACE ? &rec.ln : &pg.draft;
    char t[48];
    uint32_t i, k = 0;
    cv_rect(8, 8 - oy, 224, 150, RGB(24, 24, 30));
    text_c(120, 18 - oy, &FONT_B, Q[pg.ask], C_WHITE);
    t[0] = 0;
    if (pg.ask == ASK_PLACE) {
        for (i = 0; i < LN_N && k < 12u; i++)
            if (pr.r.known[i / 32u] >> (i % 32u) & 1u) {
                char c2[3] = {ln_char(i), ' ', 0};
                text_cat(t, c2);
                k++;
            }
        text_c(120, 48 - oy, &FONT_B, t, C_OK);
        text_c(120, 74 - oy, &FONT_XS, "whole groups right open", C_GRAY);
    } else {
        text_cat(t, "ROUND ");
        int_cat(t, s->round);
        text_cat(t, "  ");
        int_cat(t, s->open);
        text_cat(t, " OPEN");
        text_c(120, 48 - oy, &FONT_XS, t, C_GRAY);
        t[0] = 0;
        int_cat(t, (int32_t)s->answered);
        text_cat(t, " ANSWERED, ");
        int_cat(t, (int32_t)s->right);
        text_cat(t, " RIGHT");
        text_c(120, 66 - oy, &FONT_XS, t, C_GRAY);
        text_c(120, 84 - oy, &FONT_XS, pg.ask == ASK_LOAD ? "replaces the progress here" : "the progress as edited",
               C_DIM);
    }
    text_c(70, 116 - oy, &FONT_B, "PLAY: YES", C_OK);
    text_c(176, 116 - oy, &FONT_B, "HOME: NO", C_BAD);
}

static void draw_main(void)
{
    if (pg.ask) {
        main_area(ask_at);
        return;
    }
    switch (ui.page) {
    case PG_KEY: key_page(); break;
    case PG_TYPE: type_page(); break;
    case PG_PRACTICE: main_area(practice_at); break;
    case PG_LISTEN: main_area(listen_at); break;
    case PG_RHYTHM: main_area(rhythm_at); break;
    case PG_CHART: main_area(chart_at); break;
    case PG_SET: main_area(set_at); break;
    default: main_area(menu_at); break;
    }
}

static void draw_footer(void)
{
    static const char *const KV_NAME[KV_N] = {"SEQ: TEXT", "SEQ: CODE", "SEQ: TIMING"};
    const char *s = "";
    char t[64];
    const ln_card_t *c = ln_card(&pr.r);
    uint32_t i;
    cv_begin(240, 32, C_BLACK);
    cv_rect(0, 0, 240, 1, C_LINE);
    switch (pg.ask ? PG_N : ui.page) {
    case PG_N: s = "PLAY yes   HOME no"; break;
    case PG_MENU: s = "SELECT move  SEL open  HOME back"; break;
    case PG_KEY:
        t[0] = 0;
        text_cat(t, KV_NAME[kview()]);
        text_cat(t, "  K4 send speed  OCT- clear");
        s = t;
        break;
    case PG_RHYTHM: s = "SELECT drill  K4 send speed  PLAY again"; break;
    case PG_LISTEN:
        s = lhide() ? "PLAY start/stop  SEL peek  SEQ show" : "PLAY start/stop  SELECT program  SEQ hide";
        break;
    case PG_TYPE: s = "OCT-/+ page   REC del, hold: clear\nPLAY play/stop  SEL live/compose  SEQ code"; break;
    case PG_SET: s = "SELECT item  OCT- / OCT+ change"; break;
    case PG_CHART:
        if (pg.edit) {
            s = "OCT-/+ K1 level  K2 groups  FX all back\nPLAY/REC save  SEL hear, hold: reset all";
            break;
        }
        t[0] = 0;
        text_cat(t, "REC edit  PLAY hear ");
        t[20] = ln_char(ui.csel), t[21] = 0;
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
            s = ln_choose(c->type) ? "keys E4-A4, or SELECT + SEL"
                : card_key(c)      ? "PLAY again  SEL don't know"
                : visual()         ? "PLAY again  a key: next"
                                   : "PLAY again  SEQ the code  a key: next";
        else if (pr.ps == PS_RESULT)
            s = visual() || pr.has_j ? "a key: next" : "a key: next  SEQ the code";
        else
            s = "PLAY start  SELECT round/test  FX mute";
        break;
    default: break;
    }
    for (i = 0; s[i] && s[i] != '\n'; i++)
        ;
    if (s[i]) {                          /* two lines */
        char a[48];
        uint32_t j;
        for (j = 0; j < i && j < sizeof a - 1u; j++)
            a[j] = s[j];
        a[j] = 0;
        cv_text(8, 3, &FONT_T, a, C_DIM);
        cv_text(8, 17, &FONT_T, s + i + 1, C_DIM);
    } else
        cv_text(8, 9, &FONT_XS, s, C_DIM);
    cv_blit(0, 208);
}

static int32_t lamp_y(void)              /* where this page shows the lamp, 0 none */
{
    if (ui.page == PG_KEY)
        return 88;
    if (ui.page == PG_TYPE)
        return type_lamp_y();
    if ((ui.page == PG_PRACTICE && (pr.ps == PS_CARD || pr.ps == PS_RESULT)) || ui.page == PG_RHYTHM
        || (ui.page == PG_LISTEN && ls.on))
        return 196;                      /* under the page (practice_at, rhythm_at, listen_at keep above 196) */
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
    if ((r & (R_MAIN | R_LAMP)) && lamp_y() && (lamp_y() == 196 || !(r & R_MAIN)))
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
    zero(&tim, sizeof tim);
    zero(&ls, sizeof ls);
    zero(&rh, sizeof rh);
    zero(txt, sizeof txt);
    zero(dsrc, sizeof dsrc);
    zero(&applied, sizeof applied);
    ev_w = ev_r = 0;
    rx_t = plat_ms();
#if FM1B_BLE
    bx_w = bx_r = 0;
    zero(&ble_rx, sizeof ble_rx);
#endif
    own_down = ptx_down = prx_down = rx_midi = rx_down = midi_down = 0;
    key_input = own_midi = ptx_midi = 0;
    saves = 0;
    dirty_at = 0;
    if (plat_store_load(OBJ_MORSE, &rec, sizeof rec) == (int)sizeof rec && rec.set.send == 0u)
        rec.set.send = SEND_DEFAULT;     /* a 0.1 record: it had no send speed */
    if (rec.magic != REC_MAGIC || rec.ver != REC_VER || rec.size != sizeof rec || !set_valid(&rec.set)
        || !ln_valid(&rec.ln)) {
        zero(&rec, sizeof rec);
        set_defaults();
        ln_init(&rec.ln);
    }
    mo_keyer_init(&keyer, SET.keyer, mo_dit_ms(SET.send));
    mo_player_init(&ptx, mo_dit_ms(SET.wpm), mo_gap_ms(SET.wpm, SET.eff));
    mo_player_init(&prx, mo_dit_ms(SET.wpm), mo_gap_ms(SET.wpm, SET.eff));
#ifdef PLAT_HOST
    mo_player_init(&host_hand, mo_dit_ms(SET.send), mo_dit_ms(SET.send));
#endif
    tone_stop();
    timing_apply();
    hl_init(&link, 0x4D, 0x4F, MO_VER);
    ui.page = PG_PRACTICE;               /* straight into today's practice */
    ui.back = PG_PRACTICE;
    pr.ps = PS_START;
    rhythm_hint();
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
#if FM1B_BLE
    ble_in();
#endif
    read_events();
    if (ui.page == PG_PRACTICE)
        practice_step(now);
    if (ui.page == PG_LISTEN)
        listen_step();
    hl_tick(&link, now, T_HELLO, T_HEARTBEAT, HELLO, 3);
    prog_pump();
    host = (uint8_t)hl_host(&link, now);
    if (host != ui.host_was) {
        ui.host_was = host;
        ui.redraw |= R_HEAD | (ui.page == PG_SET ? R_MAIN : 0u);
    }
#if FM1B_BLE
    if ((uint8_t)plat_ble_connected() != ui.ble_was) {   /* BT grey or blue, and SETTINGS says so */
        ui.ble_was = (uint8_t)plat_ble_connected();
        ui.redraw |= R_HEAD | (ui.page == PG_SET ? R_MAIN : 0u);
    }
#endif
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
    if (dirty_at && (int32_t)(now - dirty_at) > 3000)   /* settings saved once the knobs rest */
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
        *got = (int)muted();
    else if (!strcmp(what, "ble_off"))
        *got = (int)bt_off();
    else if (!strcmp(what, "wpm"))
        *got = SET.wpm;
    else if (!strcmp(what, "eff"))
        *got = SET.eff;
    else if (!strcmp(what, "send"))
        *got = SET.send;
    else if (!strcmp(what, "pitch"))
        *got = SET.pitch;
    else if (!strcmp(what, "keyer"))
        *got = SET.keyer;
    else if (!strcmp(what, "learn"))
        *got = SET.learn;
    else if (!strcmp(what, "preset"))
        *got = preset_now();
    else if (!strcmp(what, "kview"))
        *got = (int)kview();
    else if (!strcmp(what, "tlabel"))
        *got = (int)tlabel();
    else if (!strcmp(what, "tlayout"))
        *got = (int)tlayout();
    else if (!strcmp(what, "tcompose"))
        *got = (int)tcompose();
    else if (!strcmp(what, "tpage"))
        *got = ui.tpage;
    else if (!strcmp(what, "line_len"))
        *got = (int)slen(ui.line);
    else if (!strcmp(what, "host"))
        *got = hl_host(&link, plat_ms());
    else if (!strcmp(what, "transport"))
        *got = link.transport;
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
    else if (!strcmp(what, "playing"))
        *got = playing(&ptx);
    else if (!strcmp(what, "tim_rows"))
        *got = tim.count;
    else if (!strcmp(what, "rh_n"))
        *got = rh.n;
    else if (!strcmp(what, "rh_sets"))
        *got = rh.sets;
    else if (!strcmp(what, "rh_clean"))
        *got = rh.good_sets;
    else if (!strcmp(what, "rh_hint"))
        *got = rh.hint;
    else if (!strcmp(what, "rh_sugg"))
        *got = rh.sugg;
    else if (!strcmp(what, "listening"))
        *got = ls.on;
    else if (!strcmp(what, "lines"))
        *got = (int)ls.count;
    else if (!strcmp(what, "prog"))
        *got = SET.prog;
    else if (!strcmp(what, "lhide"))
        *got = (int)lhide();
    else if (!strcmp(what, "res"))
        *got = pr.res;
    else if (!strcmp(what, "score"))
        *got = pr.has_j ? pr.j.score : -1;
    else if (!strcmp(what, "hint"))
        *got = pr.has_j ? pr.j.hint : -1;
    else if (!strcmp(what, "marks"))
        *got = pr.an;
    else if (!strcmp(what, "answer_ok"))
        *got = pr.answer_ok;
    else if (!strcmp(what, "test"))
        *got = pr.test;
    else if (!strcmp(what, "ask"))
        *got = pg.ask;
    else if (!strcmp(what, "edit"))
        *got = pg.edit;
    else if (!strcmp(what, "dirty"))
        *got = pg.dirty;
    else if (!strcmp(what, "csel"))
        *got = ui.csel;
    else if (!strcmp(what, "lv"))            /* the level of the letter under CHART's cursor, as shown */
        *got = ui.csel < chart_state()->open ? (int)ln_level(chart_state(), ui.csel) : -1;
    else if (!strcmp(what, "shown_open"))    /* the groups CHART shows (the copy, editing) */
        *got = chart_state()->open;
    else if (!strcmp(what, "strict"))
        *got = (int)strict();
    else if (!strcmp(what, "peek"))
        *got = pr.peek;
    else if (!strcmp(what, "near"))
        *got = pr.r.near;
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
#if FM1B_BLE
    if (!strcmp(cmd, "ble_frame")) {     /* ble_frame hello_ack|prog_req|text WORD: one of the host's frames over
                                          * Bluetooth, as the own stack hands it over (SysEx packets, CIN 4..7) */
        uint8_t f[48];
        uint32_t n = 0, i, k;
        f[n++] = 0xF0, f[n++] = 0x7D, f[n++] = 0x4D, f[n++] = 0x4F, f[n++] = MO_VER;
        if (!strcmp(a, "hello_ack"))
            f[n++] = T_HELLO_ACK, f[n++] = 1;
        else if (!strcmp(a, "prog_req"))
            f[n++] = T_PROG_REQ;
        else {
            f[n++] = T_TEXT;
            for (i = 0; b[i] && n < sizeof f - 1u; i++)
                f[n++] = (uint8_t)b[i];
        }
        f[n++] = 0xF7;
        for (i = 0; i < n; i += 3) {
            uint32_t left = n - i, pkt;
            k = left > 3u ? 3u : left;
            pkt = left > 3u ? 4u : 4u + k;
            pkt |= (uint32_t)f[i] << 8;
            if (k > 1u)
                pkt |= (uint32_t)f[i + 1u] << 16;
            if (k > 2u)
                pkt |= (uint32_t)f[i + 2u] << 24;
            bx[bx_w] = pkt;
            bx_w = (bx_w + 1u) % BX_N;
        }
        return 1;
    }
#endif
    if (!strcmp(cmd, "setcard")) {       /* setcard hear_key|see_key CHAR: the card asked now becomes this one */
        ln_card_t *k = pr.ps == PS_CARD && pr.r.at < pr.r.n ? &pr.r.card[pr.r.at] : 0;
        int i = ln_index(b[0]);
        if (!k || i < 0) {
            printf("  FAIL setcard: no card, or not a letter of the course\n");
            host_fails++;
            return 1;
        }
        k->ch = (uint8_t)i;
        k->type = !strcmp(a, "see_key") ? CARD_SEE_KEY : CARD_HEAR_KEY;
        k->dir = k->type == CARD_SEE_KEY ? LN_SEND : LN_REC;
        k->ch = (uint8_t)ln_index(b[0]);
        play_stop(&ptx);
        card_present();
        return 1;
    }
    if (!strcmp(cmd, "setstrict")) {     /* setstrict 0|1: JUDGING */
        look_set(7, 1u, a[0] == '1');
        return 1;
    }
    if (!strcmp(cmd, "clear_text")) {
        zero(txt, sizeof txt);
        return 1;
    }
    if (!strcmp(cmd, "sendkeys")) {      /* sendkeys TEXT: keyed by hand at the send speed (_ a space) */
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
        if (pr.ps == PS_DONE)
            printf("  (answer: the round is over)\n");
        else if (pr.ps != PS_CARD || !k) {
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
