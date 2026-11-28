// SPDX-License-Identifier: GPL-3.0-or-later
// The progress file the FM-1 and this app trade: "fm1-morse-progress" version 2, the same JSON that MORSE on the web
// (../index.html) and the host's `morse backup` write, so either takes the other's. Its names and checks follow
// ../index.html (toJSON, fromJSON, setToDict, setFromDict); keep them in step.
//
// One addition: "stats", MORSE 0.9's per-letter counts (learn.h ln_stats_t). The FM-1's backup does not carry them
// (0.9 keeps its own), and the readers above skip a key they do not know, so a file with them still loads there.
import { ORDER, N, REC, SEND } from "./core.js";

const KEYERS = ["straight", "iambic_a", "iambic_b"], PAGES = ["menu", "practice", "listen", "key", "rhythm", "type", "chart"];
const KEY_VIEWS = ["text", "code", "timing"], TYPE_LAYOUTS = ["white_black", "a_to_z", "koch"];
const LISTEN_PROGRAMS = ["koch", "words", "qso", "abbreviations", "text"];
export const SET_DEFAULT = { wpm: 20, eff: 8, keyer: 0, swap: 0, flags: 0, cards: 20, learn: 0, look: 0, pitch: 600, send: 10, prog: 0 };

export function setToDict(s) {
  const bit = (v, n) => !!(v >> n & 1);
  return { hear_wpm: s.wpm, spacing_wpm: s.eff, send_wpm: s.send, pitch_hz: s.pitch, keyer: KEYERS[s.keyer],
    paddles: s.swap ? "left_dah" : "left_dit", sound: !bit(s.flags, 0), bluetooth: !bit(s.flags, 1),
    start_page: PAGES[s.flags >> 2 & 7], cards: s.cards, learn_by: s.learn ? "sight" : "sound",
    judging: bit(s.look, 7) ? "strict" : "rhythm", key_view: KEY_VIEWS[s.look & 3], type_layout: TYPE_LAYOUTS[s.look >> 3 & 3],
    type_shows_code: bit(s.look, 2), type_composes: bit(s.look, 6), listen_hides_text: bit(s.look, 5),
    listen_program: LISTEN_PROGRAMS[s.prog] };
}
export function setFromDict(d) {
  const dflt = setToDict(SET_DEFAULT);
  for (const k of Object.keys(d)) if (!(k in dflt)) throw Error(`unknown setting ${k}`);
  const v = { ...dflt, ...d };
  const pick = (k, names) => { const i = names.indexOf(v[k]); if (i < 0) throw Error(`${k} must be one of ${names.join(", ")}`); return i; };
  return { wpm: +v.hear_wpm, eff: +v.spacing_wpm, send: +v.send_wpm, pitch: +v.pitch_hz,
    keyer: pick("keyer", KEYERS), swap: pick("paddles", ["left_dit", "left_dah"]),
    flags: (v.sound ? 0 : 1) | (v.bluetooth ? 0 : 2) | pick("start_page", PAGES) << 2, cards: +v.cards,
    learn: pick("learn_by", ["sound", "sight"]),
    look: pick("key_view", KEY_VIEWS) | (v.type_shows_code ? 4 : 0) | pick("type_layout", TYPE_LAYOUTS) << 3
      | (v.listen_hides_text ? 32 : 0) | (v.type_composes ? 64 : 0) | pick("judging", ["rhythm", "strict"]) << 7,
    prog: pick("listen_program", LISTEN_PROGRAMS) };
}

// the blob (learn.h ln_blob) from the file's numbers, and back
function toBlob(p) {
  const b = [], put = (v, n) => { for (let k = 0; k < n; k++) b.push((v >>> (8 * k)) & 0xFF); };
  put(p.round, 2); put(p.streak, 2); put(p.open, 1); put(p.answered, 4); put(p.right, 4);
  for (const x of p.box) b.push(x[0], x[1]);
  for (const x of p.last) { put(x[0] === null ? 0xFFFF : x[0], 2); put(x[1] === null ? 0xFFFF : x[1], 2); }
  return Uint8Array.from(b);
}

