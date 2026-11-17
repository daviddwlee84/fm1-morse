/* SPDX-License-Identifier: MIT */
/* content: see content.h. Uses learn.h's Koch order for the groups. */
#include "content.h"
#include "learn.h"

const char *const LP_NAME[LP_N] = {"KOCH GROUPS", "WORDS", "QSO", "ABBREVIATIONS", "SAYINGS"};

/* the most common English words (a frequency list; facts, not anyone's text) */
static const char *const CT_WORDS[] = {
    "THE", "BE", "TO", "OF", "AND", "A", "IN", "THAT", "HAVE", "I", "IT", "FOR", "NOT", "ON", "WITH", "HE", "AS",
    "YOU", "DO", "AT", "THIS", "BUT", "HIS", "BY", "FROM", "THEY", "WE", "SAY", "HER", "SHE", "OR", "AN", "WILL",
    "MY", "ONE", "ALL", "WOULD", "THERE", "THEIR", "WHAT", "SO", "UP", "OUT", "IF", "ABOUT", "WHO", "GET", "WHICH",
    "GO", "ME", "WHEN", "MAKE", "CAN", "LIKE", "TIME", "NO", "JUST", "HIM", "KNOW", "TAKE", "PEOPLE", "INTO",
    "YEAR", "YOUR", "GOOD", "SOME", "COULD", "THEM", "SEE", "OTHER", "THAN", "THEN", "NOW", "LOOK", "ONLY", "COME",
    "ITS", "OVER", "THINK", "ALSO", "BACK", "AFTER", "USE", "TWO", "HOW", "OUR", "WORK", "FIRST", "WELL", "WAY",
    "EVEN", "NEW", "WANT", "BECAUSE", "ANY", "THESE", "GIVE", "DAY", "MOST", "US",
};

/* CW abbreviations and prosigns (<..> run together) with what they mean */
static const struct {
    const char *a, *m;
} CT_ABBR[] = {
    {"CQ", "calling any station"},   {"DE", "this is, from"},         {"K", "over, go ahead"},
    {"<KN>", "over, only you"},      {"<AR>", "end of message"},      {"<SK>", "end of contact"},
    {"<BT>", "pause, new part"},     {"<BK>", "back to you"},         {"R", "received"},
    {"TU", "thank you"},             {"TNX", "thanks"},               {"FER", "for"},
    {"73", "best regards"},          {"GM", "good morning"},          {"GA", "good afternoon"},
    {"GE", "good evening"},          {"GL", "good luck"},             {"HW?", "how do you copy?"},
    {"UR", "your, you are"},         {"ES", "and"},                   {"FB", "fine business: great"},
    {"OM", "old man: fellow ham"},   {"OP", "operator"},              {"NAME", "my name is"},
    {"RST", "signal report"},        {"5NN", "report 599, short"},    {"QTH", "my location is"},
    {"QRZ?", "who is calling me?"},  {"QSL", "I confirm"},            {"QRS", "send slower"},
    {"QRQ", "send faster"},          {"QRM", "interference"},         {"QSB", "fading signal"},
    {"WX", "weather"},               {"RIG", "my radio"},             {"ANT", "antenna"},
    {"PSE", "please"},               {"AGN", "again"},                {"SRI", "sorry"},
    {"HR", "here"},                  {"CUL", "see you later"},
};
#define CT_NABBR (sizeof CT_ABBR / sizeof CT_ABBR[0])

/* old sayings, and the first telegraph message (Morse, 1844) */
static const char *const CT_SAYINGS[] = {
    "WHAT HATH GOD WROUGHT",
    "THE QUICK BROWN FOX JUMPS OVER THE LAZY DOG",
    "PRACTICE MAKES PERFECT",
    "SLOW AND STEADY WINS THE RACE",
    "THE EARLY BIRD CATCHES THE WORM",
    "ACTIONS SPEAK LOUDER THAN WORDS",
    "A BIRD IN THE HAND IS WORTH TWO IN THE BUSH",
    "WHERE THERE IS A WILL THERE IS A WAY",
    "TIME AND TIDE WAIT FOR NO MAN",
    "THE PEN IS MIGHTIER THAN THE SWORD",
    "ALL THAT GLITTERS IS NOT GOLD",
    "BETTER LATE THAN NEVER",
    "KNOWLEDGE IS POWER",
    "NO NEWS IS GOOD NEWS",
    "LOOK BEFORE YOU LEAP",
    "HASTE MAKES WASTE",
    "TWO HEADS ARE BETTER THAN ONE",
    "EASY COME EASY GO",
    "ROME WAS NOT BUILT IN A DAY",
    "EVERY CLOUD HAS A SILVER LINING",
};

