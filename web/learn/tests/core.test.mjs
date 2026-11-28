// SPDX-License-Identifier: GPL-3.0-or-later
// node web/learn/tests/core.test.mjs: core.wasm and the progress file against the owner's FM-1. rec-0.8.bin is the
// record MORSE 0.8 kept at 0xDA000 (bytes 0x100..0x218 of that sector in the full-flash backup of 2026-11-28,
// M-VAVE_FM-1 release backup-2026-11-28-pre-sloop-2.8.5); progress-0.8.json is the backup the unit sent at the
// same time (release progress-morse-2026-11-28-pre-sloop).
import { readFileSync } from "fs";
import assert from "assert/strict";
import { loadCore, N, REC, SEND, RIGHT, WRONG, CARD } from "../core.js";
import { toJSON, fromJSON, applyFile } from "../progress.js";

const here = new URL(".", import.meta.url);
const read = f => readFileSync(new URL(f, here));
const core = await loadCore(read("../core.wasm"));
let n = 0;
const t = (name, f) => { f(); n++; console.log("ok", name); };

t("record sizes are the FM-1's", () => { assert.equal(core.size, 1020); assert.equal(core.x.rec1_size(), 280); });

t("the unit's 0.8 record loads, stats empty", () => {
  assert.equal(core.recLoad(read("rec-0.8.bin")), true);
  assert.equal(core.ln.round, 61); assert.equal(core.ln.open, 30);
  assert.ok(core.stats.every(s => s[0].asked === 0 && s[1].asked === 0));
});

t("its file says what the unit's own backup says (but the stats)", () => {
  const a = JSON.parse(toJSON(core, "0.8")); delete a.stats;   // (JS puts the digits' keys first: order aside)
  assert.deepEqual(a, JSON.parse(read("progress-0.8.json")));
});

t("the 0.9 record goes out and back the same", () => {
  core.recLoad(read("rec-0.8.bin"));
  const b = core.recBytes();
  assert.equal(b.length, 1020);
  assert.deepEqual([...b.slice(0, 8)], [0x31, 0x52, 0x4F, 0x4D, 2, 0, 0xFC, 0x03]);   // MOR1, ver 2, size 1020
  assert.equal(core.recLoad(b), true);
  assert.deepEqual(core.recBytes(), b);
});

t("a record MORSE would refuse comes up as the defaults", () => {
  const b = core.recBytes(); b[20 + 4] = 99;                    // open 99
  assert.equal(core.recLoad(b), false); assert.equal(core.ln.open, 5); assert.equal(core.ln.round, 0);
  assert.equal(core.recLoad(new Uint8Array(100)), false);
});

t("a round runs, the stats count, and its file carries them back", () => {
  core.recLoad(read("rec-0.8.bin"));
  core.build(0, 12345);
  let asked = 0;
  for (let c; (c = core.card());) {
    if (c.type !== CARD.NEW) asked++;
    core.answer(asked === 1 ? WRONG : RIGHT);
  }
  core.finish();
  assert.equal(core.ln.round, 62);
  const total = core.stats.reduce((a, s) => a + s[REC].asked + s[SEND].asked, 0);
  assert.ok(total > 0 && total <= asked);
  const text = toJSON(core, "0.9"), stats = core.stats, ln = core.ln;
  core.recNew();
  applyFile(core, fromJSON(core, text));
  assert.deepEqual(core.stats, stats); assert.deepEqual(core.ln, ln);
});

t("a file without stats keeps the ones here (as the FM-1 does)", () => {
  const before = core.stats;
  applyFile(core, fromJSON(core, read("progress-0.8.json").toString()));
  assert.equal(core.ln.round, 61); assert.deepEqual(core.stats, before);
});

t("files MORSE would refuse are refused", () => {
  const d = JSON.parse(read("progress-0.8.json"));
  assert.throws(() => fromJSON(core, JSON.stringify({ ...d, open: 50 })));
  assert.throws(() => fromJSON(core, JSON.stringify({ ...d, chars: { ...d.chars, K: { recognise: 9 } } })));
  assert.throws(() => fromJSON(core, JSON.stringify({ ...d, settings: { ...d.settings, hear_wpm: 99 } })));
  assert.throws(() => fromJSON(core, JSON.stringify({ ...d, format: "x" })));
});

t("judging: a clean K at any speed, short dahs almost or wrong", () => {
  assert.equal(core.judge([60, 180, 60].map((x, i) => [180, 60, 180][i]), [0, 60, 60], "K", 60).verdict, 0);
  assert.equal(core.judge([400, 130, 400], [0, 130, 130], "K", 60).verdict, 0);
  assert.notEqual(core.judge([150, 110, 150], [0, 110, 110], "K", 110).verdict, 0);
});

t("timing: PARIS at 20 WPM, Farnsworth 8", () => {
  assert.equal(core.ditMs(20), 60);
  const tl = core.timeline("PARIS ", 60, core.gapMs(20, 8));
  assert.equal(tl.filter((_, i) => i % 2 === 0).reduce((a, b) => a + b, 0), 60 * 22);   // marks: 22 dits
});
console.log(`${n} passed`);
