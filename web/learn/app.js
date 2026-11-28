// SPDX-License-Identifier: GPL-3.0-or-later
// MORSE practice: the FM-1's PRACTICE, LISTEN, KEY and CHART pages on a touch screen. Every decision about the
// course (which cards, how a box moves, how a keyed answer is judged, what LISTEN plays) is made by MORSE's own C in
// core.wasm; this file is the screen, the sound and the touch, after app.c's practice_* and listen_* functions.
import { loadCore, ORDER, N, GROUP, REC, SEND, WRONG, RIGHT, ALMOST, MJ_WRONG, MJ_RIGHT, MJ_ALMOST, CARD,
         F_MUTED, F_VISUAL, F_PLACE, LP } from "./core.js";
import { toJSON, fromJSON, applyFile } from "./progress.js";

export const VERSION = "0.1";            // this app; the course is MORSE 0.9's (core.wasm)
const CORE_VERSION = "0.9";
const STORE = "fm1-morse.rec";           // the record's 1020 bytes, base64
const $ = id => document.getElementById(id);
const core = await loadCore();
window.morse = { core };                 // for the tests and the curious

// ------------------------------------------------------------------------------------------------ the record ---
function b64(u8) { let s = ""; for (const x of u8) s += String.fromCharCode(x); return btoa(s); }
function unb64(s) { return Uint8Array.from(atob(s), c => c.charCodeAt(0)); }
function load() {
  let s = null;
  try { s = localStorage.getItem(STORE); } catch { }
  if (s) { try { if (core.recLoad(unb64(s))) return true; } catch { } }
  core.recNew();
  return false;
}
function save() {
  try { localStorage.setItem(STORE, b64(core.recBytes())); } catch { toast("could not save (storage blocked)"); }
}
const hadRecord = load();
try { navigator.storage && navigator.storage.persist && navigator.storage.persist(); } catch { }

const S = () => core.set;
function setChange(f) {                   // f mutates a copy of the settings; MORSE's own check decides
  const s = S(); f(s);
  if (!core.putSet(s)) { toast("MORSE would refuse that"); return false; }
  save(); return true;
}
const muted = () => !!(S().flags & 1);
const visual = () => S().learn === 1;
const strict = () => !!(S().look >> 7 & 1);
const lhide = () => !!(S().look >> 5 & 1);

// ---------------------------------------------------------------------------------------------------- sound ---
const audio = {
  ctx: null, osc: null, g: null, end: 0, t0: 0, segs: [], vol: 0.22,
  ensure() {
    if (!this.ctx) {
      const C = window.AudioContext || window.webkitAudioContext;
      if (!C) return;
      this.ctx = new C();
      this.osc = this.ctx.createOscillator(); this.g = this.ctx.createGain();
      this.osc.type = "sine"; this.g.gain.value = 0;
      this.osc.connect(this.g).connect(this.ctx.destination); this.osc.start();
    }
    if (this.ctx.state !== "running") this.ctx.resume();
  },
  // text at the hearing speeds, as the FM-1's player keys it (core.timeline). Returns when it ends, ms
  play(text, dit, gap) {
    this.stop();
    const segs = timelineLong(text, dit, gap), lead = 60;
    this.segs = segs; this.t0 = performance.now() + lead;
    this.end = this.t0 + segs.reduce((a, b) => a + b, 0);
    if (!muted() && this.ensureRunning()) {
      const g = this.g.gain, r = 0.005;
      this.osc.frequency.setValueAtTime(S().pitch, this.ctx.currentTime);
      let t = this.ctx.currentTime + lead / 1000;
      g.cancelScheduledValues(0); g.setValueAtTime(0, this.ctx.currentTime);
      segs.forEach((d, i) => {
        if (i % 2 === 0 && d > 0) {
          g.setValueAtTime(0, t); g.linearRampToValueAtTime(this.vol, t + r);
          g.setValueAtTime(this.vol, t + d / 1000); g.linearRampToValueAtTime(0, t + d / 1000 + r);
        }
        t += d / 1000;
      });
    }
    return this.end;
  },
  ensureRunning() { this.ensure(); return this.ctx && this.ctx.state !== "closed"; },
  playing() { return performance.now() < this.end; },
  markNow() {                            // the lamp: is the playback in a mark now
    const now = performance.now();
    if (now < this.t0 || now >= this.end) return false;
    let t = this.t0;
    for (let i = 0; i < this.segs.length; i++) { t += this.segs[i]; if (now < t) return i % 2 === 0; }
    return false;
  },
  stop() {
    this.end = 0; this.segs = [];
    if (this.ctx) { const g = this.g.gain; g.cancelScheduledValues(0); g.setTargetAtTime(0, this.ctx.currentTime, 0.002); }
  },
  key(on) {                              // the sidetone under the finger
    if (muted() || !this.ctx) return;
    const g = this.g.gain, t = this.ctx.currentTime;
    if (on) this.stop();
    this.osc.frequency.setValueAtTime(S().pitch, t);
    g.cancelScheduledValues(0); g.setValueAtTime(g.value, t); g.linearRampToValueAtTime(on ? this.vol : 0, t + 0.005);
  },
};
// mo_player queues 64 characters: longer text goes a word group at a time, the timelines joined
function timelineLong(text, dit, gap) {
  const out = [], push = (d, mark) => {
    const last = out.length - 1;
    if (last >= 0 && (last % 2 === 0) === mark) out[last] += d;
    else { if (out.length === 0 && !mark) out.push(0); out.push(d); }
  };
  const words = String(text).split(/(?<= )/);
  let chunk = "";
  const flush = () => { if (chunk) core.timeline(chunk, dit, gap).forEach((d, i) => push(d, i % 2 === 0)); chunk = ""; };
  for (const w of words) { if ((chunk + w).length > 60) flush(); chunk += w; }
  flush();
  return out;
}
const hear = () => [core.ditMs(S().wpm), core.gapMs(S().wpm, S().eff)];
function playText(t) { const [d, g] = hear(); return audio.play(t, d, g); }

