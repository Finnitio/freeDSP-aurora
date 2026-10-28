// The WiFi dialog on the Matrix page (joining a WiFi and the AP): same checks, messages and warning as the settings UI.
// Run: SOURCES/WEBAPP/ESP32/test/run.sh (runs all host tests)
const assert = require("assert");
const fs = require("fs");
const vm = require("vm");

const header = fs.readFileSync("SOURCES/WEBAPP/ESP32/aurora/matrixpage.h", "utf8");
const page = header.match(/<script>\n([\s\S]*?)<\/script>/)[1];
const settings = fs.readFileSync("SOURCES/WEBAPP/js/aurora.js", "utf8");

// Runs a page script with persistent fake elements; fetch answers from `replies` by URL.
// Timers only fire on tick(), which runs the ones pending at that moment.
function run(script, replies) {
  let timers = [];
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
    setTimeout(f) { const t = { f }; timers.push(t); return t; },
    clearTimeout(t) { timers = timers.filter((x) => x !== t); },
  };
  vm.createContext(context);
  vm.runInContext(script, context);
  const tick = () => { const due = timers; timers = []; due.forEach((t) => t.f()); return due.length; };
  return { context, elements, requests, alerts, replies, tick };
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

// While the AP is off ("AP off while connected") it is not running open: only a note that it comes back open.
async function openApWarningOnlyWhileTheApIsUp() {
  const down = Object.assign({}, fallback, { connected: true, ip: "10.0.0.7", apoff: true, apup: false });
  const p = run(page, { "/wificonfig": { status: 200, body: JSON.stringify(down) } });
  p.context.openWifi();
  await settle();
  const s = run(settings, { "/wificonfig": { status: 200, body: JSON.stringify(down) } });
  s.context.getWifiConfig();
  await settle();
  assert(!/running OPEN/.test(p.elements.apwarn.textContent), p.elements.apwarn.textContent);
  assert.strictEqual(p.elements.apwarn.textContent,
    "Note: the access point is off now. When it comes back it runs OPEN because the stored AP password is invalid"
    + " (AP password needs at least 8 characters). Store a valid AP password and restart.");
  assert.strictEqual(p.elements.apwarn.textContent, s.elements.apwarn.textContent, "same note as the settings UI");

  const up = Object.assign({}, down, { apup: true });
  const q = run(page, { "/wificonfig": { status: 200, body: JSON.stringify(up) } });
  q.context.openWifi();
  await settle();
  assert(/running OPEN/.test(q.elements.apwarn.textContent), "the AP is up: the warning is shown");
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

// Joining a WiFi: the settings UI's alert text is the expected message on the matrix page.
async function rejectsInvalidWifiWithSettingsMessages() {
  const cases = [
    { ssid: "x".repeat(33), pwd: "secret123", msg: "The network name can have at most 32 characters." },
    { ssid: "home", pwd: "1234567", msg: "The WiFi password must have 8 to 63 characters, or be empty for an open network." },
    { ssid: "home", pwd: "p".repeat(64), msg: "The WiFi password must have 8 to 63 characters, or be empty for an open network." },
  ];
  for (const c of cases) {
    const s = run(settings, {});
    Object.assign(s.context.document.getElementById("ssid"), { value: c.ssid });
    Object.assign(s.context.document.getElementById("pwd"), { value: c.pwd });
    s.context.sendWifi();
    assert.deepStrictEqual(s.alerts, [c.msg], "settings UI reference for " + JSON.stringify(c));
    assert.strictEqual(s.requests.length, 0, "settings UI sends nothing for " + JSON.stringify(c));

    const p = run(page, {});
    Object.assign(p.context.document.getElementById("ssid"), { value: c.ssid });
    Object.assign(p.context.document.getElementById("pwd"), { value: c.pwd });
    await p.context.joinWifi();
    assert.strictEqual(p.elements.smsg.textContent, c.msg, JSON.stringify(c));
    assert.strictEqual(p.requests.length, 0, "nothing is sent for " + JSON.stringify(c));
  }

  // The firmware refuses the same entries with the same texts (staConfigError in apconfig.h).
  const body = fs.readFileSync("SOURCES/WEBAPP/ESP32/aurora/apconfig.h", "utf8")
    .match(/staConfigError\([^)]*\)\s*\{([\s\S]*?)\n\}/)[1];
  const firmware = [...body.matchAll(/return "([^"]*)"/g)].map((m) => m[1]);
  assert.deepStrictEqual(firmware, [...new Set(cases.map((c) => c.msg))], "firmware messages = settings UI messages");
}

