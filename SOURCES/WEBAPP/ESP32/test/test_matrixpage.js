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

async function main() {
  context.activePreset = 2;
  context.draw = () => {};

  await context.sendLink("out", "B", "mute", true);

  assert.strictEqual(request.url, "/matrix");
  assert.deepStrictEqual(JSON.parse(request.options.body), {
    ch: "link",
    scope: "out",
    group: "B",
    preset: 2,
    mute: true,
  });
  console.log("matrix page tests passed");
}

main().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