// ------------------------------------------------------------------------------------------------ keying ---
// one key, as the FM-1's own source (app.c read_events): edges into the decoder, the practice's capture, the sound
const ks = { down: false, seen: false, tDown: 0, tUp: 0, paddles: [false, false], kdown: false, kt: 0, timer: 0 };
function edge(down, t) {
  let out = "";
  if (down) {
    let gap = 0;
    if (ks.seen && !ks.down) { gap = t - ks.tUp; out = core.decGap(gap); }
    practiceEdge(true, t, gap);
    ks.tDown = t;
  } else if (ks.down) {
    const mark = t - ks.tDown;
    core.decMark(mark);
    ks.tUp = t;
    practiceEdge(false, t, mark);
  }
  ks.down = down; ks.seen = true;
  audio.key(down);
  $("pad") && $("pad").classList.toggle("down", down && S().keyer === 0);
  for (const c of out) emit(c);
}
function idle(now) {
  if (ks.seen && !ks.down) { const c = core.decIdle(now - ks.tUp); if (c) emit(c); }
}
function emit(c) {
  if (page === "key") keyEmit(c);
  else if (page === "practice" && c !== " ") practiceKeyed(c);
}
// the pad: a straight key, or two paddles (left dit, right dah; swapped by PADDLES) run through MORSE's keyer
function paddle(i, on) {
  ks.paddles[i] = on;
  if (S().keyer === 0) { edge(ks.paddles[0] || ks.paddles[1], performance.now()); return; }
  if (!ks.timer) { ks.kt = performance.now(); ks.timer = setInterval(keyerRun, 4); keyerRun(); }
}
function keyerRun() {                    // mo_keyer, a millisecond a call, caught up to now
  const now = performance.now(), sw = S().swap;
  const dit = ks.paddles[sw ? 1 : 0], dah = ks.paddles[sw ? 0 : 1];
  while (ks.kt < now) {
    const k = !!core.keyerTick(dit, dah);
    if (k !== ks.kdown) { ks.kdown = k; edge(k, ks.kt); }
    ks.kt += 1;
  }
  if (!dit && !dah && !ks.kdown && now - ks.tUp > 3 * core.ditMs(S().send)) { clearInterval(ks.timer); ks.timer = 0; }
}
function keyerReset() { core.keyerInit(S().keyer, core.ditMs(S().send)); ks.kdown = false; }
// the pad stays put while it is wanted (a redraw under a finger would lose the key-up)
function setPad(want, enabled) {
  const w = $("padwrap"), cur = $("pad"), sig = `${S().keyer}${S().swap}`;
  if (!want) { if (cur) { ks.paddles = [false, false]; if (ks.down) edge(false, performance.now()); } w.innerHTML = ""; return; }
  if (cur && cur.dataset.sig === sig) { cur.classList.toggle("off", !enabled); return; }
  w.innerHTML = ""; w.appendChild(padEl(enabled));
}
function padEl(enabled) {
  const two = S().keyer !== 0, d = document.createElement("div");
  d.className = "pad" + (enabled ? "" : " off"); d.id = "pad"; d.dataset.sig = `${S().keyer}${S().swap}`;
  const lab = two ? (S().swap ? ["DAH", "DIT"] : ["DIT", "DAH"]) : ["TAP AND HOLD"];
  lab.forEach((l, i) => { const h = document.createElement("div"); h.textContent = l; h.dataset.i = i; d.appendChild(h); });
  const active = new Map();
  const half = e => two ? (e.clientX - d.getBoundingClientRect().left < d.clientWidth / 2 ? 0 : 1) : 0;
  d.addEventListener("pointerdown", e => {
    e.preventDefault(); audio.ensure();
    if (!keyAllowed()) return;
    d.setPointerCapture && d.setPointerCapture(e.pointerId);
    const i = half(e); active.set(e.pointerId, i);
    if (two) d.children[i].classList.add("down");
    paddle(i, true);
  });
  const up = e => {
    if (!active.has(e.pointerId)) return;
    const i = active.get(e.pointerId); active.delete(e.pointerId);
    if (two) d.children[i].classList.remove("down");
    if (![...active.values()].includes(i)) paddle(i, false);
  };
  d.addEventListener("pointerup", up); d.addEventListener("pointercancel", up);
  d.addEventListener("contextmenu", e => e.preventDefault());
  return d;
}
// a computer's keyboard too: space or . for a straight key / dit, / or - for dah (handy on a tablet with a keyboard)
const KEYMAP = { " ": 0, ".": 0, "/": 1, "-": 1, "ArrowLeft": 0, "ArrowRight": 1 };
addEventListener("keydown", e => {
  if (e.repeat || !(e.key in KEYMAP) || e.target.closest("select,input")) return;
  e.preventDefault(); audio.ensure();
  if (keyAllowed()) paddle(S().keyer === 0 ? 0 : KEYMAP[e.key], true);
});
addEventListener("keyup", e => { if (e.key in KEYMAP && !e.target.closest("select,input")) paddle(S().keyer === 0 ? 0 : KEYMAP[e.key], false); });
function keyAllowed() { return page === "key" || capturing(); }