async function joinsWifiAndKeepsTheAp() {
  const p = run(page, {});
  Object.assign(p.context.document.getElementById("ssid"), { value: "home" });
  Object.assign(p.context.document.getElementById("pwd"), { value: "secret123" });
  await p.context.joinWifi();
  const posts = p.requests.filter((r) => r.url == "/wifi");
  assert.strictEqual(posts.length, 1);
  assert.strictEqual(posts[0].options.method, "POST");
  assert.deepStrictEqual(JSON.parse(posts[0].options.body), { ssid: "home", pwd: "secret123" });
  assert.strictEqual(p.elements.smsg.textContent,
    "Stored. The DSP now joins \"home\". Once connected, it is reachable in that network on the IP shown above; the access point stays available.");
  assert.strictEqual(p.elements.pwd.value, "", "the password is cleared after storing");

  const q = run(page, { "/wifi": { status: 400, body: "The network name can have at most 32 characters." } });
  Object.assign(q.context.document.getElementById("ssid"), { value: "home" });
  await q.context.joinWifi();
  assert.deepStrictEqual(JSON.parse(q.requests[0].options.body), { ssid: "home", pwd: "" }, "open network");
  assert.strictEqual(q.elements.smsg.textContent, "Not stored: The network name can have at most 32 characters.");
}

// The page has its own "Forget network" button, so "Join network" refuses an empty name
// (the settings UI has one button only and forgets the network on an empty name).
async function joinRefusesEmptyName() {
  const p = run(page, {});
  Object.assign(p.context.document.getElementById("pwd"), { value: "secret123" });
  await p.context.joinWifi();
  assert.strictEqual(p.elements.smsg.textContent,
    "Please enter the name of the network to join. To stop using a network, use \"Forget network\".");
  assert.strictEqual(p.requests.length, 0, "nothing is sent");
}

async function forgetsWifiAndKeepsTheAp() {
  const p = run(page, {});
  Object.assign(p.context.document.getElementById("ssid"), { value: "home" });
  Object.assign(p.context.document.getElementById("pwd"), { value: "typed" });
  await p.context.forgetWifi();
  const posts = p.requests.filter((r) => r.url == "/wifi");
  assert.strictEqual(posts.length, 1);
  assert.deepStrictEqual(JSON.parse(posts[0].options.body), { ssid: "", pwd: "" }, "empty SSID ends the connection");
  assert.strictEqual(p.elements.ssid.value, "");
  assert.strictEqual(p.elements.pwd.value, "");
  assert.strictEqual(p.elements.smsg.textContent,
    "The network has been forgotten. The DSP is now reachable on its access point only.");
}

const wificonfig = (d) => ({ status: 200, body: JSON.stringify(Object.assign({ apname: "aurora", apchannel: 6, apopenfallback: false }, d)) });

async function showsStoredNetworkAndConnection() {
  const p = run(page, { "/wificonfig": wificonfig({ ssid: "home", connected: true, ip: "192.168.178.40" }) });
  p.elements.pwd = { value: "typed-before", textContent: "", style: {} };
  p.context.openWifi();
  await settle();
  assert.strictEqual(p.elements.ssid.value, "home", "the stored SSID is shown");
  assert.strictEqual(p.elements.pwd.value, "", "the stored password is never shown, a previous entry is cleared");
  assert.strictEqual(p.elements.wstat.textContent,
    "Connected to \"home\", IP 192.168.178.40. In that network the DSP is reachable at http://192.168.178.40/; the access point stays available.");

  const q = run(page, { "/wificonfig": wificonfig({ ssid: "home", connected: false }) });
  q.context.openWifi();
  await settle();
  assert.strictEqual(q.elements.wstat.textContent, "Not connected to \"home\". The DSP keeps trying; the access point stays available.");

  const r = run(page, { "/wificonfig": wificonfig({ ssid: "", connected: false }) });
  r.context.openWifi();
  await settle();
  assert.strictEqual(r.elements.ssid.value, "");
  assert.strictEqual(r.elements.wstat.textContent, "No WiFi network stored. The DSP is reachable on its access point.");
}

// While the dialog is open the connection state is refreshed; typed entries stay.
async function refreshesConnectionWhileOpen() {
  const p = run(page, { "/wificonfig": wificonfig({ ssid: "", connected: false }) });
  p.context.openWifi();
  await settle();
  p.elements.ssid.value = "home";
  p.replies["/wificonfig"] = wificonfig({ ssid: "home", connected: true, ip: "10.0.0.7" });
  assert(p.tick() > 0, "a refresh is pending while the dialog is open");
  await settle();
  assert(/IP 10\.0\.0\.7/.test(p.elements.wstat.textContent), p.elements.wstat.textContent);
  assert.strictEqual(p.elements.ssid.value, "home", "the typed SSID is not overwritten");
  assert.strictEqual(p.elements.apname.value, "aurora");

  p.context.closeWifi();
  assert.strictEqual(p.tick(), 0, "no refresh once the dialog is closed");
}

// "AP off while connected": the same texts in the matrix page and the settings UI. The grace
// time in them is the firmware's, so changing ApSwitch::GRACE_MS without the texts fails here.
const GRACE_MS = Number(fs.readFileSync("SOURCES/WEBAPP/ESP32/aurora/apswitch.h", "utf8")
  .match(/GRACE_MS\s*=\s*(\d+)/)[1]);
const GRACE = GRACE_MS / 1000 + " s";
const APOFF_ON = "Stored. The access point switches off " + GRACE + " after the DSP has joined its WiFi; the DSP is then only reachable on its WiFi IP. The access point comes back when the connection is lost.";
const APOFF_OFF = "Stored. The access point stays available.";

