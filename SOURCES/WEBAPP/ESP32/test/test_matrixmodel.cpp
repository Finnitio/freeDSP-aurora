// Host tests for the matrix model (Bus Sources, Output routes, mute, gain, names, presets).
// Build & run (ArduinoJson is header-only):
//   g++ -std=c++11 -Wall -I ~/Documents/Arduino/libraries/ArduinoJson/src -o test_matrixmodel SOURCES/WEBAPP/ESP32/test/test_matrixmodel.cpp SOURCES/WEBAPP/ESP32/aurora/matrixmodel.cpp && ./test_matrixmodel
#include <cassert>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
#include "../aurora/matrixmodel.h"

struct Write
{
  char type;  // 'g' gain, 'r' route, 's' source
  MatrixChannelKind kind;
  int idx;
  double val;
};

struct FakePort : MatrixPort
{
  std::vector<Write> w;
  void writeGain(MatrixChannelKind kind, int idx, double linear) override { w.push_back({ 'g', kind, idx, linear }); }
  void writeRoute(int out, int route) override { w.push_back({ 'r', MATRIX_OUT, out, (double)route }); }
  void writeSource(int bus, uint32_t sel) override { w.push_back({ 's', MATRIX_BUS, bus, (double)sel }); }
};

static std::string patch(MatrixModel& m, const char* json, uint32_t now = 0)
{
  DynamicJsonDocument doc(512);
  assert(!deserializeJson(doc, json));
  return m.apply(doc.as<JsonObject>(), now);
}

static DynamicJsonDocument stateOf(const MatrixModel& m)
{
  DynamicJsonDocument doc(8192);
  std::string s = m.stateJson();
  assert(!deserializeJson(doc, s));
  return doc;
}

static bool near(double a, double b) { return std::fabs(a - b) < 1e-6; }

static void freshDefaults()
{
  FakePort port;
  MatrixModel m(port, 2, 8);
  DynamicJsonDocument st = stateOf(m);
  assert(st["buses"][0]["source"] == 0);  // Analog 1
  assert(st["buses"][1]["source"] == 1);  // Analog 2
  for (int o = 0; o < 8; o++)
  {
    assert(st["outs"][o]["route"] == o % 2);  // alternate Bus 1 / Bus 2
    assert(st["outs"][o]["mute"] == true);
  }
  assert(st["buses"][0]["mute"] == false);
  assert(st["outs"][0]["gain"] == 0);
  assert(std::string(st["outs"][3]["name"].as<const char*>()) == "Output 4");
}

static void applyAllEmitsFullWriteSet()
{
  FakePort port;
  MatrixModel m(port, 2, 8);
  m.applyAll();
  // 2 x (source + gain) + 8 x (route + gain)
  assert(port.w.size() == 4 + 16);
  int muted = 0;
  for (size_t i = 0; i < port.w.size(); i++)
    if (port.w[i].type == 'g' && port.w[i].kind == MATRIX_OUT && port.w[i].val == 0.0)
      muted++;
  assert(muted == 8);
}

static void muteAndGainWrites()
{
  FakePort port;
  MatrixModel m(port, 2, 8);
  assert(patch(m, "{\"ch\":\"out\",\"idx\":2,\"mute\":false}").empty());
  assert(port.w.size() == 1 && port.w[0].type == 'g' && port.w[0].kind == MATRIX_OUT && port.w[0].idx == 2);
  assert(near(port.w[0].val, 1.0));

  port.w.clear();
  assert(patch(m, "{\"ch\":\"out\",\"idx\":2,\"gain\":6.0206}").empty());
  assert(port.w.size() == 1 && near(port.w[0].val, 2.0));

  // muted: gain edits are stored but nothing audible changes, so no write
  port.w.clear();
  assert(patch(m, "{\"ch\":\"out\",\"idx\":2,\"mute\":true}").empty());
  assert(port.w.size() == 1 && port.w[0].val == 0.0);
  port.w.clear();
  assert(patch(m, "{\"ch\":\"out\",\"idx\":2,\"gain\":-80}").empty());
  assert(port.w.empty());
  assert(patch(m, "{\"ch\":\"out\",\"idx\":2,\"mute\":false}").empty());
  assert(near(port.w[0].val, 1e-4));

  DynamicJsonDocument st = stateOf(m);
  assert(st["outs"][2]["mute"] == false);
  assert(st["outs"][2]["gain"] == -80);
}