// --------------------------------------------------------------------------------------------- the pages ---
let page = "practice";
function show(p) {
  if (page === "practice" && p !== "practice") practiceLeave();
  if (page === "listen" && p !== "listen") listenStop();
  page = p;
  for (const b of $("nav").children) b.setAttribute("aria-selected", b.dataset.pg === p);
  for (const s of document.querySelectorAll("main > section")) s.hidden = s.id !== "pg-" + p;
  if (p === "key") keyEnter();
  render();
}
$("nav").addEventListener("click", e => { const b = e.target.closest("button"); if (b) { audio.ensure(); show(b.dataset.pg); } });
function render() {
  ({ practice: renderPractice, listen: renderListen, key: renderKey, chart: renderChart, settings: renderSettings })[page]();
  const s = S(), ln = core.ln;
  $("hdr").textContent = `${ln.open} open · round ${ln.round} · hear ${s.wpm}/${s.eff} · send ${s.send}`;
}
let toastT = 0;
function toast(t) { const e = $("toast"); e.textContent = t; e.classList.add("show"); clearTimeout(toastT); toastT = setTimeout(() => e.classList.remove("show"), 2200); }
function h(html) { const t = document.createElement("template"); t.innerHTML = html.trim(); return t.content; }
function esc(s) { return String(s).replace(/[&<>"]/g, c => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" })[c]); }
function codeStr(c) { return core.pattern(c).replace(/\./g, "·").replace(/-/g, "–"); }

// ------------------------------------------------------------------------------------------------ PRACTICE ---
// app.c's pr and practice_*: PS_START, PS_CARD, PS_RESULT, PS_DONE
const pr = { ps: "start", test: 0, active: false, opened: false, hand: false, unit0: 0, am: [], ag: [], pend: 0, aUp: 0,
             read: "", j: null, hasJ: false, res: 0, got: "", until: 0, answerOk: false, peek: false, ask: false, ratio10: 0 };
const cardKey = c => c.type === CARD.HEAR_KEY || c.type === CARD.SEE_KEY;
const cardSounds = c => c.type === CARD.NEW || c.type === CARD.HEAR_CHOOSE || c.type === CARD.HEAR_KEY || c.type === CARD.LIGHT_CHOOSE;
function capturing() { const c = core.card(); return page === "practice" && pr.ps === "card" && c && cardKey(c) && pr.answerOk; }

function handReady() {                   // app.c hand_ready
  const send = core.ditMs(S().send);
  if (strict()) core.decInit(send, send);
  else if (!pr.hand) { const d = Math.floor((core.ditMs(S().wpm) + send) / 2); core.decInit(d, d); pr.hand = true; }
  else core.decRestart();
  ks.seen = false;
  pr.unit0 = core.decUnit(); pr.am = []; pr.ag = []; pr.hasJ = false; pr.read = "";
}
function practiceEdge(down, t, ms) {     // app.c practice_edge
  if (!capturing()) return;
  if (down) pr.pend = pr.am.length ? Math.min(ms, 0xFFFF) : 0;
  else {
    if (pr.am.length < 8) { pr.am.push(Math.min(Math.round(ms), 0xFFFF)); pr.ag.push(Math.round(pr.pend)); }
    pr.aUp = t;
  }
  const k = $("kdots"); if (k) k.textContent = keyNote();   // not a redraw: the pad is under a finger
}
function keyNote() {
  return pr.answerOk ? (pr.am.length ? "●".repeat(pr.am.length) : (strict() ? "key it at your send speed" : "your speed: the rhythm counts")) : "listen first";
}
function present() {                     // app.c card_present
  const c = core.card();
  Object.assign(pr, { answerOk: false, got: "", am: [], ag: [], hasJ: false, peek: false });
  if (!c) {
    pr.active = false; pr.ps = "done";
    if (core.round.place) { pr.ask = core.placeCount() > 0; pr.opened = false; }
    else { pr.opened = core.finish(); save(); if (pr.opened) toast("NEW LETTERS"); }
    render(); return;
  }
  pr.ps = "card";
  if (c.type === CARD.NEW) playText(`${c.char} ${c.char}`);
  else if (cardSounds(c)) playText(c.char);
  else audio.stop();
  render();
}
function practiceStart() {               // app.c practice_start
  audio.ensure();
  let flags = (muted() ? F_MUTED : 0) | (visual() ? F_VISUAL : 0);
  if (pr.test) {
    if (core.ln.open >= N) { toast("ALL OPEN: NOTHING TO TEST"); return; }
    flags |= F_PLACE;
  }
  core.build(flags, (Date.now() * 2654435761 + core.ln.round) >>> 0);
  Object.assign(pr, { active: true, opened: false, hand: false, ask: false });
  present();
}
function practiceAnswer(res) {           // app.c practice_answer
  const c = core.card();
  if (!c || pr.ps !== "card") return;
  if (c.type === CARD.NEW) { core.answer(RIGHT); audio.stop(); present(); return; }
  pr.res = res; pr.peek = false;
  pr.hasJ = cardKey(c) && pr.am.length > 0;
  if (pr.hasJ) pr.j = core.judge(pr.am, pr.ag, c.char, pr.unit0);
  pr.ratio10 = cardKey(c) ? core.decRatio10() : 0;
  pr.card = c;
  core.answer(res);
  pr.ps = "result"; pr.answerOk = false;
  audio.stop();
  if (res !== RIGHT) playText(c.char);   // how it should have gone
  pr.until = performance.now() + (res !== RIGHT ? 2800 : pr.hasJ && pr.j.score < 85 ? 1800 : 900);
  render();
}
function practiceJudge() {               // app.c practice_judge: by its own rhythm
  const c = core.card(); if (!c) return;
  const j = core.judge(pr.am, pr.ag, c.char, pr.unit0);
  if (j.verdict !== MJ_WRONG) core.teach(pr.am, pr.ag, c.char);
  practiceAnswer(j.verdict === MJ_RIGHT ? RIGHT : j.verdict === MJ_ALMOST ? ALMOST : WRONG);
}
function practiceKeyed(ch) {             // app.c practice_keyed
  const k = core.card();
  if (pr.ps !== "card" || !k || !cardKey(k) || !pr.answerOk) return;
  if (pr.read.length < 3) pr.read += ch;
  pr.got = ch;
  if (strict()) practiceAnswer(ch === k.char ? RIGHT : WRONG);
}
function practiceChoose(i) {
  const c = core.card();
  if (pr.ps !== "card" || !c || !core.choose(c.type) || i > 3) return;
  pr.got = ORDER[c.opt[i]];
  practiceAnswer(i === c.right ? RIGHT : WRONG);
}
function practiceLeave() {               // app.c practice_leave
  if (pr.active) { pr.active = false; if (!core.round.place) { core.finish(); save(); } }
  audio.stop(); pr.ps = "start"; pr.ask = false;
}
function practiceStep(now) {             // app.c practice_step
  const c = core.card();
  if (pr.ps === "card" && c && cardKey(c) && !pr.answerOk && !audio.playing()) {
    pr.answerOk = true; handReady(); renderPractice();
  }
  if (!strict() && capturing() && pr.am.length && !ks.down) {
    const len = core.x.code_len(core.code(c.char)), u = pr.unit0;
    let wait;
    if (pr.am.length < len) wait = Math.max(6 * u, 900);
    else wait = Math.max(3 * u, 2 * core.decUnit() + 20);
    if (now - pr.aUp >= wait) practiceJudge();
  }
  if (pr.ps === "result" && now >= pr.until && !audio.playing()) present();
}

