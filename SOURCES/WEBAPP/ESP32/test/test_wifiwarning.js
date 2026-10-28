// The WiFi settings dialog warns while the AP runs open by fallback.
// Run: node SOURCES/WEBAPP/ESP32/test/test_wifiwarning.js
const assert = require("assert");
const fs = require("fs");
const vm = require("vm");

const script = fs.readFileSync("SOURCES/WEBAPP/js/aurora.js", "utf8");

function openDialog(reply) {
  const elements = {};
  const context = {
    console,
    document: {
      getElementById(id) {
        if (!elements[id]) elements[id] = { value: "", textContent: "", style: {} };
        return elements[id];
      },
    },
    fetch() {
      return Promise.resolve({ json: () => Promise.resolve(reply) });
    },
  };
  vm.createContext(context);
  vm.runInContext(script, context);
  context.getWifiConfig();
  return new Promise((resolve) => setImmediate(() => resolve(elements)));
}

async function main() {
  let el = await openDialog({
    ssid: "", apname: "aurora", apchannel: 6,
    apopenfallback: true, appwderror: "AP password needs at least 8 characters",
  });
  assert.strictEqual(el.apwarn.style.display, "block");
  assert(/running OPEN/.test(el.apwarn.textContent));
  assert(/at least 8 characters/.test(el.apwarn.textContent));
  assert.strictEqual(el.pwdap, undefined);  // the password is never filled in

  el = await openDialog({ ssid: "", apname: "aurora", apchannel: 6, apopenfallback: false });
  assert.strictEqual(el.apwarn.style.display, "none");
  assert.strictEqual(el.apwarn.textContent, "");

  // Older firmware without the flag: no warning
  el = await openDialog({ ssid: "", apname: "aurora", apchannel: 6 });
  assert.strictEqual(el.apwarn.style.display, "none");

  console.log("all tests passed");
}

main().catch((e) => { console.error(e); process.exit(1); });