static const char *const CT_PREFIX[] = {"K", "W", "N", "AA", "VE", "G", "DL", "F", "JA", "VK", "BV", "OH",
                                        "ON", "PA", "EA", "I", "SM", "LA", "HB9", "ZL"};
static const char *const CT_NAMES[] = {"BOB", "ANN", "JOE", "MAY", "TOM", "LEE", "SAM", "KEN",
                                       "AMY", "RAY", "DAN", "EVA", "IAN", "JIM", "LIZ", "MAX"};
static const char *const CT_PLACES[] = {"TAIPEI", "BOSTON", "TOKYO", "PARIS", "BERLIN", "DENVER",
                                        "OSLO", "ROME", "MADRID", "SEATTLE", "SYDNEY", "TORONTO"};
static const char *const CT_RST[] = {"599", "579", "559", "479", "589", "569"};

const char *const CT_PHRASE_WHITE[CT_PHRASE_WHITE_N] = {"CQ", "DE", "K", "<KN>", "<AR>", "<SK>", "<BT>", "73",
                                                        "TU", "RST", "5NN", "QTH", "NAME", "OP", "UR", "ES"};
const char *const CT_PHRASE_BLACK[CT_PHRASE_BLACK_N] = {"GM", "GE", "FB", "R", "QRZ?", "QSL",
                                                        "<BK>", "HW?", "PSE", "AGN", "QRS"};

#define CT_N(a) (sizeof(a) / sizeof((a)[0]))

static uint32_t ct_rand(ct_gen_t *g)     /* xorshift32 */
{
    uint32_t v = g->rng ? g->rng : 1u;
    v ^= v << 13;
    v ^= v >> 17;
    v ^= v << 5;
    return g->rng = v;
}

static uint32_t ct_pick(ct_gen_t *g, uint32_t n)   /* an index, not the one given last */
{
    uint32_t i = ct_rand(g) % n;
    if (n > 1u && i == g->last)
        i = (i + 1u + ct_rand(g) % (n - 1u)) % n;
    g->last = (uint8_t)i;
    return i;
}

static int ct_eq(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return *a == *b;
}

const char *ct_gloss(const char *abbr)
{
    uint32_t i;
    for (i = 0; i < CT_NABBR; i++)
        if (ct_eq(CT_ABBR[i].a, abbr))
            return CT_ABBR[i].m;
    return 0;
}

/* a line built with cat: never past CT_LINE_MAX */
static void ct_cat(char *out, const char *s)
{
    uint32_t n = 0;
    while (out[n])
        n++;
    while (*s && n < CT_LINE_MAX)
        out[n++] = *s++;
    out[n] = 0;
}

static void ct_call(ct_gen_t *g, char *c)   /* a made-up call sign: prefix, digit, two or three letters */
{
    const char *p = CT_PREFIX[ct_rand(g) % CT_N(CT_PREFIX)];
    uint32_t n = 0, k, len = 2u + ct_rand(g) % 2u;
    while (*p)
        c[n++] = *p++;
    if (c[n - 1u] < '0' || c[n - 1u] > '9')
        c[n++] = (char)('0' + ct_rand(g) % 10u);
    for (k = 0; k < len && n < 7u; k++)
        c[n++] = (char)('A' + ct_rand(g) % 26u);
    c[n] = 0;
}

void ct_init(ct_gen_t *g, uint32_t seed)
{
    g->rng = seed ? seed : 0x2545F491u;
    g->step = 0;
    g->last = 0xFF;
    g->a[0] = g->b[0] = 0;
}

