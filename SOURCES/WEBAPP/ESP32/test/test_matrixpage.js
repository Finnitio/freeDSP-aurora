// Browser-facing contract checks for the hand-written Matrix page.
// Run: node SOURCES/WEBAPP/ESP32/test/test_matrixpage.js
const assert = require("assert");
const fs = require("fs");
const vm = require("vm");

const header = fs.readFileSync(
  "SOURCES/WEBAPP/ESP32/aurora/matrixpage.h",
  "utf8"
);
const script = header.match(/<script>\n([\s\S]*?)<\/script>/);
assert(script, "matrix page script not found");

let request;
const context = {
  console,
  document: {
    addEventListener() {},
    getElementById() { return { innerHTML: "", textContent: "" }; },
  },
  fetch(url, options) {
    request = { url, options };
    return Promise.resolve({
      ok: true,
      status: 200,
      text: () => Promise.resolve("{}"),
    });
  },
  setTimeout,
  clearTimeout,
};
vm.createContext(context);
vm.runInContext(script[1], context);

const realCoeffs = context.coeffs;
const realDraw = context.draw;

async function main() {
  context.activePreset = 2;
  context.draw = () => {};

  await context.sendLink("B", "mute", true);

  assert.strictEqual(request.url, "/matrix");
  assert.deepStrictEqual(JSON.parse(request.options.body), {
    linkGroup: "B",
    preset: 2,
    mute: true,
  });

  await context.sendLink("bus", "filters", false);
  assert.deepStrictEqual(JSON.parse(request.options.body), {
    linkGroup: "bus",
    preset: 2,
    filters: false,
  });

  await context.send("out", 3, { gain: -2 });
  assert.deepStrictEqual(JSON.parse(request.options.body), {
    ch: "out",
    idx: 3,
    preset: 2,
    gain: -2,
  });

  context.st = {
    buses: [{ gain: 12, gainBypass: false, slots: [] }],
    outs: [{ gain: -7.5, gainBypass: false, slots: [] }],
  };
  context.cf = { fs: 48000, bq: [[1, 0, 0, 0, 0]] };
  context.view = { ch: "bus", idx: 0 };
  assert.strictEqual(context.mag(1000), 12,
    "a Bus with no active filtering plots its positive Channel gain");

  context.view = { ch: "out", idx: 0 };
  assert.strictEqual(context.mag(1000), -7.5,
    "an Output with no active filtering plots its negative Channel gain");
  context.st.outs[0].gainBypass = true;
  assert.strictEqual(context.mag(1000), 0,
    "gain bypass plots unity without clearing the stored gain");
  assert.strictEqual(context.st.outs[0].gain, -7.5);
  context.st.outs[0].gainBypass = false;
  assert.strictEqual(context.mag(1000), -7.5,
    "disabling bypass restores the stored gain contribution");

  context.st.buses[0].gain = 3;
  context.view = { ch: "bus", idx: 0 };
  context.cf.bq = [[0.5000000596046448, 0, 0, 0, 0]];
  assert(Math.abs(context.mag(1000) - -3.0205988778409525) < 1e-12,
    "effective gain combines with the firmware-provided quantized Slot response");

  const scheduled = [];
  context.setTimeout = (fn) => { scheduled.push(fn); return scheduled.length; };
  context.coeffs = () => {};
  context.st.gainmin = -80;
  context.st.gainmax = 12;
  context.st.choices = [{ c: 1 }];
  context.st.peq = { fmin: 20, fmax: 20000 };
  context.drawF();
  scheduled.shift()();
  context.st.buses[0].gain = 4;
  context.drawF();
  assert.strictEqual(scheduled.length, 1,
    "a live gain state change schedules a fresh plot response without a Slot edit");
  scheduled.shift()();
  context.st.buses[0].gainBypass = true;
  context.drawF();
  assert.strictEqual(scheduled.length, 1,
    "toggling gain bypass schedules a fresh plot response without a Slot edit");

  // A coeffs reply still in flight when the user opens another Channel belongs to
  // the old Channel; plotting it would combine its biquads with the new Channel's gain.
  context.coeffs = realCoeffs;
  context.plot = () => {};
  context.document.getElementById = () => ({ innerHTML: "", textContent: "", style: {} });
  context.st.buses[0].name = "Bus 1";
  Object.assign(context.st.outs[0], { name: "Out 1", delayUs: 0, delaySamples: 0, delayMm: 0 });
  let reply;
  context.fetch = (url) => {
    request = { url };
    return Promise.resolve({ json: () => new Promise((res) => { reply = res; }) });
  };
  context.openF("bus", 0);
  context.coeffs();
  assert.strictEqual(request.url, "/matrix/coeffs?ch=bus&idx=0");
  await new Promise((res) => setImmediate(res));
  context.openF("out", 0);
  reply({ fs: 48000, bq: [[2, 0, 0, 0, 0]] });
  await new Promise((res) => setImmediate(res));
  assert.strictEqual(context.cf, null,
    "a coeffs reply for the previously opened Channel is not plotted for the new one");
  await commitTests();
  console.log("matrix page tests passed");
}