const RES = ["NO", "RIGHT", "ALMOST"], RES_CLS = ["bad", "ok", "meh"];
const GRADE = ["GREAT", "GOOD", "CLOSE", "OFF"];
const gradeOf = s => s >= 85 ? 0 : s >= 65 ? 1 : s >= 40 ? 2 : 3;
const HINT = ["clean rhythm", "dahs short: hold a dah for 3 dits", "dits long: tap a dit short", "a mark too many",
  "a mark missing, or the letter split", "gaps inside long: run it together", "keep marks of a kind the same length",
  "dahs long: 3 dits is enough"];

function bars(marks, gaps, want, dit, den, you, res) {   // app.c bars, as SVG: 0..1000 wide
  const k = core.code(want), n = marks.length, len = core.x.code_len(k);
  let x = 0, s = "";
  for (let i = 0; i < n; i++) {
    const dah = len === n ? core.x.code_elem(k, i) : marks[i] >= 2 * dit;
    let col = dah ? "var(--rx)" : "var(--own)";
    if (i) {
      const g = gaps[i] * 1000 / den;
      if (you && gaps[i] * 10 >= dit * 25) s += `<rect x="${x}" y="30" width="${g}" height="5" fill="var(--bad)"/>`;
      x += g;
    }
    const w = Math.max(8, marks[i] * 1000 / den);
    if (you && len === n && res !== RIGHT && (marks[i] * 10 >= dit * 20) !== !!dah) col = "var(--bad)";
    s += `<rect x="${x}" y="6" width="${Math.min(w, 1000 - x)}" height="20" rx="3" fill="${col}"/>`;
    x += w;
  }
  return `<svg class="bars" viewBox="0 0 1000 40" preserveAspectRatio="none">${s}</svg>`;
}
function resultKeyed(c) {                // app.c result_keyed
  const k = core.code(c.char), n = core.x.code_len(k), dit = pr.j.dit || 100, g = gradeOf(pr.j.score);
  const sm = [], sg = [];
  let sh = 0, yo = 0;
  for (let i = 0; i < n && i < 8; i++) { sm.push(core.x.code_elem(k, i) ? 3 * dit : dit); sg.push(i ? dit : 0); sh += sm[i] + sg[i]; }
  pr.am.forEach((m, i) => yo += m + pr.ag[i]);
  const den = Math.max(sh, yo, 1);
  return `<div class="row"><span class="mid ${g < 2 ? "ok" : g === 2 ? "meh" : "bad"}">${GRADE[g]} ${pr.j.score}</span>
    <span class="grow"></span>${pr.read ? `<span class="dim">read as ${esc(pr.read)}</span>` : ""}</div>
    <p class="dim" style="margin:10px 0 0">SHOULD</p>${bars(sm, sg, c.char, dit, den, false, pr.res)}
    <p class="dim" style="margin:4px 0 0">YOU</p>${bars(pr.am, pr.ag, c.char, dit, den, true, pr.res)}
    <p class="${pr.j.hint ? "dim" : "ok"}">${HINT[pr.j.hint]}</p>
    ${pr.res === ALMOST ? `<p class="dim">close: no box moved, asked again</p>` : ""}`;
}
function renderPractice() {
  if (page !== "practice") return;
  const el = $("pg-practice"), ln = core.ln, c = core.card();
  let html = "";
  setPad(pr.ps === "card" && c && cardKey(c), pr.answerOk);
  if (pr.ps === "done" && core.round.place) {
    const known = ORDER.split("").filter((_, i) => core.known(i)).join(" ");
    html = `<div class="card center"><p class="mid">TEST DONE</p><p class="dim">${core.placeCount()} of ${core.round.n} known</p>
      <p class="mid ok">${known || "&nbsp;"}</p>
      ${pr.ask ? `<p>Mark these known? (recognise box 3, send box 2; each group all right opens)</p>
        <div class="row" style="justify-content:center"><button class="b primary" data-a="placeyes">Yes</button>
        <button class="b" data-a="placeno">No</button></div>` : ""}</div>${startButtons()}`;
  } else if (pr.ps === "start" || pr.ps === "done") {
    let top;
    if (pr.ps === "done") {
      const r = core.round;
      top = `<p class="mid">ROUND ${ln.round} DONE</p><p class="dim">${r.ok} right · ${r.miss} missed · +${r.up}</p>
        ${pr.opened ? `<p class="mid ok">NEW: ${ORDER.slice(ln.open - GROUP, ln.open).split("").join(" ")}</p>` : ""}`;
    } else
      top = `<p class="mid">ROUND ${ln.round + 1}</p><p class="dim">${core.dueCount()} due · ${core.newCount()} new · ${ln.open} open</p>`;
    const waits = ORDER.slice(0, ln.open).split("").filter((_, i) => ln.box[i][REC] < 2 || ln.box[i][SEND] < 1);
    html = `<div class="card center">${top}<p class="${ln.streak ? "ok" : "dim"}">streak ${ln.streak}</p>
      ${ln.open < N && waits.length ? `<p class="dim">next group waits on ${waits.slice(0, 8).join(" ")}</p>` : ""}</div>
      ${startButtons()}
      ${!hadRecord && ln.round === 0 && pr.ps === "start" ? `<div class="card"><h2>New here?</h2><p class="dim">This is MORSE's
        PRACTICE from the FM-1: flash cards in Koch order, by sound. Already learning on the FM-1? Back it up on
        <a href="../">MORSE on the web</a> and load the file in SETTINGS.</p></div>` : ""}`;
  } else if (pr.ps === "result") {
    const a = pr.card;
    html = `<p class="dim">${core.round.at} / ${core.round.n}</p><div class="card" data-a="next">`;
    if (pr.hasJ)
      html += `<div class="row"><span class="big ${RES_CLS[pr.res]}" style="font-size:2.4rem">${RES[pr.res]}</span>
        <span class="grow"></span><span class="big" style="font-size:2.4rem">${esc(a.char)}</span></div>${resultKeyed(a)}`;
    else
      html += `<div class="center"><p class="mid ${RES_CLS[pr.res]}">${RES[pr.res]}</p><p class="big">${esc(a.char)}</p>
        ${visual() || pr.peek ? `<p class="code">${codeStr(a.char)}</p>` : ""}
        ${pr.res !== RIGHT && pr.got ? `<p class="bad">you: ${esc(pr.got)}</p>` : ""}</div>`;
    html += `</div><div class="row"><button class="b primary grow" data-a="next">Next</button>
      ${!visual() && !pr.hasJ ? `<button class="b" data-a="peek">Code</button>` : ""}</div>`;
  } else if (c) {
    const r = core.round;
    html = `<p class="dim">${r.at + 1} / ${r.n}${c.again ? " · again" : ""}</p><div class="card center">`;
    const lamp = muted() ? `<div class="lamp" id="lamp"></div>` : "";   // by sound nothing shows the rhythm (learn.h)
    switch (c.type) {
      case CARD.NEW:
        html += `<p class="dim">NEW LETTER</p><p class="big">${esc(c.char)}</p>
          ${visual() || pr.peek ? `<p class="code">${codeStr(c.char)}</p>` : ""}${lamp}`; break;
      case CARD.HEAR_CHOOSE: html += `<p class="mid">WHICH ONE?</p><p class="dim">listen</p>${lamp}`; break;
      case CARD.LIGHT_CHOOSE: html += `<p class="mid">WHICH ONE?</p><p class="dim">watch the light</p>${lamp}`; break;
      case CARD.CODE_CHOOSE: html += `<p class="mid">WHICH ONE?</p><p class="code">${codeStr(c.char)}</p>`; break;
      case CARD.HEAR_KEY: html += `<p class="mid">KEY WHAT YOU HEAR</p>${lamp}`; break;
      case CARD.SEE_KEY: html += `<p class="dim">KEY IT</p><p class="big">${esc(c.char)}</p>
        ${visual() ? `<p class="code">${codeStr(c.char)}</p>` : ""}`; break;
    }
    if (cardKey(c)) html += `<p class="dim" id="kdots" style="min-height:1.5em">${keyNote()}</p>`;
    html += `</div>`;
    if (core.choose(c.type))
      html += `<div class="choices">${c.opt.map((o, i) => `<button data-a="pick" data-i="${i}">${esc(ORDER[o])}</button>`).join("")}</div>`;
    const again = cardSounds(c) ? `<button class="b" data-a="again">Again</button>` : "";
    if (c.type === CARD.NEW)
      html += `<div class="row">${again}${!visual() ? `<button class="b" data-a="peek">Code</button>` : ""}
        <button class="b primary grow" data-a="new">Next</button></div>`;
    else if (cardKey(c)) html += `<div class="row">${again}<span class="grow"></span><button class="b" data-a="dunno">Don't know</button></div>`;
    else html += `<div class="row">${again}</div>`;
  }
  el.innerHTML = html;
}
function startButtons() {
  return `<div class="row"><button class="b primary grow" data-a="start">${pr.test ? "Start the test" : "Start a round"}</button>
    <button class="b" data-a="mode">${pr.test ? "A round instead" : "Placement test"}</button></div>
    <p class="dim center" style="font-size:.85rem">${pr.test ? "the next 10 letters: skip the ones you know" :
      `${S().cards} cards · learning by ${visual() ? "sight" : "sound"}${muted() ? " · sound off: watch the light" : ""}`}</p>`;
}
$("pg-practice").addEventListener("click", e => {
  const b = e.target.closest("[data-a]"); if (!b) return;
  audio.ensure();
  const c = core.card();
  switch (b.dataset.a) {
    case "start": practiceStart(); break;
    case "mode": pr.test ^= 1; pr.ps = "start"; render(); break;
    case "placeyes": { const n = core.placeApply(); save(); pr.ask = false; toast(`${n} MARKED KNOWN`); render(); break; }
    case "placeno": pr.ask = false; toast("NOTHING CHANGED"); render(); break;
    case "again": if (c && pr.ps === "card") { pr.answerOk = false; c.type === CARD.NEW ? playText(`${c.char} ${c.char}`) : playText(c.char); renderPractice(); } break;
    case "peek": pr.peek = !pr.peek; renderPractice(); break;
    case "new": practiceAnswer(RIGHT); break;
    case "dunno": practiceAnswer(WRONG); break;
    case "pick": practiceChoose(+b.dataset.i); break;
    case "next": if (pr.ps === "result") { pr.until = 0; audio.stop(); } break;
  }
});