async function showsStoredApOffSetting() {
  const p = run(page, { "/wificonfig": wificonfig({ ssid: "home", connected: false, apoff: true, apup: true }) });
  p.context.openWifi();
  await settle();
  assert.strictEqual(p.elements.apoff.checked, true);

  const q = run(page, { "/wificonfig": wificonfig({ ssid: "home", connected: false }) });
  q.context.openWifi();
  await settle();
  assert.strictEqual(q.elements.apoff.checked, false, "older firmware without the setting: off");

  const s = run(settings, { "/wificonfig": wificonfig({ ssid: "home", apoff: true }) });
  s.context.getWifiConfig();
  await settle();
  assert.strictEqual(s.elements.apoff.checked, true, "the settings UI shows it too");
}

async function storesApOffWithReachabilityNote() {
  for (const [checked, msg] of [[true, APOFF_ON], [false, APOFF_OFF]]) {
    const s = run(settings, {});
    s.context.document.getElementById("apoff").checked = checked;
    s.context.sendApOff();
    await settle();
    assert.strictEqual(s.requests.length, 1);
    assert.strictEqual(s.requests[0].url, "/apoff");
    assert.strictEqual(s.requests[0].options.method, "POST");
    assert.deepStrictEqual(JSON.parse(s.requests[0].options.body), { apoff: checked });
    assert.deepStrictEqual(s.alerts, [msg], "settings UI reference");

    const p = run(page, {});
    p.context.document.getElementById("apoff").checked = checked;
    await p.context.storeApOff();
    assert.strictEqual(p.requests.length, 1);
    assert.strictEqual(p.requests[0].url, "/apoff");
    assert.deepStrictEqual(JSON.parse(p.requests[0].options.body), { apoff: checked });
    assert.strictEqual(p.elements.smsg.textContent, msg);
  }

  const p = run(page, { "/apoff": { status: 400, body: "apoff must be true or false" } });
  p.context.document.getElementById("apoff").checked = true;
  await p.context.storeApOff();
  assert.strictEqual(p.elements.smsg.textContent, "Not stored: apoff must be true or false");
  assert.strictEqual(p.elements.apoff.checked, false, "a refused change is undone");
}

async function connectionStateTellsWhetherTheApStays() {
  const p = run(page, { "/wificonfig": wificonfig({ ssid: "home", connected: true, ip: "10.0.0.7", apoff: true, apup: true }) });
  p.context.openWifi();
  await settle();
  assert.strictEqual(p.elements.wstat.textContent,
    "Connected to \"home\", IP 10.0.0.7. In that network the DSP is reachable at http://10.0.0.7/; the access point switches off shortly.");

  const q = run(page, { "/wificonfig": wificonfig({ ssid: "home", connected: true, ip: "10.0.0.7", apoff: true, apup: false }) });
  q.context.openWifi();
  await settle();
  assert.strictEqual(q.elements.wstat.textContent,
    "Connected to \"home\", IP 10.0.0.7. In that network the DSP is reachable at http://10.0.0.7/; the access point is off while connected.");

  const r = run(page, { "/wificonfig": wificonfig({ ssid: "home", connected: false, apoff: true, apup: true }) });
  r.context.openWifi();
  await settle();
  assert.strictEqual(r.elements.wstat.textContent, "Not connected to \"home\". The DSP keeps trying; the access point stays available.");
}

async function joinNoteMentionsTheApGoingOff() {
  const p = run(page, {});
  Object.assign(p.context.document.getElementById("ssid"), { value: "home" });
  Object.assign(p.context.document.getElementById("pwd"), { value: "secret123" });
  p.context.document.getElementById("apoff").checked = true;
  await p.context.joinWifi();
  assert.deepStrictEqual(JSON.parse(p.requests[0].options.body), { ssid: "home", pwd: "secret123" });
  assert.strictEqual(p.elements.smsg.textContent,
    "Stored. The DSP now joins \"home\". Once connected, it is reachable in that network on the IP shown above; the access point then switches off after " + GRACE + ".");
}

async function main() {
  await opensWithCurrentApConfig();
  await showsOpenApWarningLikeSettings();
  await openApWarningOnlyWhileTheApIsUp();
  await rejectsInvalidInputWithSettingsMessages();
  await storesValidConfigWithRestartNote();
  await showsFirmwareRefusal();
  await closes();
  await rejectsInvalidWifiWithSettingsMessages();
  await joinsWifiAndKeepsTheAp();
  await joinRefusesEmptyName();
  await forgetsWifiAndKeepsTheAp();
  await showsStoredNetworkAndConnection();
  await refreshesConnectionWhileOpen();
  await showsStoredApOffSetting();
  await storesApOffWithReachabilityNote();
  await connectionStateTellsWhetherTheApStays();
  await joinNoteMentionsTheApGoingOff();
  console.log("all tests passed");
}

main().catch((e) => { console.error(e); process.exit(1); });
