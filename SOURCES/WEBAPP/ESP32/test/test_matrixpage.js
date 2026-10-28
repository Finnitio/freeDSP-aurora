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
  console.log("matrix page tests passed");
}

main().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