// -------------------------------------------------------------------------------------------------- LISTEN ---
const ls = { on: false, line: "", gloss: "", prev: [], reveal: false };
function listenStart() { audio.ensure(); core.listenInit((Date.now() * 2654435761 + 1) >>> 0); ls.on = true; ls.prev = []; listenNext(); }
function listenNext() {
  if (ls.line) { ls.prev.unshift({ t: ls.line, g: ls.gloss }); ls.prev.length = Math.min(ls.prev.length, 4); }
  ls.line = core.listenLine(S().prog); ls.gloss = core.listenGloss();
  playText(ls.line + "  ");
  renderListen();
}
function listenStop() { if (ls.on) audio.stop(); ls.on = false; ls.line = ""; }
function renderListen() {
  if (page !== "listen") return;
  const s = S(), hide = lhide() && !ls.reveal;
  setPad(false);
  $("pg-listen").innerHTML = `<div class="card"><div class="row" style="margin:0">
      <select id="prog" class="grow" style="font:inherit;padding:10px;border-radius:10px;border:1px solid var(--line);background:var(--bg);color:var(--fg)">
      ${["Koch groups of your letters", "Common words", "A QSO", "CW abbreviations", "Sayings"].map((t, i) => `<option value="${i}" ${i === s.prog ? "selected" : ""}>${t}</option>`).join("")}
      </select></div>
      ${muted() ? `<div class="lamp" id="lamp" style="margin-top:14px"></div>` : ""}
      <p class="listen-text ${hide ? "hidden-text" : ""}"><span class="now">${esc(ls.line)}</span></p>
      ${ls.gloss && !hide ? `<p class="dim">${esc(ls.gloss)}</p>` : ""}
      ${ls.prev.map(p => `<p class="dim ${hide ? "hidden-text" : ""}" style="margin:2px 0;font-family:ui-monospace,monospace">${esc(p.t)}${p.g ? ` — ${esc(p.g)}` : ""}</p>`).join("")}
    </div>
    <div class="row"><button class="b primary grow" data-a="play">${ls.on ? "Stop" : "Play"}</button>
      <button class="b" data-a="hide">${lhide() ? "Show text" : "Hide text"}</button>
      ${lhide() ? `<button class="b" data-a="peek">Peek</button>` : ""}</div>
    <p class="dim center" style="font-size:.85rem">hear ${s.wpm} WPM, spaced to ${s.eff} · ${core.ln.open} letters open</p>`;
  $("prog").onchange = e => { setChange(x => x.prog = +e.target.value); if (ls.on) { listenStop(); listenStart(); } };
}
$("pg-listen").addEventListener("click", e => {
  const b = e.target.closest("[data-a]"); if (!b) return;
  if (b.dataset.a === "play") { ls.on ? listenStop() : listenStart(); renderListen(); }
  if (b.dataset.a === "hide") { setChange(x => x.look ^= 32); renderListen(); }
  if (b.dataset.a === "peek") { ls.reveal = true; renderListen(); setTimeout(() => { ls.reveal = false; renderListen(); }, 1500); }
});