/* a made-up QSO, six lines: a calls CQ, b answers, they trade reports, names and places, and sign off */
static void ct_qso(ct_gen_t *g, char *out)
{
    const char *a = g->a, *b = g->b;
    if (g->step == 0) {
        ct_call(g, g->a);
        do
            ct_call(g, g->b);
        while (ct_eq(g->a, g->b));
        g->na = (uint8_t)(ct_rand(g) % CT_N(CT_NAMES));
        g->nb = (uint8_t)((g->na + 1u + ct_rand(g) % (CT_N(CT_NAMES) - 1u)) % CT_N(CT_NAMES));
        g->qa = (uint8_t)(ct_rand(g) % CT_N(CT_PLACES));
        g->qb = (uint8_t)((g->qa + 1u + ct_rand(g) % (CT_N(CT_PLACES) - 1u)) % CT_N(CT_PLACES));
        g->ra = (uint8_t)(ct_rand(g) % CT_N(CT_RST));
        g->rb = (uint8_t)(ct_rand(g) % CT_N(CT_RST));
    }
    out[0] = 0;
    switch (g->step) {
    case 0:
        ct_cat(out, "CQ CQ CQ DE "), ct_cat(out, a), ct_cat(out, " "), ct_cat(out, a), ct_cat(out, " K");
        break;
    case 1:
        ct_cat(out, a), ct_cat(out, " DE "), ct_cat(out, b), ct_cat(out, " "), ct_cat(out, b), ct_cat(out, " K");
        break;
    case 2:
        ct_cat(out, b), ct_cat(out, " DE "), ct_cat(out, a), ct_cat(out, " GM TNX FER CALL UR RST ");
        ct_cat(out, CT_RST[g->ra]), ct_cat(out, " NAME "), ct_cat(out, CT_NAMES[g->na]), ct_cat(out, " QTH ");
        ct_cat(out, CT_PLACES[g->qa]), ct_cat(out, " HW? <AR> "), ct_cat(out, b), ct_cat(out, " DE "), ct_cat(out, a);
        ct_cat(out, " K");
        break;
    case 3:
        ct_cat(out, a), ct_cat(out, " DE "), ct_cat(out, b), ct_cat(out, " R TU "), ct_cat(out, CT_NAMES[g->na]);
        ct_cat(out, " UR RST "), ct_cat(out, CT_RST[g->rb]), ct_cat(out, " NAME "), ct_cat(out, CT_NAMES[g->nb]);
        ct_cat(out, " QTH "), ct_cat(out, CT_PLACES[g->qb]), ct_cat(out, " <BK>");
        break;
    case 4:
        ct_cat(out, "R FB "), ct_cat(out, CT_NAMES[g->nb]), ct_cat(out, " TNX FER QSO 73 GL <SK> ");
        ct_cat(out, b), ct_cat(out, " DE "), ct_cat(out, a);
        break;
    default:
        ct_cat(out, "TU "), ct_cat(out, CT_NAMES[g->na]), ct_cat(out, " 73 CUL <SK> "), ct_cat(out, a);
        ct_cat(out, " DE "), ct_cat(out, b), ct_cat(out, " E E");
        break;
    }
    g->step = (uint8_t)((g->step + 1u) % 6u);
}

void ct_line(ct_gen_t *g, int p, uint32_t open, char *out, const char **gloss)
{
    uint32_t i, k;
    out[0] = 0;
    *gloss = 0;
    switch (p) {
    case LP_KOCH:                        /* five groups of five from the letters open */
        if (open < 2u)
            open = 2u;
        if (open > LN_N)
            open = LN_N;
        for (k = 0; k < 5u; k++) {
            char grp[7];
            for (i = 0; i < 5u; i++)
                grp[i] = ln_char(ct_rand(g) % open);
            grp[5] = k < 4u ? ' ' : 0;
            grp[6] = 0;
            ct_cat(out, grp);
        }
        break;
    case LP_WORDS:                       /* six words */
        for (k = 0; k < 6u; k++) {
            if (k)
                ct_cat(out, " ");
            ct_cat(out, CT_WORDS[ct_rand(g) % CT_N(CT_WORDS)]);
        }
        break;
    case LP_QSO:
        ct_qso(g, out);
        break;
    case LP_ABBR:                        /* one, twice, and what it means */
        i = ct_pick(g, CT_NABBR);
        ct_cat(out, CT_ABBR[i].a), ct_cat(out, " "), ct_cat(out, CT_ABBR[i].a);
        *gloss = CT_ABBR[i].m;
        break;
    default:
        ct_cat(out, CT_SAYINGS[ct_pick(g, CT_N(CT_SAYINGS))]);
        break;
    }
}
