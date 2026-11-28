// SPDX-License-Identifier: GPL-3.0-or-later
// core.wasm (MORSE's learn.c, morse.c, content.c and core.c) with the record laid out as on the FM-1 (app.c rec_t).
// The offsets below are learn.h's structs on a 32-bit little-endian core; core.c asserts the sizes they add up to.
export const ORDER = "KMURESNAPTLWI.JZ=FOY,VG5/Q92H38B?47C1D60X", N = 41, GROUP = 5, TOP = 5, NEVER = 0xFFFF;
export const REC = 0, SEND = 1;                     // LN_REC, LN_SEND
export const WRONG = 0, RIGHT = 1, ALMOST = 2;      // LN_*
export const MJ_RIGHT = 0, MJ_ALMOST = 1, MJ_WRONG = 2;
export const CARD = { NEW: 0, HEAR_CHOOSE: 1, HEAR_KEY: 2, SEE_KEY: 3, CODE_CHOOSE: 4, LIGHT_CHOOSE: 5 };
export const F_MUTED = 1, F_VISUAL = 2, F_PLACE = 4;
export const LP = ["koch", "words", "qso", "abbreviations", "text"];
const LN = 20, ST = 280;                            // offsetof(rec_t, ln), offsetof(rec_t, st)
const L_BOX = 6, L_LAST = 88, L_ANS = 252, L_RIGHT = 256;
const S_ASKED = 0, S_WRONG = 164, S_NEAR = 328, S_LAPSE = 492, S_RQ = 574, S_RM = 656;

export async function loadCore(src) {
  const bytes = src instanceof ArrayBuffer || ArrayBuffer.isView(src) ? src
    : await (await fetch(src || new URL("core.wasm", import.meta.url))).arrayBuffer();
  const { instance } = await WebAssembly.instantiate(bytes, {});
  return new Core(instance.exports);
}

export class Core {
  constructor(x) { this.x = x; }
  get mem() { return new Uint8Array(this.x.memory.buffer); }
  get dv() { return new DataView(this.x.memory.buffer); }
  get size() { return this.x.rec_size(); }

  // ---- the record: 1020 bytes, as MORSE 0.9 keeps it at 0xDA000
  recBytes() { this.x.rec_seal(); return this.mem.slice(this.x.rec_ptr(), this.x.rec_ptr() + this.size); }
  recLoad(b) {        // MORSE's own check (rec_load): 0.9's record, or 0.1-0.8's (stats empty). False: defaults
    if (!b || b.length > this.size) { this.x.rec_new(); return false; }
    const p = this.x.rec_ptr(); this.mem.fill(0, p, p + this.size); this.mem.set(b, p);
    return !!this.x.rec_check(b.length);
  }
  recNew() { this.x.rec_new(); }

  // ---- settings (set_t), by their names in app.c
  get set() {
    const m = this.mem, p = this.x.set_ptr();
    return { wpm: m[p], eff: m[p + 1], keyer: m[p + 2], swap: m[p + 3], flags: m[p + 4], cards: m[p + 5],
             learn: m[p + 6], look: m[p + 7], pitch: m[p + 8] | m[p + 9] << 8, send: m[p + 10], prog: m[p + 11] };
  }
  putSet(s) {         // false (nothing changed) when MORSE would refuse it
    const m = this.mem, p = this.x.set_ptr(), old = m.slice(p, p + 12);
    m.set([s.wpm, s.eff, s.keyer, s.swap, s.flags, s.cards, s.learn, s.look, s.pitch & 0xFF, s.pitch >> 8, s.send, s.prog], p);
    if (this.x.set_ok()) return true;
    this.mem.set(old, p); return false;
  }

  // ---- progress (ln_state_t)
  get ln() {
    const d = this.dv, b = this.x.rec_ptr() + LN, box = [], last = [];
    for (let i = 0; i < N; i++) {
      box.push([d.getUint8(b + L_BOX + 2 * i), d.getUint8(b + L_BOX + 2 * i + 1)]);
      const a = d.getUint16(b + L_LAST + 4 * i, true), c = d.getUint16(b + L_LAST + 4 * i + 2, true);
      last.push([a === NEVER ? null : a, c === NEVER ? null : c]);
    }
    return { round: d.getUint16(b, true), streak: d.getUint16(b + 2, true), open: d.getUint8(b + 4), box, last,
             answered: d.getUint32(b + L_ANS, true), right: d.getUint32(b + L_RIGHT, true) };
  }
  // the progress as a backup carries it (learn.h ln_blob: 2+2+1+4+4 + boxes + last rounds, little-endian)
  blob() { this.x.blob_out(); return this.mem.slice(this.x.blob_ptr(), this.x.blob_ptr() + this.x.blob_size()); }
  putBlob(b, check) { this.mem.set(b, this.x.blob_ptr()); return !!(check ? this.x.blob_check() : this.x.blob_in()); }

  // ---- stats (ln_stats_t, 0.9): per character and direction
  get stats() {
    const d = this.dv, b = this.x.rec_ptr() + ST, out = [];
    for (let i = 0; i < N; i++) {
      const one = dir => { const k = 2 * i + dir;
        return { asked: d.getUint16(b + S_ASKED + 2 * k, true), wrong: d.getUint16(b + S_WRONG + 2 * k, true),
                 almost: d.getUint16(b + S_NEAR + 2 * k, true), lapses: d.getUint8(b + S_LAPSE + k),
                 recent: d.getUint8(b + S_RQ + k), recent_missed: d.getUint8(b + S_RM + k) }; };
      out.push([one(REC), one(SEND)]);
    }
    return out;
  }
  putStats(st) {      // false (nothing changed) when 0.9 would refuse them
    const d = this.dv, b = this.x.rec_ptr() + ST, m = this.mem, old = m.slice(b, b + 738);
    for (let i = 0; i < N; i++) for (const dir of [REC, SEND]) {
      const k = 2 * i + dir, e = st[i][dir];
      d.setUint16(b + S_ASKED + 2 * k, e.asked, true); d.setUint16(b + S_WRONG + 2 * k, e.wrong, true);
      d.setUint16(b + S_NEAR + 2 * k, e.almost, true); d.setUint8(b + S_LAPSE + k, e.lapses);
      d.setUint8(b + S_RQ + k, e.recent); d.setUint8(b + S_RM + k, e.recent_missed);
    }
    if (this.x.stats_ok()) return true;
    m.set(old, b); return false;
  }

