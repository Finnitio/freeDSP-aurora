// The WiFi dialog on the Matrix page: same checks, messages and warning as the settings UI.
// Run: node SOURCES/WEBAPP/ESP32/test/test_matrixwifi.js
const assert = require("assert");
const fs = require("fs");
const vm = require("vm");

const header = fs.readFileSync("SOURCES/WEBAPP/ESP32/aurora/matrixpage.h", "utf8");
const page = header.match(/<script>\n([\s\S]*?)<\/script>/)[1];
const settings = fs.readFileSync("SOURCES/WEBAPP/js/aurora.js", "utf8");

// Runs a page script with persistent fake elements; fetch answers from `replies` by URL.
function run(script, replies) {
  const elements = {};
  const requests = [];
  const alerts = [];
  const context = {
    console,
    alert(t) { alerts.push(t); },
    document: {
      addEventListener() {},
      getElementById(id) {
        if (!elements[id]) elements[id] = { value: "", textContent: "", innerHTML: "", style: {} };
        return elements[id];
      },
    },
    fetch(url, options) {
      requests.push({ url, options });
      const r = replies[url] || { status: 200, body: "{}" };
      return Promise.resolve({
        ok: r.status == 200,
        status: r.status,
        json: () => Promise.resolve(JSON.parse(r.body)),
        text: () => Promise.resolve(r.body),
      });
    },
    setTimeout,
    clearTimeout,
  };
  vm.createContext(context);
  vm.runInContext(script, context);
  return { context, elements, requests, alerts };
}
const settle = () => new Promise((r) => setImmediate(r));

const fallback = {
  ssid: "home", apname: "aurora", apchannel: 11,
  apopenfallback: true, appwderror: "AP password needs at least 8 characters",
};

async function opensWithCurrentApConfig() {
  const p = run(page, { "/wificonfig": { status: 200, body: JSON.stringify({ ssid: "home", apname: "aurora", apchannel: 11, apopenfallback: false }) } });
  p.elements.pwdap = { value: "typed-before", textContent: "", style: {} };
  p.context.openWifi();
  await settle();
  assert.strictEqual(p.elements.wifi.style.display, "block", "the dialog is shown");
  assert.strictEqual(p.elements.apname.value, "aurora");
  assert.strictEqual(String(p.elements.apchannel.value), "11");
  assert.strictEqual(p.elements.pwdap.value, "", "the password is never shown, a previous entry is cleared");
  assert.strictEqual(p.elements.apwarn.style.display, "none");
  assert.strictEqual(p.elements.apwarn.textContent, "");
}

async function showsOpenApWarningLikeSettings() {
  const p = run(page, { "/wificonfig": { status: 200, body: JSON.stringify(fallback) } });
  p.context.openWifi();
  await settle();
  const s = run(settings, { "/wificonfig": { status: 200, body: JSON.stringify(fallback) } });
  s.context.getWifiConfig();
  await settle();
  assert.strictEqual(p.elements.apwarn.style.display, "block");
  assert(/running OPEN/.test(p.elements.apwarn.textContent));
  assert.strictEqual(p.elements.apwarn.textContent, s.elements.apwarn.textContent,
    "same warning text as the settings UI");
}

// Each invalid entry: the settings UI's alert text is the expected message on the matrix page.
async function rejectsInvalidInputWithSettingsMessages() {
  const cases = [
    { pwdap: "short", apname: "aurora", apchannel: "6", msg: "Please select a password with at least 8 characters." },
    { pwdap: "", apname: "", apchannel: "6", msg: "The name must have 1 to 32 characters." },
    { pwdap: "", apname: "x".repeat(33), apchannel: "6", msg: "The name must have 1 to 32 characters." },
    { pwdap: "", apname: "aurora", apchannel: "0", msg: "The channel must be between 1 and 13." },
    { pwdap: "", apname: "aurora", apchannel: "14", msg: "The channel must be between 1 and 13." },
    { pwdap: "", apname: "aurora", apchannel: "", msg: "The channel must be between 1 and 13." },
  ];
  for (const c of cases) {
    const s = run(settings, {});
    Object.assign(s.context.document.getElementById("pwdap"), { value: c.pwdap });
    Object.assign(s.context.document.getElementById("apname"), { value: c.apname });
    Object.assign(s.context.document.getElementById("apchannel"), { value: c.apchannel });
    s.context.sendPwdAP();
    assert.deepStrictEqual(s.alerts, [c.msg], "settings UI reference for " + JSON.stringify(c));

    const p = run(page, {});
    Object.assign(p.context.document.getElementById("pwdap"), { value: c.pwdap });
    Object.assign(p.context.document.getElementById("apname"), { value: c.apname });
    Object.assign(p.context.document.getElementById("apchannel"), { value: c.apchannel });
    await p.context.storeAp();
    assert.strictEqual(p.elements.wmsg.textContent, c.msg, JSON.stringify(c));
    assert.strictEqual(p.requests.length, 0, "nothing is sent for " + JSON.stringify(c));
  }
}

async function storesValidConfigWithRestartNote() {
  const p = run(page, {});
  Object.assign(p.context.document.getElementById("pwdap"), { value: "longenough" });
  Object.assign(p.context.document.getElementById("apname"), { value: "aurora" });
  Object.assign(p.context.document.getElementById("apchannel"), { value: "13" });
  await p.context.storeAp();
  assert.strictEqual(p.requests.length, 1);
  assert.strictEqual(p.requests[0].url, "/pwdap");
  assert.strictEqual(p.requests[0].options.method, "POST");
  assert.deepStrictEqual(JSON.parse(p.requests[0].options.body), { pwdap: "longenough", apname: "aurora", apchannel: 13 });
  assert.strictEqual(p.elements.wmsg.textContent,
    "Success! The configuration of the access point has been stored. It is applied after a restart.");
  assert.strictEqual(p.elements.pwdap.value, "", "the password is cleared after storing");

  // empty password = open AP, as in the settings UI
  const q = run(page, {});
  Object.assign(q.context.document.getElementById("apname"), { value: "aurora" });
  Object.assign(q.context.document.getElementById("apchannel"), { value: "1" });
  await q.context.storeAp();
  assert.deepStrictEqual(JSON.parse(q.requests[0].options.body), { pwdap: "", apname: "aurora", apchannel: 1 });
}

async function showsFirmwareRefusal() {
  const p = run(page, { "/pwdap": { status: 400, body: "AP password can have at most 63 characters" } });
  Object.assign(p.context.document.getElementById("pwdap"), { value: "x".repeat(64) });
  Object.assign(p.context.document.getElementById("apname"), { value: "aurora" });
  Object.assign(p.context.document.getElementById("apchannel"), { value: "6" });
  await p.context.storeAp();
  assert.strictEqual(p.elements.wmsg.textContent, "Not stored: AP password can have at most 63 characters");
}

async function closes() {
  const p = run(page, { "/wificonfig": { status: 200, body: JSON.stringify(fallback) } });
  p.context.openWifi();
  await settle();
  p.elements.wmsg.textContent = "old";
  p.context.closeWifi();
  assert.strictEqual(p.elements.wifi.style.display, "none");
  p.context.openWifi();
  assert.strictEqual(p.elements.wmsg.textContent, "", "a reopened dialog starts without an old message");
}

async function main() {
  await opensWithCurrentApConfig();
  await showsOpenApWarningLikeSettings();
  await rejectsInvalidInputWithSettingsMessages();
  await storesValidConfigWithRestartNote();
  await showsFirmwareRefusal();
  await closes();
  console.log("all tests passed");
}

main().catch((e) => { console.error(e); process.exit(1); });