// ----------------------------------------------------------------------------------------------------- KEY ---
const kp = { text: "" };
function keyEnter() { const d = core.ditMs(S().send); core.decInit(d, d); ks.seen = false; keyerReset(); }
function keyEmit(c) { kp.text = (kp.text + c).slice(-240); renderKeyText(); }
function renderKeyText() {
  const t = $("ktext"); if (!t) return;
  t.textContent = kp.text || "…";
  $("kstat").textContent = `${core.decWpm()} WPM · dah:dit ${(core.decRatio10() / 10).toFixed(1)}`;
}
function renderKey() {
  if (page !== "key") return;
  $("pg-key").innerHTML = `<div class="card"><p class="listen-text" id="ktext"></p><p class="dim" id="kstat"></p></div>
    <div class="row"><button class="b" data-a="clear">Clear</button><span class="grow"></span>
    <span class="dim">${["straight key", "iambic A", "iambic B"][S().keyer]} · send ${S().send} WPM</span></div>`;
  setPad(true, true);
  renderKeyText();
}
$("pg-key").addEventListener("click", e => { const b = e.target.closest("[data-a]"); if (b && b.dataset.a === "clear") { kp.text = ""; keyEnter(); renderKeyText(); } });

// --------------------------------------------------------------------------------------------------- CHART ---
const ch = { sel: 0, miss: false };
function missLevel(i) {                  // by misses: 5 never missed .. 0 missed a lot (-1: never asked)
  const a = core.missPct(i, REC), b = core.missPct(i, SEND);
  if (a < 0 && b < 0) return -1;
  const p = Math.max(a, b);
  return p < 5 ? 5 : p < 12 ? 4 : p < 20 ? 3 : p < 30 ? 2 : p < 45 ? 1 : 0;
}
function renderChart() {
  if (page !== "chart") return;
  setPad(false);
  const ln = core.ln, st = core.stats, i = ch.sel, c = ORDER[i];
  const cells = ORDER.split("").map((x, k) => {
    const open = k < ln.open, lv = ch.miss ? missLevel(k) : core.level(k);
    const bg = !open ? "var(--closed)" : lv < 0 ? "var(--l0)" : `var(--l${lv})`;
    const dot = open && (core.hard(k, REC) || core.hard(k, SEND)) ? "<i></i>" : "";
    return `<button class="${open ? "" : "closed"} ${k === i ? "sel" : ""}" style="background:${bg}" data-i="${k}">${esc(x)}${dot}</button>`;
  }).join("");
  const pct = d => { const p = core.missPct(i, d); return p < 0 ? "-" : `${p}% of ${st[i][d].asked}`; };
  const lost = st[i][REC].lapses + st[i][SEND].lapses;
  $("pg-chart").innerHTML = `<div class="chart" id="chart">${cells}</div>
    <div class="legend"><span class="dim">${ch.miss ? "missed a lot" : "level 0"}</span>${[0, 1, 2, 3, 4, 5].map(l => `<span style="background:var(--l${l})"></span>`).join("")}
      <span class="dim">${ch.miss ? "never" : "5"}</span></div>
    <p class="dim" style="font-size:.8rem;margin:2px 0 0"><span class="bad">●</span> missed often lately: comes back sooner</p>
    <div class="card"><div class="row" style="margin:0"><span class="big" style="font-size:2.6rem">${esc(c)}</span>
      <span class="code" style="font-size:1.3rem">${codeStr(c)}</span><span class="grow"></span>
      <button class="b" data-a="hear">Hear</button></div>
      <p>${i < ln.open ? `box: hear ${ln.box[i][REC]} · send ${ln.box[i][SEND]} (level ${core.level(i)})` : "not open yet"}</p>
      <p class="dim">missed: hear ${pct(REC)} · send ${pct(SEND)}${lost ? ` · lost ${lost}` : ""}</p></div>
    <div class="row"><button class="b" data-a="miss">${ch.miss ? "Colour by level" : "Colour by misses"}</button></div>
    <p class="dim" style="font-size:.85rem">${ln.answered} answered, ${ln.right} right since the start. Editing the chart by
      hand is on the FM-1 (CHART, REC) for now.</p>`;
}
$("pg-chart").addEventListener("click", e => {
  const b = e.target.closest("button"); if (!b) return;
  audio.ensure();
  if (b.dataset.i !== undefined) { ch.sel = +b.dataset.i; playText(ORDER[ch.sel]); renderChart(); }
  else if (b.dataset.a === "hear") playText(ORDER[ch.sel]);
  else if (b.dataset.a === "miss") { ch.miss = !ch.miss; renderChart(); }
});