static void busMuteWritesBusGain()
{
  // The Bus gain sits before the route selector, so muting it silences the Bus in
  // every Output it feeds; an Output on Bus 1+2 keeps the other Bus.
  FakePort port;
  MatrixModel m(port, 2, 8);
  assert(patch(m, "{\"ch\":\"bus\",\"idx\":0,\"mute\":true}").empty());
  assert(port.w.size() == 1 && port.w[0].kind == MATRIX_BUS && port.w[0].idx == 0 && port.w[0].val == 0.0);
}

static void routeChangeRampsDownSwitchesRampsUp()
{
  FakePort port;
  MatrixModel m(port, 2, 8);
  patch(m, "{\"ch\":\"out\",\"idx\":1,\"mute\":false,\"gain\":-6}");
  port.w.clear();
  assert(patch(m, "{\"ch\":\"out\",\"idx\":1,\"route\":2}").empty());
  assert(port.w.size() == 3);
  assert(port.w[0].type == 'g' && port.w[0].idx == 1 && port.w[0].val == 0.0);
  assert(port.w[1].type == 'r' && port.w[1].idx == 1 && port.w[1].val == 2);
  assert(port.w[2].type == 'g' && port.w[2].idx == 1 && near(port.w[2].val, std::pow(10.0, -6.0 / 20)));

  // muted Output: just switch
  port.w.clear();
  assert(patch(m, "{\"ch\":\"out\",\"idx\":0,\"route\":1}").empty());
  assert(port.w.size() == 1 && port.w[0].type == 'r');

  // same value again: nothing to do
  port.w.clear();
  assert(patch(m, "{\"ch\":\"out\",\"idx\":0,\"route\":1}").empty());
  assert(port.w.empty());
}

static void sourceChangeRampsTheBus()
{
  FakePort port;
  MatrixModel m(port, 2, 8);
  assert(patch(m, "{\"ch\":\"bus\",\"idx\":1,\"source\":65538}").empty());  // USB 3
  assert(port.w.size() == 3);
  assert(port.w[0].type == 'g' && port.w[0].kind == MATRIX_BUS && port.w[0].val == 0.0);
  assert(port.w[1].type == 's' && port.w[1].idx == 1 && port.w[1].val == 65538);
  assert(port.w[2].type == 'g' && port.w[2].kind == MATRIX_BUS && near(port.w[2].val, 1.0));
}

static void invalidPatchesAreRejectedWithoutSideEffects()
{
  FakePort port;
  MatrixModel m(port, 2, 8);
  const char* bad[] = {
    "{\"ch\":\"out\",\"idx\":8,\"mute\":false}",           // no such Output
    "{\"ch\":\"bus\",\"idx\":2,\"mute\":false}",           // no such Bus
    "{\"ch\":\"amp\",\"idx\":0}",                          // unknown channel kind
    "{\"ch\":\"out\",\"idx\":0,\"gain\":12.5}",            // above range
    "{\"ch\":\"out\",\"idx\":0,\"gain\":-80.5}",           // below range
    "{\"ch\":\"out\",\"idx\":0,\"gain\":\"loud\"}",        // wrong type
    "{\"ch\":\"out\",\"idx\":0,\"route\":3}",              // no such route
    "{\"ch\":\"out\",\"idx\":0,\"source\":0}",             // Outputs have no Source
    "{\"ch\":\"bus\",\"idx\":0,\"route\":0}",              // Buses have no route
    "{\"ch\":\"bus\",\"idx\":0,\"source\":99}",            // no such Source
    "{\"ch\":\"out\",\"idx\":0,\"name\":\"12345678901234567\"}",  // 17 chars
    "{\"ch\":\"out\",\"idx\":0,\"colour\":1}",             // unknown field
    "{\"ch\":\"out\",\"idx\":0,\"mute\":false,\"gain\":99}",  // one bad field spoils the patch
  };
  for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
    assert(!patch(m, bad[i]).empty());
  assert(port.w.empty());
  DynamicJsonDocument st = stateOf(m);
  assert(st["outs"][0]["mute"] == true);
}

static void namesAreStoredWithoutDspWrites()
{
  FakePort port;
  MatrixModel m(port, 2, 8);
  assert(patch(m, "{\"ch\":\"out\",\"idx\":4,\"name\":\"Tweeter L\"}").empty());
  assert(port.w.empty());
  DynamicJsonDocument st = stateOf(m);
  assert(std::string(st["outs"][4]["name"].as<const char*>()) == "Tweeter L");
}