const STAT_KEYS = ["asked", "wrong", "almost", "lapses", "recent", "recent_missed"];

// the core's record as a file (firmware: what wrote it, for the reader)
export function toJSON(core, firmware) {
  const p = core.ln, st = core.stats, chars = {}, stats = {};
  ORDER.split("").forEach((c, i) => {
    chars[c] = { recognise: p.box[i][0], send: p.box[i][1], last_recognise: p.last[i][0], last_send: p.last[i][1] };
    if (st[i][REC].asked || st[i][SEND].asked || st[i][REC].lapses || st[i][SEND].lapses)
      stats[c] = { recognise: STAT_KEYS.map(k => st[i][REC][k]), send: STAT_KEYS.map(k => st[i][SEND][k]) };
  });
  const d = { format: "fm1-morse-progress", version: 2, firmware: firmware || null,
    round: p.round, streak: p.streak, open: p.open, answered: p.answered, right: p.right, chars,
    settings: setToDict(core.set), stats: { keys: STAT_KEYS, chars: stats } };
  return JSON.stringify(d, null, 1) + "\n";
}

// a file, read and checked as the FM-1 would check it (ln_unblob, set_valid), without changing the core.
// Returns { blob, settings | null, stats | null, summary }
export function fromJSON(core, text) {
  const d = JSON.parse(text);
  if (d.format !== "fm1-morse-progress") throw Error("not a MORSE progress file");
  if (d.version !== 1 && d.version !== 2) throw Error(`file version ${d.version}, this app knows 1 and 2`);
  const p = { round: +d.round, streak: +(d.streak || 0), open: +d.open, answered: +(d.answered || 0),
              right: +(d.right || 0), box: [], last: [] };
  for (const c of Object.keys(d.chars || {})) if (!ORDER.includes(c)) throw Error(`${c} is not in the course`);
  for (const c of ORDER) { const e = (d.chars || {})[c] || {};
    p.box.push([+(e.recognise || 0), +(e.send || 0)]);
    p.last.push([e.last_recognise ?? null, e.last_send ?? null]); }
  const blob = toBlob(p);
  if (!core.putBlob(blob, true)) throw Error("progress MORSE would refuse (a box past 5, a round after the last, open not 5..41)");
  let settings = null;
  if (d.settings) {
    settings = setFromDict(d.settings);
    const keep = core.set;
    if (!core.putSet(settings)) throw Error("settings MORSE would refuse");
    core.putSet(keep);
  }
  let stats = null;
  if (d.stats && d.stats.chars) {
    const keys = d.stats.keys || STAT_KEYS, zero = { asked: 0, wrong: 0, almost: 0, lapses: 0, recent: 0, recent_missed: 0 };
    stats = [];
    for (let i = 0; i < N; i++) {
      const e = d.stats.chars[ORDER[i]];
      const one = a => { const o = { ...zero }; if (a) keys.forEach((k, j) => { if (k in o) o[k] = +a[j] || 0; }); return o; };
      stats.push([one(e && e.recognise), one(e && e.send)]);
    }
  }
  const learnt = p.box.slice(0, p.open).filter(b => b[0] >= 2 && b[1] >= 1).length;
  return { blob, settings, stats, firmware: d.firmware || null,
           summary: `round ${p.round}, ${p.open} open, ${learnt} past the line, ${p.answered} answered, ${p.right} right` };
}

// take a file read by fromJSON into the core, as the FM-1's ASK_LOAD does: the progress, the settings if it has them;
// the stats if it has them (else the ones here stay, as the FM-1 keeps its own)
export function applyFile(core, f) {
  if (!core.putBlob(f.blob, false)) throw Error("not taken");
  if (f.settings) core.putSet(f.settings);
  if (f.stats && !core.putStats(f.stats)) throw Error("stats not taken");
}