// ---- Commit: text fields send on Enter, Tab or click outside, never while typing.
// A tiny stand-in for the browser: the inputs the page renders are parsed from its HTML and
// their inline handlers run in the page's context, as a browser would run them.
const flush = () => new Promise((res) => setImmediate(res));
let made = {};
const els = {};
function element(id) {
  if (els[id]) return els[id];
  let html = "";
  return (els[id] = {
    id, textContent: "", style: {}, value: "",
    get innerHTML() { return html; },
    set innerHTML(h) {  // a rebuild replaces the inputs it held
      for (const m of html.matchAll(/<input\b([^>]*)>/g)) delete made[attrs(m[1]).id];
      html = h;
    },
  });
}
function attrs(tag) {
  const a = {};
  for (const m of tag.matchAll(/([\w-]+)=(['"])(.*?)\2/g)) a[m[1]] = m[3];
  return a;
}
function handler(code) {
  return code ? vm.runInContext("(function(event){" + code + "})", context) : null;
}
function rendered() {
  const all = [];
  for (const holder of Object.values(els))
    for (const m of holder.innerHTML.matchAll(/<input\b([^>]*)>/g)) all.push(attrs(m[1]));
  return all;
}
// The input with this id in the current DOM, created the way a browser would.
function input(id) {
  if (made[id]) return made[id];
  const a = rendered().find((x) => x.id === id);
  if (!a) throw new Error("no input " + id);
  const el = { id, tagName: "INPUT", type: a.type, value: a.value, dataset: {},
    className: a.class || "", closest: () => true };
  for (const k in a) if (k.startsWith("data-")) el.dataset[k.slice(5)] = a[k];
  el.onkeydown = handler(a.onkeydown);
  el.onblur = handler(a.onblur);
  return (made[id] = el);
}
function focus(el) {  // Tab or a click moves the focus; the field left behind gets blur
  const prev = context.document.activeElement;
  context.document.activeElement = el;
  if (prev && prev !== el && prev.onblur) prev.onblur.call(prev, {});
}
function key(el, k) { el.onkeydown.call(el, { key: k, preventDefault() {} }); }
function type(el, text) { el.value = text; }

function sample() {
  return {
    preset: 2, gainmin: -80, gainmax: 12, delaymax: 1920, slopemin: 0.1, slopemax: 1.5,
    types: ["peq"], choices: [{ t: "peq", c: 1 }],
    peq: { fmin: 20, fmax: 20000, qmin: 0.1, qmax: 20, gmin: -24, gmax: 12 },
    sources: [{ v: 0, n: "In 1" }], routes: ["Bus 1"], spdifopts: [], spdifout: 0,
    links: { bus: { id: "bus" }, outs: [{ id: "A" }] },
    buses: [{ name: "Bus 1", source: 0, link: "", mute: false, gain: 0, gainBypass: false,
      used: 0, budget: 10, free: 10, slots: [] }],
    outs: [{ name: "Out 1", route: 0, link: "", mute: false, gain: -6, gainBypass: false,
      polarity: false, delayUs: 0, delaySamples: 0, delayMm: 0, delayBypass: false,
      used: 1, budget: 10, free: 9,
      slots: [{ type: "peq", freq: 1000, q: 0.7, gain: 0, bypass: false, avail: 10 }] }],
  };
}
// The firmware accepts a patch and applies it to the state.
function accept(body) {
  const s = JSON.parse(JSON.stringify(context.st));
  const c = (body.ch == "bus" ? s.buses : s.outs)[body.idx];
  if (body.slot) {
    for (const k of ["freq", "q", "gain", "slope"]) if (k in body.slot) c.slots[body.slot.i][k] = body.slot[k];
  } else {
    if ("gain" in body) c.gain = body.gain;
    if ("delayUs" in body)  // quantized to whole samples at 48 kHz
      Object.assign(c, { delayUs: 104.16666, delaySamples: 5, delayMm: 35.7 });
  }
  return { ok: true, status: 200, text: JSON.stringify(s) };
}

async function commitTests() {
  let posts = [], reply = accept;
  context.draw = realDraw;
  context.plot = () => {};
  context.coeffs = () => {};
  context.activePreset = 2;
  context.document.getElementById = element;
  context.document.activeElement = null;
  context.document.querySelectorAll = (sel) => {
    assert.strictEqual(sel, "input.num");
    return rendered().filter((a) => /\bnum\b/.test(a.class || "")).map((a) => input(a.id));
  };
  context.fetch = (url, options) => {
    const body = JSON.parse(options.body);
    posts.push({ url, body });
    const r = url == "/matrix" ? reply(body) : { ok: true, status: 200, text: "" };
    return Promise.resolve({ ok: r.ok, status: r.status, text: () => Promise.resolve(r.text) });
  };

  context.st = sample();
  context.view = null;
  context.draw();
  for (const holder of ["buses", "outs"])
    assert(!/type='range'/.test(els[holder].innerHTML), "Channel gain on the Matrix is a text field");

  // Matrix page: Channel gain is sent on Enter, as typed, and not while typing.
  const g = input("gout0");
  assert.strictEqual(g.value, "-6");
  focus(g);
  type(g, "-3");
  assert.strictEqual(posts.length, 0, "typing sends nothing");
  key(g, "Enter");
  assert.deepStrictEqual(posts.pop().body, { ch: "out", idx: 0, preset: 2, gain: -3 });
  await flush();
  assert.strictEqual(context.st.outs[0].gain, -3);
  focus(null);
  assert.strictEqual(posts.length, 0, "clicking outside after Enter does not send the value again");

  // Filter page: frequency, Q and gain are text fields.
  context.openF("out", 0);
  assert(!/type='range'/.test(els.slots.innerHTML + els.top.innerHTML), "the filter page has no sliders");
  const freq = input("s0freq"), q = input("s0q"), sg = input("s0gain");
  assert.strictEqual(freq.value, "1000");
  assert.strictEqual(q.value, "0.7");

  focus(freq);
  type(freq, "1200.0");
  key(freq, "Enter");
  assert.deepStrictEqual(posts.pop().body,
    { ch: "out", idx: 0, preset: 2, slot: { op: "set", i: 0, freq: 1200 } });
  await flush();
  assert.strictEqual(context.st.outs[0].slots[0].freq, 1200);
  assert.strictEqual(freq.value, "1200", "the focused field shows the applied value");

  // Tab on: the frequency is not sent again, an unchanged Q is not sent at all.
  focus(q);
  type(q, "0.70");
  focus(sg);
  assert.strictEqual(posts.length, 0, "an unchanged value on Commit sends no request");

  // Escape restores the old value; the following Commit then sends nothing.
  type(sg, "5");
  key(sg, "Escape");
  assert.strictEqual(sg.value, "0");
  focus(null);
  assert.strictEqual(posts.length, 0, "Escape discards the typed value");

  // Tab commits; a decimal comma is read as a decimal point.
  context.openF("out", 0);
  const q2 = input("s0q");
  focus(q2);
  type(q2, "1,5");
  focus(input("s0gain"));
  assert.deepStrictEqual(posts.pop().body,
    { ch: "out", idx: 0, preset: 2, slot: { op: "set", i: 0, q: 1.5 } });
  await flush();
  assert.strictEqual(q2.value, "1.5");

  // A rejected value: the field shows the previous value again and the reply's reason.
  reply = () => ({ ok: false, status: 400, text: "gain out of range" });
  const sg2 = input("s0gain");
  type(sg2, "99");
  focus(input("s0freq"));
  assert.deepStrictEqual(posts.pop().body,
    { ch: "out", idx: 0, preset: 2, slot: { op: "set", i: 0, gain: 99 } });
  await flush();
  assert.strictEqual(sg2.value, "0", "a rejected value reverts the field");
  assert.strictEqual(els.msg.textContent, "gain out of range");

  // Text that is not a number goes to the firmware, which names the problem.
  reply = () => ({ ok: false, status: 400, text: "freq must be a number" });
  const f2 = input("s0freq");
  type(f2, "abc");
  key(f2, "Enter");
  assert.strictEqual(posts.pop().body.slot.freq, null);
  await flush();
  assert.strictEqual(f2.value, "1200");
  assert.strictEqual(els.msg.textContent, "freq must be a number");

  // Channel gain and delay on the filter page.
  reply = accept;
  focus(null);
  context.openF("out", 0);
  const tg = input("tgout0"), td = input("tdout0");
  assert.strictEqual(tg.value, "-3");
  focus(tg);
  type(tg, "-4.5");
  focus(td);
  assert.deepStrictEqual(posts.pop().body, { ch: "out", idx: 0, preset: 2, gain: -4.5 });
  await flush();
  type(td, "100");
  key(td, "Enter");
  assert.deepStrictEqual(posts.pop().body, { ch: "out", idx: 0, preset: 2, delayUs: 100 });
  await flush();
  assert.strictEqual(td.value, "104.2", "the delay field shows the applied, sample-quantized delay");
  assert(/104\.2 &micro;s = 5 samples/.test(els.dapp.innerHTML), "the applied label follows the Commit");
  assert.strictEqual(posts.length, 0);

  // Master volume stays a live slider.
  context.sendVol("-20");
  assert.strictEqual(posts.pop().url, "/mvol");
}

main().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