// ------------------------------------------------------------------------------------------------ SETTINGS ---
const PRESETS = [["START", 20, 8, 10], ["BUILDING", 20, 13, 13], ["QSO", 22, 18, 18], ["FAST", 25, 25, 22]];   // app.c PRESET
let picked = null;
function stepper(id, v, lo, hi, step = 1) {
  return `<span class="stepper"><button data-s="${id}" data-d="${-step}" ${v <= lo ? "disabled" : ""}>−</button><output>${v}</output>
    <button data-s="${id}" data-d="${step}" ${v >= hi ? "disabled" : ""}>+</button></span>`;
}
function sel(id, v, opts) { return `<select data-k="${id}">${opts.map((o, i) => `<option value="${i}" ${i === v ? "selected" : ""}>${o}</option>`).join("")}</select>`; }
function renderSettings() {
  if (page !== "settings") return;
  setPad(false);
  const s = S(), p = PRESETS.findIndex(x => x[1] === s.wpm && x[2] === s.eff && x[3] === s.send);
  const standalone = matchMedia("(display-mode: standalone)").matches || navigator.standalone;
  $("pg-settings").innerHTML = `<div class="card">
    <label class="f">Preset ${sel("preset", p, PRESETS.map(x => `${x[0]} (${x[1]}/${x[2]}, send ${x[3]})`).concat(p < 0 ? ["custom"] : []))}</label>
    <label class="f">Hear speed (WPM) ${stepper("wpm", s.wpm, 5, 40)}</label>
    <label class="f">Spacing (Farnsworth) ${stepper("eff", s.eff, 5, s.wpm)}</label>
    <label class="f">Send speed ${stepper("send", s.send, 5, 40)}</label>
    <label class="f">Pitch (Hz) ${stepper("pitch", s.pitch, 300, 1200, 10)}</label>
    <label class="f">Sound ${sel("sound", s.flags & 1, ["on", "off: watch the light"])}</label>
    <label class="f">Cards a round ${stepper("cards", s.cards, 10, 30)}</label>
    <label class="f">Learn by ${sel("learn", s.learn, ["sound (recommended)", "sight"])}</label>
    <label class="f">Judging ${sel("judging", s.look >> 7 & 1, ["by rhythm", "strict (send speed)"])}</label>
    <label class="f">Key ${sel("keyer", s.keyer, ["straight", "iambic A", "iambic B"])}</label>
    ${s.keyer ? `<label class="f">Paddles ${sel("swap", s.swap, ["left dit", "left dah"])}</label>` : ""}
  </div>
  <div class="card"><h2>Progress file</h2>
    <p class="dim">The same file as the FM-1's backup on <a href="../">MORSE on the web</a> and <code>morse backup</code>:
      every letter's two boxes, the rounds, the settings. Load it there to take this progress to the FM-1, or load the
      FM-1's file here.</p>
    <div class="row"><button class="b primary" data-a="export">Save the progress</button>
      <label class="b file">Load a file<input type="file" id="file" accept=".json,application/json"></label></div>
    ${picked ? `<div class="card" style="margin-top:12px"><p><b>${esc(picked.name)}</b>${picked.f.firmware ? ` (MORSE ${esc(picked.f.firmware)})` : ""}</p>
      <p class="dim">${esc(picked.f.summary)}${picked.f.settings ? "; with settings" : ""}${picked.f.stats ? "; with per-letter stats" : "; no per-letter stats (these here stay)"}</p>
      <p>Load it over the progress here?</p>
      <div class="row"><button class="b primary" data-a="load">Load</button><button class="b" data-a="cancel">Cancel</button></div></div>` : ""}
  </div>
  <div class="card"><h2>Start over</h2><p class="dim">Every letter new again, the stats gone. The settings stay.</p>
    <div class="row"><button class="b" data-a="reset" style="border-color:var(--bad);color:var(--bad)">Reset all progress</button></div></div>
  <div class="card dim" style="font-size:.88rem">
    ${standalone ? "" : `<p><b>Install:</b> Android Chrome: menu, <i>Install app</i>. iPhone Safari: Share, <i>Add to Home Screen</i>. It then works offline.</p>`}
    <p>MORSE practice ${VERSION}: MORSE ${CORE_VERSION}'s course (learn.c, morse.c, content.c in WebAssembly), its record
      kept here byte for byte. <a href="https://github.com/daviddwlee84/fm1-morse">fm1-morse</a>, GPL-3.0-or-later.</p></div>`;
  $("file").onchange = async e => {
    const f = e.target.files[0]; if (!f) return;
    try { picked = { name: f.name, f: fromJSON(core, await f.text()) }; }
    catch (err) { picked = null; toast(err.message); }
    renderSettings();
  };
}
$("pg-settings").addEventListener("click", async e => {
  const b = e.target.closest("button"); if (!b) return;
  if (b.dataset.s) {
    const k = b.dataset.s, d = +b.dataset.d;
    setChange(s => { s[k] += d; if (k === "wpm" && s.eff > s.wpm) s.eff = s.wpm; });
    if (k === "send") keyerReset();
    renderSettings(); render();
    return;
  }
  switch (b.dataset.a) {
    case "export": await exportFile(); break;
    case "load":
      practiceLeave(); listenStop();
      try { applyFile(core, picked.f); save(); toast(picked.f.settings ? "LOADED: PROGRESS + SETTINGS" : "PROGRESS LOADED"); }
      catch (err) { toast(err.message); }
      picked = null; render(); break;
    case "cancel": picked = null; toast("NOT LOADED"); renderSettings(); break;
    case "reset":
      if (confirm("Reset all progress? Every letter new again.")) { practiceLeave(); core.progressReset(); save(); toast("ALL RESET"); render(); }
      break;
  }
});
$("pg-settings").addEventListener("change", e => {
  const k = e.target.dataset.k; if (!k) return;
  const v = +e.target.value;
  if (k === "preset") { const p = PRESETS[v]; if (p) setChange(s => { s.wpm = p[1]; s.eff = p[2]; s.send = p[3]; }); }
  else if (k === "sound") setChange(s => s.flags = (s.flags & ~1) | v);
  else if (k === "judging") setChange(s => s.look = (s.look & 0x7F) | v << 7);
  else setChange(s => s[k] = v);
  keyerReset(); render();
});
async function exportFile() {
  const text = toJSON(core, CORE_VERSION), d = new Date(), z = n => String(n).padStart(2, "0");
  const name = `fm1-morse-progress-${d.getFullYear()}-${z(d.getMonth() + 1)}-${z(d.getDate())}-${z(d.getHours())}${z(d.getMinutes())}.json`;
  const file = new File([text], name, { type: "application/json" });
  if (navigator.canShare && navigator.canShare({ files: [file] }) && matchMedia("(pointer: coarse)").matches) {
    try { await navigator.share({ files: [file], title: "MORSE progress" }); return; } catch (err) { if (err.name === "AbortError") return; }
  }
  const a = document.createElement("a"); a.href = URL.createObjectURL(file); a.download = name; a.click();
  setTimeout(() => URL.revokeObjectURL(a.href), 4000);
}

// ---------------------------------------------------------------------------------------------- the clock ---
function frame() {
  const now = performance.now();
  idle(now);
  if (page === "practice") practiceStep(now);
  if (page === "listen" && ls.on && !audio.playing()) listenNext();
  const lamp = $("lamp"); if (lamp) lamp.classList.toggle("on", audio.markNow());
  requestAnimationFrame(frame);
}
setInterval(() => { if (document.hidden) return; const now = performance.now(); idle(now); if (page === "practice") practiceStep(now); }, 25);
document.addEventListener("visibilitychange", () => {
  if (document.hidden) { if (page === "practice" && pr.active) { practiceLeave(); render(); } listenStop(); save(); }
});
keyerReset();
render();
requestAnimationFrame(frame);
if ("serviceWorker" in navigator && location.protocol !== "file:") navigator.serviceWorker.register("sw.js").catch(() => { });