static void presetRoundTripAndFullWriteSetOnLoad()
{
  FakePort port;
  MatrixModel a(port, 2, 8);
  patch(a, "{\"ch\":\"bus\",\"idx\":0,\"source\":131075}");  // Exp 4
  patch(a, "{\"ch\":\"out\",\"idx\":5,\"name\":\"Sub \\\"mono\\\"\",\"route\":2,\"mute\":false,\"gain\":-3.5}");
  patch(a, "{\"ch\":\"bus\",\"idx\":1,\"mute\":true}");
  std::string saved = a.savePreset();

  FakePort port2;
  MatrixModel b(port2, 2, 8);
  assert(b.loadPreset(saved).empty());
  assert(b.savePreset() == saved);  // unchanged
  assert(port2.w.size() == 4 + 16);  // full write set
  bool sawExp = false, sawRoute2 = false;
  for (size_t i = 0; i < port2.w.size(); i++)
  {
    if (port2.w[i].type == 's' && port2.w[i].val == 131075) sawExp = true;
    if (port2.w[i].type == 'r' && port2.w[i].idx == 5 && port2.w[i].val == 2) sawRoute2 = true;
  }
  assert(sawExp && sawRoute2);
  DynamicJsonDocument st = stateOf(b);
  assert(std::string(st["outs"][5]["name"].as<const char*>()) == "Sub \"mono\"");
}

static void badPresetIsRejectedAndKeepsState()
{
  FakePort port;
  MatrixModel m(port, 2, 8);
  std::string good = m.savePreset();
  assert(!m.loadPreset("not json").empty());
  assert(!m.loadPreset("{\"v\":2,\"buses\":[],\"outs\":[]}").empty());  // unknown version
  assert(!m.loadPreset("{\"v\":1,\"buses\":[],\"outs\":[]}").empty());  // wrong counts
  assert(m.savePreset() == good);
  assert(port.w.empty());
}

static void loadDefaultsResetsAndWritesEverything()
{
  FakePort port;
  MatrixModel m(port, 2, 8);
  std::string fresh = m.savePreset();
  patch(m, "{\"ch\":\"out\",\"idx\":0,\"mute\":false,\"name\":\"X\"}", 10);
  port.w.clear();
  m.loadDefaults();
  assert(m.savePreset() == fresh);
  assert(port.w.size() == 4 + 16);
  assert(!m.dirty());
}

static void saveIsDueTwoSecondsAfterTheLastChange()
{
  FakePort port;
  MatrixModel m(port, 2, 8);
  assert(!m.saveDue(100000));  // nothing changed yet
  patch(m, "{\"ch\":\"out\",\"idx\":0,\"mute\":false}", 1000);
  assert(!m.saveDue(2999));
  patch(m, "{\"ch\":\"out\",\"idx\":0,\"gain\":-1}", 2500);  // restarts the wait
  assert(!m.saveDue(4499));
  assert(m.saveDue(4500));
  m.markSaved();
  assert(!m.saveDue(99999));
  // a rejected patch is not a change
  patch(m, "{\"ch\":\"out\",\"idx\":0,\"gain\":99}", 5000);
  assert(!m.saveDue(99999));
}

static void undoRestoresTheLastAcceptedPatch()
{
  FakePort port;
  MatrixModel m(port, 2, 8);
  patch(m, "{\"ch\":\"out\",\"idx\":1,\"name\":\"Keep\"}", 1000);
  m.markSaved();
  const std::string before = m.savePreset();
  assert(patch(m, "{\"ch\":\"out\",\"idx\":1,\"mute\":false,\"route\":0,\"name\":\"X\"}", 5000).empty());
  assert(m.dirty());
  m.undo();
  assert(m.savePreset() == before);
  assert(!m.dirty());  // dirty flag restored too
  m.undo();            // second undo is a no-op
  assert(m.savePreset() == before);
}

int main()
{
  undoRestoresTheLastAcceptedPatch();
  freshDefaults();
  applyAllEmitsFullWriteSet();
  muteAndGainWrites();
  busMuteWritesBusGain();
  routeChangeRampsDownSwitchesRampsUp();
  sourceChangeRampsTheBus();
  invalidPatchesAreRejectedWithoutSideEffects();
  namesAreStoredWithoutDspWrites();
  presetRoundTripAndFullWriteSetOnLoad();
  badPresetIsRejectedAndKeepsState();
  loadDefaultsResetsAndWritesEverything();
  saveIsDueTwoSecondsAfterTheLastChange();
  printf("all tests passed\n");
  return 0;
}