  // ---- a round (ln_round_t)
  build(flags, seed) { this.x.round_build(flags, seed >>> 0); }
  get round() {
    const m = this.mem, p = this.x.round_ptr() + this.x.round_n_off();
    return { n: m[p], at: m[p + 1], ok: m[p + 2], miss: m[p + 3], up: m[p + 4], fresh: m[p + 5], near: m[p + 6], place: m[p + 7] };
  }
  cardAt(i) { const p = this.x.round_ptr() + i * this.x.card_size(); return this.cardFrom(p); }
  card() { const p = this.x.round_card(); return p ? this.cardFrom(p) : null; }
  cardFrom(p) {
    const m = this.mem;
    return { ch: m[p], dir: m[p + 1], type: m[p + 2], again: m[p + 3], opt: [m[p + 4], m[p + 5], m[p + 6], m[p + 7]], right: m[p + 8],
             char: ORDER[m[p]] };
  }
  answer(res) { return this.x.round_answer(res); }
  finish() { return !!this.x.round_finish(); }
  placeCount() { return this.x.round_place_count(); }
  placeApply() { return this.x.round_place_apply(); }
  known(i) { return !!this.x.round_known(i); }

  dueCount() { return this.x.due_count(); }
  newCount() { return this.x.new_count(); }
  level(i) { return this.x.level(i); }
  hard(i, dir) { return this.x.hard(i, dir); }
  missPct(i, dir) { return this.x.miss_pct(i, dir); }
  choose(type) { return !!this.x.choose_type(type); }
  stepBack() { this.x.step_back(); }
  progressReset() { this.x.progress_reset(); }

  // ---- the code
  code(c) { return this.x.code(c.charCodeAt(0)); }
  pattern(c) { const k = this.code(c); let s = ""; for (let i = 0; i < this.x.code_len(k); i++) s += this.x.code_elem(k, i) ? "-" : "."; return s; }
  ditMs(wpm) { return this.x.dit_ms(wpm); }
  gapMs(wpm, eff) { return this.x.gap_ms(wpm, eff); }
  // text as marks and spaces, ms, the first a mark: mo_player's own timing, run a millisecond at a time
  timeline(text, dit, gap) {
    const p = this.x.text_ptr(), m = this.mem, s = String(text).slice(0, 120);
    for (let i = 0; i < s.length; i++) m[p + i] = s.charCodeAt(i) & 0x7F;
    m[p + s.length] = 0;
    const n = this.x.timeline(dit, gap), t = this.x.tl_ptr() >> 2;
    return Array.from(new Uint32Array(this.x.memory.buffer).subarray(t, t + n));
  }

  // ---- a keyed answer: marks and the gaps before them (ms), judged as the FM-1 judges it
  setKeyed(marks, gaps) {
    const d = this.dv, mp = this.x.marks_ptr(), gp = this.x.gaps_ptr();
    for (let i = 0; i < 8; i++) { d.setUint16(mp + 2 * i, Math.min(0xFFFF, marks[i] || 0), true); d.setUint16(gp + 2 * i, Math.min(0xFFFF, gaps[i] || 0), true); }
  }
  judge(marks, gaps, want, ditEst) {
    this.setKeyed(marks, gaps);
    const v = this.x.judge(marks.length, this.code(want), ditEst);
    return { verdict: v & 0xFF, hint: v >> 8 & 0xFF, score: v >>> 16 & 0xFF, dit: this.x.judge_dit() };
  }
  teach(marks, gaps, want) { this.setKeyed(marks, gaps); this.x.dec_teach(marks.length, this.code(want)); }
  decInit(dit, gap) { this.x.dec_init(dit, gap); }
  decRestart() { this.x.dec_restart(); }
  decMark(ms) { this.x.dec_mark(Math.round(ms)); }
  decGap(ms) { const v = this.x.dec_gap(Math.round(ms)); return (v & 0xFF ? String.fromCharCode(v & 0xFF) : "") + (v >> 8 ? String.fromCharCode(v >> 8) : ""); }
  decIdle(ms) { const v = this.x.dec_idle(Math.round(ms)); return v ? String.fromCharCode(v) : ""; }
  decUnit() { return this.x.dec_unit(); }
  decWpm() { return this.x.dec_wpm(); }
  decRatio10() { return this.x.dec_ratio10(); }
  keyerInit(mode, dit) { this.x.keyer_init(mode, dit); }
  keyerTick(dit, dah) { return this.x.keyer_tick(dit ? 1 : 0, dah ? 1 : 0); }

  // ---- LISTEN
  listenInit(seed) { this.x.listen_init(seed >>> 0); }
  listenLine(prog) { const p = this.x.listen_line(prog); return this.cstr(p); }
  listenGloss() { return this.cstr(this.x.listen_gloss()); }
  cstr(p) { const m = this.mem; let s = ""; while (m[p]) s += String.fromCharCode(m[p++]); return s; }
}
