// Host tests for the matrix model (Bus Sources, Output routes, mute, gain, names, presets).
// Build & run (ArduinoJson is header-only):
//   g++ -std=gnu++11 -Wall -I ~/Documents/Arduino/libraries/ArduinoJson/src -o test_matrixmodel SOURCES/WEBAPP/ESP32/test/test_matrixmodel.cpp SOURCES/WEBAPP/ESP32/aurora/matrixmodel.cpp SOURCES/WEBAPP/ESP32/aurora/AudioFilterFactory.cpp && ./test_matrixmodel
#include <cassert>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
#include "../aurora/matrixmodel.h"

struct Write
{
  char type;  // 'g' gain, 'r' route, 's' source, 'p' polarity (0/1), 'd' delay samples
  MatrixChannelKind kind;
  int idx;
  double val;
};

struct FakePort : MatrixPort
{
  std::vector<Write> w;
  std::vector<Write> events;
  void record(char type, MatrixChannelKind kind, int idx, double val)
  {
    Write write = { type, kind, idx, val };
    w.push_back(write);
    events.push_back(write);
  }
  void writeGain(MatrixChannelKind kind, int idx, double linear) override { record('g', kind, idx, linear); }
  void writeRoute(int out, int route) override { w.push_back({ 'r', MATRIX_OUT, out, (double)route }); }
  void writeSource(int bus, uint32_t sel) override { w.push_back({ 's', MATRIX_BUS, bus, (double)sel }); }
  void writePolarity(int out, bool invert) override { w.push_back({ 'p', MATRIX_OUT, out, invert ? 1.0 : 0.0 }); }
  void writeDelay(int out, int samples) override { w.push_back({ 'd', MATRIX_OUT, out, (double)samples }); }
  void waitMs(int ms) override { record('w', MATRIX_OUT, 0, (double)ms); }
  void writeSpdifOut(uint32_t left, uint32_t right) override { w.push_back({ 'o', MATRIX_OUT, (int)right, (double)left }); }
  struct Bq
  {
    MatrixChannelKind kind;
    int idx, stage;
    float a[3], b[3];
  };
  std::vector<Bq> bq;
  void writeBiquad(MatrixChannelKind kind, int idx, int stage, const float a[3], const float b[3]) override
  {
    Bq q = { kind, idx, stage, { a[0], a[1], a[2] }, { b[0], b[1], b[2] } };
    bq.push_back(q);
    events.push_back({ 'b', kind, idx, (double)stage });
  }
};

static std::string patch(MatrixModel& m, const char* json, uint32_t now = 0)
{
  DynamicJsonDocument doc(512);
  assert(!deserializeJson(doc, json));
  return m.apply(doc.as<JsonObject>(), now);
}

static DynamicJsonDocument stateOf(const MatrixModel& m)
{
  DynamicJsonDocument doc(65536);
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
  // 2 x (source + gain) + 8 x (route + gain + polarity + delay) + SPDIF out
  assert(port.w.size() == 4 + 32 + 1);
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
  // down, wait for the slew to finish, switch, up: only this Output is touched
  assert(port.w.size() == 4);
  assert(port.w[0].type == 'g' && port.w[0].idx == 1 && port.w[0].val == 0.0);
  assert(port.w[1].type == 'w' && port.w[1].val >= 20);
  assert(port.w[2].type == 'r' && port.w[2].idx == 1 && port.w[2].val == 2);
  assert(port.w[3].type == 'g' && port.w[3].idx == 1 && near(port.w[3].val, std::pow(10.0, -6.0 / 20)));

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
  // the Bus ramps, not the Outputs it feeds
  assert(port.w.size() == 4);
  assert(port.w[0].type == 'g' && port.w[0].kind == MATRIX_BUS && port.w[0].idx == 1 && port.w[0].val == 0.0);
  assert(port.w[1].type == 'w' && port.w[1].val >= 20);
  assert(port.w[2].type == 's' && port.w[2].idx == 1 && port.w[2].val == 65538);
  assert(port.w[3].type == 'g' && port.w[3].kind == MATRIX_BUS && port.w[3].idx == 1 && near(port.w[3].val, 1.0));
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
  assert(port2.w.size() == 4 + 32 + 1);  // full write set
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
  assert(!m.loadPreset("{\"v\":4,\"buses\":[],\"outs\":[]}").empty());  // unknown version
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
  assert(port.w.size() == 4 + 32 + 1);
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

//==============================================================================
// Filter row: PEQ Slots on the Channel's biquads
static bool isPassthrough(const FakePort::Bq& q)
{
  return q.b[0] == 1.0f && q.b[1] == 0 && q.b[2] == 0 && q.a[1] == 0 && q.a[2] == 0;
}

static const char* kAddPeq = "{\"ch\":\"out\",\"idx\":1,\"slot\":{\"op\":\"add\",\"freq\":1000,\"q\":1,\"gain\":6}}";

// magnitude in dB of the quantized coefficients [b0,b1,b2,a1,a2] (a = negated denominator)
static double magDb(const DynamicJsonDocument& c, double f)
{
  const double w = 2 * 3.14159265358979 * f / c["fs"].as<double>();
  double db = 0;
  for (size_t i = 0; i < c["bq"].size(); i++)
  {
    JsonVariantConst q = c["bq"][i];
    double b0 = q[0], b1 = q[1], b2 = q[2], a1 = q[3], a2 = q[4];
    double nr = b0 + b1 * std::cos(w) + b2 * std::cos(2 * w), ni = -b1 * std::sin(w) - b2 * std::sin(2 * w);
    double dr = 1 - a1 * std::cos(w) - a2 * std::cos(2 * w), di = a1 * std::sin(w) + a2 * std::sin(2 * w);
    db += 10 * std::log10((nr * nr + ni * ni) / (dr * dr + di * di));
  }
  return db;
}

static DynamicJsonDocument coeffsOf(const MatrixModel& m, const char* ch, int idx)
{
  DynamicJsonDocument doc(8192);
  std::string s = m.coeffsJson(ch, idx);
  assert(!deserializeJson(doc, s));
  return doc;
}

static void addPeqWritesItsBiquad()
{
  FakePort port;
  MatrixModel m(port, 2, 8, 4);
  assert(patch(m, kAddPeq).empty());
  assert(port.bq.size() == 1);
  assert(port.bq[0].kind == MATRIX_OUT && port.bq[0].idx == 1 && port.bq[0].stage == 0);
  assert(!isPassthrough(port.bq[0]));
  DynamicJsonDocument st = stateOf(m);
  assert(st["outs"][1]["slots"].size() == 1);
  assert(st["outs"][1]["slots"][0]["freq"] == 1000);
  assert(st["outs"][1]["budget"] == 4 && st["outs"][1]["used"] == 1 && st["outs"][1]["free"] == 3);
  assert(st["outs"][0]["used"] == 0);
}

static void peqCoefficientsMatchTheDesign()
{
  FakePort port;
  MatrixModel m(port, 2, 8, 4);
  patch(m, kAddPeq);
  DynamicJsonDocument c = coeffsOf(m, "out", 1);
  assert(c["bq"].size() == 1);
  // reference: RBJ cookbook peaking EQ, f0 = 1 kHz, Q = 1, +6 dB, fs = 48 kHz
  // (a1/a2 negated for the DSP); tolerance covers the DSP's bandwidth warping
  const double ref[5] = { 1.0440, -1.8953, 0.8677, 1.8953, -0.9117 };
  for (int i = 0; i < 5; i++)
    assert(std::fabs(c["bq"][0][i].as<double>() - ref[i]) < 3e-3);
  // quantized to 8.24 (JSON carries 9 digits, so allow a quarter LSB of text rounding)
  for (int i = 0; i < 5; i++)
  {
    double v = c["bq"][0][i].as<double>() * 16777216.0;
    assert(std::fabs(v - std::round(v)) < 0.25);
  }
  // the firmware truncates (floor) to 8.24; the plot must show what the DSP gets
  {
    const float* a = port.bq[0].a;
    const float* b = port.bq[0].b;
    const double sent[5] = { b[0], b[1], b[2], a[1], a[2] };
    for (int i = 0; i < 5; i++)
      assert(std::fabs(c["bq"][0][i].as<double>() * 16777216.0 - std::floor(sent[i] * 16777216.0)) < 0.25);
  }
  // the gain at the centre frequency is exactly the Slot gain, flat far away
  assert(std::fabs(magDb(c, 1000) - 6.0) < 0.01);
  assert(std::fabs(magDb(c, 20)) < 0.1);
  assert(std::fabs(magDb(c, 20000)) < 0.6);
  assert(coeffsOf(m, "out", 0)["bq"].size() == 0);
}

static void budgetRejectsAndAddDisabled()
{
  FakePort port;
  MatrixModel m(port, 2, 8, 2);
  assert(patch(m, kAddPeq).empty());
  assert(patch(m, kAddPeq).empty());
  size_t n = port.bq.size();
  DynamicJsonDocument st = stateOf(m);
  assert(st["outs"][1]["free"] == 0);
  assert(st["outs"][0]["free"] > 0 && !st["outs"][1].containsKey("addable"));
  assert(!patch(m, kAddPeq).empty());  // beyond the budget
  assert(port.bq.size() == n);
  assert(stateOf(m)["outs"][1]["slots"].size() == 2);
}

static void slotValuesAreValidated()
{
  FakePort port;
  MatrixModel m(port, 2, 8, 4);
  assert(!patch(m, "{\"ch\":\"out\",\"idx\":1,\"slot\":{\"op\":\"add\",\"freq\":5}}").empty());
  assert(!patch(m, "{\"ch\":\"out\",\"idx\":1,\"slot\":{\"op\":\"add\",\"freq\":30000}}").empty());
  assert(!patch(m, "{\"ch\":\"out\",\"idx\":1,\"slot\":{\"op\":\"add\",\"q\":0}}").empty());
  assert(!patch(m, "{\"ch\":\"out\",\"idx\":1,\"slot\":{\"op\":\"add\",\"gain\":99}}").empty());
  assert(!patch(m, "{\"ch\":\"out\",\"idx\":1,\"slot\":{\"op\":\"nope\"}}").empty());
  assert(!patch(m, "{\"ch\":\"out\",\"idx\":1,\"slot\":{\"op\":\"del\",\"i\":0}}").empty());
  assert(!patch(m, "{\"ch\":\"out\",\"idx\":1,\"slot\":{\"op\":\"set\",\"i\":3,\"gain\":1}}").empty());
  assert(port.bq.empty() && !m.dirty());
}

static void editWritesOnlyTheChangedBiquad()
{
  FakePort port;
  MatrixModel m(port, 2, 8, 4);
  patch(m, kAddPeq);
  patch(m, kAddPeq);
  port.bq.clear();
  assert(patch(m, "{\"ch\":\"out\",\"idx\":1,\"slot\":{\"op\":\"set\",\"i\":1,\"freq\":2000}}").empty());
  assert(port.bq.size() == 1 && port.bq[0].stage == 1);
  assert(stateOf(m)["outs"][1]["slots"][1]["freq"] == 2000);
  assert(stateOf(m)["outs"][1]["slots"][0]["freq"] == 1000);
}

static void bypassWritesPassthroughAndKeepsAllocation()
{
  FakePort port;
  MatrixModel m(port, 2, 8, 2);
  patch(m, kAddPeq);
  patch(m, kAddPeq);
  port.bq.clear();
  assert(patch(m, "{\"ch\":\"out\",\"idx\":1,\"slot\":{\"op\":\"set\",\"i\":0,\"bypass\":true}}").empty());
  assert(port.bq.size() == 1 && port.bq[0].stage == 0 && isPassthrough(port.bq[0]));
  DynamicJsonDocument st = stateOf(m);
  assert(st["outs"][1]["used"] == 2 && st["outs"][1]["free"] == 0);
  assert(st["outs"][1]["slots"][0]["bypass"] == true);
  assert(coeffsOf(m, "out", 1)["bq"].size() == 1);  // only the active one
  // un-bypass never fails for lack of budget
  assert(patch(m, "{\"ch\":\"out\",\"idx\":1,\"slot\":{\"op\":\"set\",\"i\":0,\"bypass\":false}}").empty());
  assert(!isPassthrough(port.bq.back()));
}

static void deleteRepacksAndFreesTheEnd()
{
  FakePort port;
  MatrixModel m(port, 2, 8, 4);
  patch(m, "{\"ch\":\"bus\",\"idx\":0,\"slot\":{\"op\":\"add\",\"freq\":100}}");
  patch(m, "{\"ch\":\"bus\",\"idx\":0,\"slot\":{\"op\":\"add\",\"freq\":200}}");
  patch(m, "{\"ch\":\"bus\",\"idx\":0,\"slot\":{\"op\":\"add\",\"freq\":300}}");
  port.bq.clear();
  assert(patch(m, "{\"ch\":\"bus\",\"idx\":0,\"slot\":{\"op\":\"del\",\"i\":0}}").empty());
  // stages 0 and 1 take the old slots 1 and 2; stage 2 is freed
  assert(port.bq.size() == 3);
  assert(port.bq[0].stage == 0 && !isPassthrough(port.bq[0]));
  assert(port.bq[1].stage == 1 && !isPassthrough(port.bq[1]));
  assert(port.bq[2].stage == 2 && isPassthrough(port.bq[2]));
  DynamicJsonDocument st = stateOf(m);
  assert(st["buses"][0]["slots"].size() == 2 && st["buses"][0]["slots"][0]["freq"] == 200);
  assert(st["buses"][0]["free"] == 2);
}

static void slotsRoundTripThroughPresetsAndReemit()
{
  FakePort port;
  MatrixModel a(port, 2, 8, 4);
  patch(a, kAddPeq);
  patch(a, "{\"ch\":\"out\",\"idx\":1,\"slot\":{\"op\":\"set\",\"i\":0,\"bypass\":true}}");
  std::string saved = a.savePreset();
  FakePort port2;
  MatrixModel b(port2, 2, 8, 4);
  assert(b.loadPreset(saved).empty());
  assert(b.savePreset() == saved);
  assert(port2.bq.size() == 10 * 4);  // every biquad of every Channel
  assert(stateOf(b)["outs"][1]["slots"][0]["bypass"] == true);
  // a preset without slots (older file) still loads
  FakePort port3;
  MatrixModel c(port3, 2, 8, 4);
  std::string fresh = c.savePreset();
  assert(b.loadPreset(fresh).empty());
  assert(stateOf(b)["outs"][1]["slots"].size() == 0);
}

static void undoRestoresSlots()
{
  FakePort port;
  MatrixModel m(port, 2, 8, 4);
  const std::string before = m.savePreset();
  patch(m, kAddPeq);
  m.undo();
  assert(m.savePreset() == before);
}

//==============================================================================
// Slot types: HP, LP, Shelf, Phase and their biquad cost
static std::string slotPatch(const char* ch, int idx, const std::string& slot)
{
  return std::string("{\"ch\":\"") + ch + "\",\"idx\":" + std::to_string(idx) + ",\"slot\":{" + slot + "}}";
}

static std::string addSlot(MatrixModel& m, const std::string& body, const char* ch = "out", int idx = 1)
{
  return patch(m, slotPatch(ch, idx, "\"op\":\"add\"," + body).c_str());
}

static std::string setSlot(MatrixModel& m, int i, const std::string& body, const char* ch = "out", int idx = 1)
{
  return patch(m, slotPatch(ch, idx, "\"op\":\"set\",\"i\":" + std::to_string(i) + "," + body).c_str());
}

static void slotCostPerTypeAndOrder()
{
  struct { const char* body; int cost; } cases[] = {
    { "\"type\":\"peq\"", 1 },
    { "\"type\":\"lshelf\"", 1 },
    { "\"type\":\"hshelf\"", 1 },
    { "\"type\":\"phase\"", 1 },
    { "\"type\":\"hp\",\"design\":\"bw\",\"order\":1", 1 },
    { "\"type\":\"hp\",\"design\":\"bw\",\"order\":2", 1 },
    { "\"type\":\"hp\",\"design\":\"bw\",\"order\":3", 2 },
    { "\"type\":\"lp\",\"design\":\"bw\",\"order\":5", 3 },
    { "\"type\":\"lp\",\"design\":\"bw\",\"order\":7", 4 },
    { "\"type\":\"hp\",\"design\":\"bw\",\"order\":8", 4 },
    { "\"type\":\"lp\",\"design\":\"lr\",\"order\":2", 1 },
    { "\"type\":\"hp\",\"design\":\"lr\",\"order\":4", 2 },
    { "\"type\":\"lp\",\"design\":\"lr\",\"order\":8", 4 },
  };
  for (size_t k = 0; k < sizeof(cases) / sizeof(cases[0]); k++)
  {
    FakePort port;
    MatrixModel m(port, 2, 8, 8);
    assert(addSlot(m, cases[k].body).empty());
    DynamicJsonDocument st = stateOf(m);
    assert(st["outs"][1]["used"] == cases[k].cost && st["outs"][1]["free"] == 8 - cases[k].cost);
    assert((int)port.bq.size() == cases[k].cost);  // one write per biquad of the Slot, in order
    for (int s = 0; s < cases[k].cost; s++)
      assert(port.bq[s].stage == s && !isPassthrough(port.bq[s]));
  }
}

static void slotsArepackedInOrder()
{
  FakePort port;
  MatrixModel m(port, 2, 8, 8);
  addSlot(m, "\"type\":\"peq\"");
  addSlot(m, "\"type\":\"hp\",\"design\":\"bw\",\"order\":4");
  addSlot(m, "\"type\":\"phase\"");
  port.bq.clear();
  // deleting the first Slot moves the BW4 to stages 0-1, the Phase to 2; stage 3 is freed
  assert(patch(m, slotPatch("out", 1, "\"op\":\"del\",\"i\":0").c_str()).empty());
  assert(port.bq.size() == 4);
  assert(!isPassthrough(port.bq[0]) && !isPassthrough(port.bq[1]) && !isPassthrough(port.bq[2]));
  assert(port.bq[3].stage == 3 && isPassthrough(port.bq[3]));
  assert(coeffsOf(m, "out", 1)["bq"].size() == 3);
}

static void multiBiquadEditsAreRampedAndSingleBiquadEditsStayLive()
{
  FakePort port;
  MatrixModel m(port, 2, 8, 5);
  assert(patch(m, "{\"ch\":\"bus\",\"idx\":0,\"gain\":-6}").empty());
  assert(addSlot(m, "\"type\":\"peq\",\"freq\":200", "bus", 0).empty());
  assert(addSlot(m, "\"type\":\"lp\",\"design\":\"lr\",\"order\":4,\"freq\":2000", "bus", 0).empty());

  port.events.clear();
  assert(setSlot(m, 0, "\"freq\":300", "bus", 0).empty());
  assert(port.events.size() == 1 && port.events[0].type == 'b' && port.events[0].val == 0);

  port.events.clear();
  assert(setSlot(m, 1, "\"order\":8", "bus", 0).empty());
  assert(port.events.size() == 7);
  assert(port.events[0].type == 'g' && near(port.events[0].val, 0.0));
  assert(port.events[1].type == 'w' && port.events[1].val >= MATRIX_RAMP_MS);
  for (int stage = 1; stage <= 4; stage++)
    assert(port.events[stage + 1].type == 'b' && port.events[stage + 1].idx == 0 && port.events[stage + 1].val == stage);
  assert(port.events[6].type == 'g' && near(port.events[6].val, std::pow(10.0, -6.0 / 20)));

  port.events.clear();
  assert(!setSlot(m, 1, "\"order\":7", "bus", 0).empty());
  assert(port.events.empty());
  DynamicJsonDocument st = stateOf(m);
  assert(st["buses"][0]["gain"] == -6);

  port.events.clear();
  assert(patch(m, slotPatch("bus", 0, "\"op\":\"del\",\"i\":0").c_str()).empty());
  assert(port.events.size() == 8);
  assert(port.events[0].type == 'g' && near(port.events[0].val, 0.0));
  assert(port.events[1].type == 'w');
  for (int stage = 0; stage < 5; stage++)
    assert(port.events[stage + 2].type == 'b' && port.events[stage + 2].val == stage);
  assert(port.events[7].type == 'g' && near(port.events[7].val, std::pow(10.0, -6.0 / 20)));

  port.events.clear();
  assert(setSlot(m, 0, "\"type\":\"peq\"", "bus", 0).empty());
  assert(port.events.size() == 7);
  assert(port.events.front().type == 'g' && near(port.events.front().val, 0.0));
  assert(port.events[1].type == 'w');
  assert(port.events.back().type == 'g' && near(port.events.back().val, std::pow(10.0, -6.0 / 20)));
}

static void bypassOfAMultiBiquadSlotKeepsItsStages()
{
  FakePort port;
  MatrixModel m(port, 2, 8, 4);
  addSlot(m, "\"type\":\"lp\",\"design\":\"lr\",\"order\":8");
  port.bq.clear();
  assert(setSlot(m, 0, "\"bypass\":true").empty());
  assert(port.bq.size() == 4);
  for (int s = 0; s < 4; s++)
    assert(isPassthrough(port.bq[s]));
  assert(stateOf(m)["outs"][1]["free"] == 0);
  assert(coeffsOf(m, "out", 1)["bq"].size() == 0);
  assert(setSlot(m, 0, "\"bypass\":false").empty());
}

static void typeCoefficientsGiveTheExpectedResponse()
{
  FakePort port;
  MatrixModel m(port, 2, 8, 16);
  addSlot(m, "\"type\":\"hp\",\"design\":\"bw\",\"order\":4,\"freq\":1000");
  DynamicJsonDocument c = coeffsOf(m, "out", 1);
  assert(c["bq"].size() == 2);
  assert(std::fabs(magDb(c, 1000) + 3.0103) < 0.1);
  // an octave below the corner: |H|^2 = 1 / (1 + (tan(wc/2) / tan(w/2))^8)
  const double t = std::tan(M_PI * 1000 / 48000) / std::tan(M_PI * 500 / 48000);
  assert(std::fabs(magDb(c, 500) + 10 * std::log10(1 + std::pow(t, 8))) < 0.1);
  assert(std::fabs(magDb(c, 10000)) < 0.1);

  FakePort p2;
  MatrixModel m2(p2, 2, 8, 16);
  addSlot(m2, "\"type\":\"lp\",\"design\":\"lr\",\"order\":8,\"freq\":2000");
  c = coeffsOf(m2, "out", 1);
  assert(c["bq"].size() == 4);
  assert(std::fabs(magDb(c, 2000) + 6.0206) < 0.1);
  assert(std::fabs(magDb(c, 200)) < 0.1);
  assert(magDb(c, 4000) < -45);  // 48 dB/oct: -48 dB one octave up, less the corner offset

  FakePort p3;
  MatrixModel m3(p3, 2, 8, 16);
  addSlot(m3, "\"type\":\"lshelf\",\"freq\":200,\"gain\":6,\"slope\":1");
  addSlot(m3, "\"type\":\"hshelf\",\"freq\":5000,\"gain\":-6,\"slope\":1");
  c = coeffsOf(m3, "out", 1);
  assert(std::fabs(magDb(c, 20) - 6.0) < 0.3);
  assert(std::fabs(magDb(c, 1000)) < 0.7);
  assert(std::fabs(magDb(c, 20000) + 6.0) < 0.5);

  FakePort p4;
  MatrixModel m4(p4, 2, 8, 16);
  addSlot(m4, "\"type\":\"phase\",\"freq\":1000,\"q\":0.7");
  c = coeffsOf(m4, "out", 1);
  assert(c["bq"].size() == 1);
  for (double f : { 30.0, 500.0, 1000.0, 3000.0, 15000.0 })
    assert(std::fabs(magDb(c, f)) < 0.05);  // allpass: flat magnitude
  assert(!isPassthrough(p4.bq[0]));
}

static void slopeChangesTheShelf()
{
  FakePort port;
  MatrixModel m(port, 2, 8, 4);
  addSlot(m, "\"type\":\"lshelf\",\"freq\":1000,\"gain\":6,\"slope\":1");
  FakePort::Bq steep = port.bq[0];
  port.bq.clear();
  assert(setSlot(m, 0, "\"slope\":0.3").empty());
  assert(port.bq.size() == 1 && port.bq[0].b[1] != steep.b[1]);
  assert(stateOf(m)["outs"][1]["slots"][0]["slope"].as<double>() > 0.29);
  assert(!setSlot(m, 0, "\"slope\":0").empty());
  assert(!setSlot(m, 0, "\"slope\":1.5").empty());
}

static void slotTypeAndOrderAreValidated()
{
  FakePort port;
  MatrixModel m(port, 2, 8, 16);
  assert(!addSlot(m, "\"type\":\"notch\"").empty());
  assert(!addSlot(m, "\"type\":\"hp\",\"design\":\"bessel\"").empty());
  assert(!addSlot(m, "\"type\":\"hp\",\"design\":\"bw\",\"order\":0").empty());
  assert(!addSlot(m, "\"type\":\"hp\",\"design\":\"bw\",\"order\":9").empty());
  assert(!addSlot(m, "\"type\":\"hp\",\"design\":\"lr\",\"order\":3").empty());
  assert(!addSlot(m, "\"type\":\"hp\",\"design\":\"lr\",\"order\":6").empty());
  assert(!addSlot(m, "\"type\":\"hp\",\"design\":\"bw\",\"order\":\"2\"").empty());
  assert(port.bq.empty() && stateOf(m)["outs"][1]["slots"].size() == 0);
}

// "type/design/order" of the choices a Channel offers for a new Slot
static std::vector<std::string> addable(const DynamicJsonDocument& st, const char* ch, int idx)
{
  std::vector<std::string> r;
  // as the page does: every choice whose cost fits the channel's free biquads
  for (size_t i = 0; i < st["choices"].size(); i++)
  {
    JsonVariantConst c = st["choices"][i];
    if (c["c"].as<int>() > st[ch][idx]["free"].as<int>())
      continue;
    std::string s = c["t"].as<const char*>();
    if (c["d"].is<const char*>())
      s += std::string(":") + c["d"].as<const char*>() + std::to_string(c["o"].as<int>());
    r.push_back(s);
  }
  return r;
}

static bool has(const std::vector<std::string>& v, const char* s)
{
  for (size_t i = 0; i < v.size(); i++)
    if (v[i] == s)
      return true;
  return false;
}

static void selectableListFollowsTheFreeBudget()
{
  FakePort port;
  MatrixModel m(port, 2, 8, 5);
  assert(addSlot(m, "\"type\":\"lp\",\"design\":\"lr\",\"order\":8").empty());  // 4 of 5 used
  std::vector<std::string> a = addable(stateOf(m), "outs", 1);
  // one biquad left: PEQ, both Shelves, Phase, BW 1-2, LR2 (HP and LP)
  assert(a.size() == 4 + 3 + 3);
  for (const char* s : { "peq", "lshelf", "hshelf", "phase", "hp:bw1", "hp:bw2", "hp:lr2", "lp:bw1", "lp:bw2", "lp:lr2" })
    assert(has(a, s));
  assert(!has(a, "hp:bw3") && !has(a, "lp:lr4") && !has(a, "hp:bw8"));
  // the other Channels still have everything free: 4 + 2 x (8 BW + 3 LR) = 26 choices fit
  assert(addable(stateOf(m), "outs", 0).size() == 26);
  assert(has(addable(stateOf(m), "outs", 0), "lp:lr8") && has(addable(stateOf(m), "outs", 0), "hp:bw8"));

  assert(addSlot(m, "\"type\":\"peq\"").empty());  // budget used up
  assert(addable(stateOf(m), "outs", 1).empty());
  assert(!addSlot(m, "\"type\":\"peq\"").empty());
}

static void freeBudgetOfThreeOffersUpToSixthOrderButterworth()
{
  FakePort port;
  MatrixModel m(port, 2, 8, 3);
  std::vector<std::string> a = addable(stateOf(m), "outs", 0);
  assert(has(a, "hp:bw5") && has(a, "hp:bw6") && has(a, "lp:lr4"));
  assert(!has(a, "hp:bw7") && !has(a, "hp:bw8") && !has(a, "lp:lr8"));
}

static void raisingTheOrderBeyondTheBudgetIsRejected()
{
  FakePort port;
  MatrixModel m(port, 2, 8, 4);
  addSlot(m, "\"type\":\"hp\",\"design\":\"bw\",\"order\":2,\"freq\":100");  // 1 biquad
  addSlot(m, "\"type\":\"peq\"");                                           // 1 biquad, 2 free
  // the Slot may use its own biquad plus the free ones: up to 3
  DynamicJsonDocument st = stateOf(m);
  assert(st["outs"][1]["slots"][0]["avail"] == 3 && st["outs"][1]["slots"][1]["avail"] == 3);
  port.bq.clear();
  const std::string before = m.savePreset();
  assert(!setSlot(m, 0, "\"order\":8").empty());                      // would need 4
  assert(!setSlot(m, 0, "\"design\":\"lr\",\"order\":8").empty());    // would need 4
  assert(!setSlot(m, 1, "\"type\":\"hp\",\"order\":8").empty());      // type change counts too
  assert(port.bq.empty() && m.savePreset() == before);
  assert(setSlot(m, 0, "\"order\":6").empty());                        // 3 biquads: fits exactly
  st = stateOf(m);
  assert(st["outs"][1]["used"] == 4 && st["outs"][1]["free"] == 0);
  assert(st["outs"][1]["slots"][0]["avail"] == 3 && st["outs"][1]["slots"][1]["avail"] == 1);
  assert(!setSlot(m, 0, "\"order\":8").empty());
  // lowering the order frees biquads and repacks the Slots behind it
  port.bq.clear();
  assert(setSlot(m, 0, "\"order\":2").empty());
  assert(port.bq.size() >= 3);  // stage 0 (new HP), stage 1 (the PEQ moved up), stages 2-3 passthrough
  assert(stateOf(m)["outs"][1]["free"] == 2);
}

static void slotTypesRoundTripThroughPresets()
{
  FakePort port;
  MatrixModel a(port, 2, 8, 12);
  addSlot(a, "\"type\":\"hp\",\"design\":\"lr\",\"order\":4,\"freq\":80");
  addSlot(a, "\"type\":\"lshelf\",\"freq\":200,\"gain\":3,\"slope\":0.5");
  addSlot(a, "\"type\":\"phase\",\"freq\":900,\"q\":2");
  addSlot(a, "\"type\":\"lp\",\"design\":\"bw\",\"order\":5,\"freq\":5000");
  setSlot(a, 3, "\"bypass\":true");
  std::string saved = a.savePreset();
  FakePort port2;
  MatrixModel b(port2, 2, 8, 12);
  assert(b.loadPreset(saved).empty());
  assert(b.savePreset() == saved);
  DynamicJsonDocument st = stateOf(b);
  assert(std::string(st["outs"][1]["slots"][3]["type"].as<const char*>()) == "lp");
  assert(st["outs"][1]["slots"][3]["order"] == 5 && st["outs"][1]["slots"][3]["bypass"] == true);
  assert(st["outs"][1]["used"] == 2 + 1 + 1 + 3);
  // a preset that needs more biquads than the plugin has is refused
  FakePort port3;
  MatrixModel small(port3, 2, 8, 6);
  assert(!small.loadPreset(saved).empty());
  // a Slot saved before the types existed is a PEQ
  FakePort port4;
  MatrixModel c(port4, 2, 8, 4);
  addSlot(c, "\"freq\":1000");
  std::string legacy = c.savePreset();
  for (const char* key : { "\"type\":\"peq\",", "\"slope\":1," })
  {
    size_t p = legacy.find(key);
    if (p != std::string::npos)
      legacy.erase(p, strlen(key));
  }
  FakePort port5;
  MatrixModel d(port5, 2, 8, 4);
  assert(d.loadPreset(legacy).empty());
  assert(std::string(stateOf(d)["outs"][1]["slots"][0]["type"].as<const char*>()) == "peq");
}

static void nullOrNonStringTypeIsRejected()
{
  FakePort port;
  MatrixModel m(port, 2, 8, 4);
  const std::string before = m.savePreset();
  assert(!addSlot(m, "\"type\":null").empty());
  assert(!addSlot(m, "\"type\":5").empty());
  assert(!addSlot(m, "\"type\":\"bogus\"").empty());
  assert(port.bq.empty() && m.savePreset() == before);
  addSlot(m, "\"type\":\"peq\"");
  assert(!setSlot(m, 0, "\"type\":null").empty());
  assert(!setSlot(m, 0, "\"type\":7").empty());
  // a preset Slot with a null or non-string type is bad, not a PEQ
  std::string saved = m.savePreset();
  for (const char* bad : { "\"type\":null", "\"type\":5" })
  {
    std::string p = saved;
    p.replace(p.find("\"type\":\"peq\""), 12, bad);
    FakePort port2;
    MatrixModel m2(port2, 2, 8, 4);
    assert(m2.loadPreset(p) == "bad slot");
  }
}

static void typeChangeResetsHiddenDesignAndOrder()
{
  FakePort port;
  MatrixModel m(port, 2, 8, 5);
  assert(addSlot(m, "\"type\":\"hp\",\"design\":\"lr\",\"order\":8").empty());  // 4 of 5 used
  assert(setSlot(m, 0, "\"type\":\"peq\"").empty());                             // 1 used
  for (int k = 0; k < 3; k++)
    assert(addSlot(m, "\"type\":\"peq\"").empty());                              // 4 used, 1 free
  assert(setSlot(m, 0, "\"type\":\"hp\"").empty());                              // back to HP: cheapest default, not LR8
  DynamicJsonDocument st = stateOf(m);
  assert(std::string(st["outs"][1]["slots"][0]["design"].as<const char*>()) == "bw");
  assert(st["outs"][1]["slots"][0]["order"] == 1);
  assert(st["outs"][1]["used"] == 4);
}

static void garbageDesignAndOrderAreNeverStored()
{
  FakePort port;
  MatrixModel m(port, 2, 8, 4);
  assert(addSlot(m, "\"type\":\"peq\",\"order\":99,\"design\":\"lr\"").empty());
  assert(addSlot(m, "\"type\":\"phase\",\"order\":99").empty());
  assert(setSlot(m, 0, "\"order\":99").empty());
  std::string saved = m.savePreset();
  assert(saved.find("99") == std::string::npos);
  // a switch to HP never sees the ignored values
  assert(setSlot(m, 0, "\"type\":\"hp\"").empty());
  assert(stateOf(m)["outs"][1]["slots"][0]["order"] == 1);
}

static void presetSlotFieldsAreTypeChecked()
{
  FakePort port;
  MatrixModel a(port, 2, 8, 4);
  addSlot(a, "\"type\":\"hp\",\"design\":\"bw\",\"order\":2");
  std::string saved = a.savePreset();
  struct { const char* from; const char* to; } bad[] = {
    { "\"design\":\"bw\"", "\"design\":\"xx\"" }, { "\"design\":\"bw\"", "\"design\":3" },
    { "\"order\":2", "\"order\":2.5" },           { "\"order\":2", "\"order\":\"2\"" },
    { "\"slope\":1", "\"slope\":\"1\"" },         { "\"slope\":1", "\"slope\":true" },
  };
  for (size_t k = 0; k < sizeof(bad) / sizeof(bad[0]); k++)
  {
    std::string p = saved;
    size_t at = p.find(bad[k].from);
    assert(at != std::string::npos);
    p.replace(at, strlen(bad[k].from), bad[k].to);
    FakePort port2;
    MatrixModel b(port2, 2, 8, 4);
    assert(b.loadPreset(p) == "bad slot");
  }
  // optional fields may be missing
  std::string p = saved;
  p.erase(p.find("\"slope\":1,"), 10);
  FakePort port3;
  MatrixModel c(port3, 2, 8, 4);
  assert(c.loadPreset(p).empty());
}

static void presetVersionIsBumpedAndOldOnesStillLoad()
{
  FakePort port;
  MatrixModel a(port, 2, 8, 4);
  addSlot(a, "\"type\":\"peq\"");
  std::string saved = a.savePreset();
  assert(saved.find("\"v\":3") != std::string::npos);
  std::string v1 = saved;
  v1.replace(v1.find("\"v\":3"), 5, "\"v\":1");
  FakePort port2;
  MatrixModel b(port2, 2, 8, 4);
  assert(b.loadPreset(v1).empty());
  assert(b.savePreset() == saved);
  std::string v4 = saved;
  v4.replace(v4.find("\"v\":3"), 5, "\"v\":4");
  assert(!b.loadPreset(v4).empty());
}

static std::vector<Write> only(const FakePort& port, char type)
{
  std::vector<Write> r;
  for (size_t i = 0; i < port.w.size(); i++)
    if (port.w[i].type == type)
      r.push_back(port.w[i]);
  return r;
}

static void delayMicrosecondsRoundToSamples()
{
  FakePort port;
  MatrixModel m(port, 2, 8);
  assert(patch(m, "{\"ch\":\"out\",\"idx\":3,\"delayUs\":1000}").empty());
  std::vector<Write> d = only(port, 'd');
  assert(d.size() == 1 && d[0].idx == 3 && d[0].val == 48);
  DynamicJsonDocument st = stateOf(m);
  assert(st["outs"][3]["delaySamples"] == 48);
  assert(near(st["outs"][3]["delayUs"].as<double>(), 1000.0));
  assert(near(st["outs"][3]["delayMm"].as<double>(), 343.0));
  assert(st["outs"][3]["delayBypass"] == false);

  // 100 us = 4.8 samples -> 5 samples = 104.1666 us
  assert(patch(m, "{\"ch\":\"out\",\"idx\":3,\"delayUs\":100}").empty());
  st = stateOf(m);
  assert(st["outs"][3]["delaySamples"] == 5);
  assert(std::fabs(st["outs"][3]["delayUs"].as<double>() - 104.1667) < 1e-3);
  assert(std::fabs(st["outs"][3]["delayMm"].as<double>() - 35.729) < 1e-2);
  assert(st["delaymax"] == 1920);
}

static void delayBoundaryIs40ms()
{
  FakePort port;
  MatrixModel m(port, 2, 8);
  assert(patch(m, "{\"ch\":\"out\",\"idx\":0,\"delayUs\":40000}").empty());
  assert(only(port, 'd').back().val == 1920);
  port.w.clear();
  assert(!patch(m, "{\"ch\":\"out\",\"idx\":0,\"delayUs\":40021}").empty());  // 1921 samples
  assert(!patch(m, "{\"ch\":\"out\",\"idx\":0,\"delayUs\":-1}").empty());
  assert(!patch(m, "{\"ch\":\"out\",\"idx\":0,\"delayUs\":\"x\"}").empty());
  assert(port.w.empty());
  assert(stateOf(m)["outs"][0]["delaySamples"] == 1920);
}

static void delayLimitFollowsThePlugin()
{
  FakePort port;
  MatrixModel m(port, 2, 8, 0, 480);  // plugin.ini maxdly = 10 ms
  assert(stateOf(m)["delaymax"] == 480);
  assert(patch(m, "{\"ch\":\"out\",\"idx\":0,\"delayUs\":10000}").empty());
  assert(!patch(m, "{\"ch\":\"out\",\"idx\":0,\"delayUs\":10021}").empty());
  // a larger plugin value is capped at 1920, a missing one means 1920
  MatrixModel big(port, 2, 8, 0, 5000);
  assert(stateOf(big)["delaymax"] == 1920);
  MatrixModel none(port, 2, 8, 0, 0);
  assert(stateOf(none)["delaymax"] == 1920);
  // presets beyond the limit are refused
  MatrixModel src(port, 2, 8);
  patch(src, "{\"ch\":\"out\",\"idx\":0,\"delayUs\":20000}");
  assert(!m.loadPreset(src.savePreset()).empty());
}

static void invalidJsonReasonIsAccurate()
{
  FakePort port;
  MatrixModel m(port, 2, 8);
  assert(m.loadPreset("not json").find("not valid JSON") != std::string::npos);
}

static void delayBypassWritesZeroAndKeepsValue()
{
  FakePort port;
  MatrixModel m(port, 2, 8);
  patch(m, "{\"ch\":\"out\",\"idx\":1,\"delayUs\":1000}");
  port.w.clear();
  assert(patch(m, "{\"ch\":\"out\",\"idx\":1,\"delayBypass\":true}").empty());
  assert(only(port, 'd').size() == 1 && only(port, 'd')[0].val == 0);
  assert(stateOf(m)["outs"][1]["delaySamples"] == 48);
  // editing while bypassed stores the value but writes nothing
  port.w.clear();
  assert(patch(m, "{\"ch\":\"out\",\"idx\":1,\"delayUs\":2000}").empty());
  assert(port.w.empty());
  assert(patch(m, "{\"ch\":\"out\",\"idx\":1,\"delayBypass\":false}").empty());
  assert(only(port, 'd').size() == 1 && only(port, 'd')[0].val == 96);
}

static void polarityWritesAndIsOutputsOnly()
{
  FakePort port;
  MatrixModel m(port, 2, 8);
  assert(patch(m, "{\"ch\":\"out\",\"idx\":5,\"polarity\":true}").empty());
  assert(port.w.size() == 1 && port.w[0].type == 'p' && port.w[0].idx == 5 && port.w[0].val == 1.0);
  assert(stateOf(m)["outs"][5]["polarity"] == true);
  port.w.clear();
  assert(patch(m, "{\"ch\":\"out\",\"idx\":5,\"polarity\":true}").empty());
  assert(port.w.empty());
  assert(!patch(m, "{\"ch\":\"out\",\"idx\":5,\"polarity\":1}").empty());

  // Buses have neither polarity nor delay
  assert(!patch(m, "{\"ch\":\"bus\",\"idx\":0,\"polarity\":true}").empty());
  assert(!patch(m, "{\"ch\":\"bus\",\"idx\":0,\"delayUs\":100}").empty());
  assert(!patch(m, "{\"ch\":\"bus\",\"idx\":0,\"delayBypass\":true}").empty());
  assert(port.w.empty());
  DynamicJsonDocument st = stateOf(m);
  assert(!st["buses"][0].containsKey("polarity") && !st["buses"][0].containsKey("delaySamples"));
}

static void gainBypassWritesUnityAndKeepsGain()
{
  FakePort port;
  MatrixModel m(port, 2, 8);
  patch(m, "{\"ch\":\"out\",\"idx\":2,\"mute\":false,\"gain\":-6}");
  port.w.clear();
  assert(patch(m, "{\"ch\":\"out\",\"idx\":2,\"gainBypass\":true}").empty());
  assert(port.w.size() == 1 && port.w[0].type == 'g' && near(port.w[0].val, 1.0));
  DynamicJsonDocument st = stateOf(m);
  assert(st["outs"][2]["gainBypass"] == true && st["outs"][2]["gain"] == -6);
  // mute wins over bypass
  port.w.clear();
  assert(patch(m, "{\"ch\":\"out\",\"idx\":2,\"mute\":true}").empty());
  assert(port.w.size() == 1 && port.w[0].val == 0.0);
  assert(patch(m, "{\"ch\":\"bus\",\"idx\":1,\"gainBypass\":true}").empty());  // Buses have gain bypass too
  assert(!patch(m, "{\"ch\":\"bus\",\"idx\":1,\"gainBypass\":3}").empty());
}

static void polarityDelayAndBypassRoundTripThroughPresets()
{
  FakePort port;
  MatrixModel m(port, 2, 8);
  patch(m, "{\"ch\":\"out\",\"idx\":4,\"polarity\":true,\"delayUs\":1000,\"delayBypass\":true,\"gainBypass\":true}");
  std::string saved = m.savePreset();
  FakePort port2;
  MatrixModel m2(port2, 2, 8);
  assert(m2.loadPreset(saved).empty());
  assert(m2.savePreset() == saved);
  assert(only(port2, 'p').size() == 8 && only(port2, 'd').size() == 8);
  assert(only(port2, 'p')[4].val == 1.0 && only(port2, 'd')[4].val == 0);
  DynamicJsonDocument st = stateOf(m2);
  assert(st["outs"][4]["delaySamples"] == 48 && st["outs"][4]["delayBypass"] == true && st["outs"][4]["gainBypass"] == true);
  // undo restores the new fields
  patch(m2, "{\"ch\":\"out\",\"idx\":4,\"polarity\":false}");
  m2.undo();
  assert(stateOf(m2)["outs"][4]["polarity"] == true);
}

static void spdifOutDefaultsAndPairs()
{
  FakePort port;
  MatrixModel m(port, 2, 8);
  DynamicJsonDocument st = stateOf(m);
  assert(st["spdifout"] == 0);  // Analog 1+2
  // 4 Analog + 4 USB + 4 Exp pairs, SPDIF in, 4 Output pairs
  JsonArray opts = st["spdifopts"];
  assert(opts.size() == 17);
  assert(std::string(opts[0]["n"].as<const char*>()) == "Analog 1+2");
  assert(std::string(opts[16]["n"].as<const char*>()) == "Output 7+8");
  assert(opts[16]["v"] == 0x00050006);
}

static void spdifOutEmitsLeftAndRightMuxWrites()
{
  FakePort port;
  MatrixModel m(port, 2, 8);
  // Output 3+4: port 5, channels 2 and 3
  assert(patch(m, "{\"ch\":\"spdif\",\"sel\":327682}").empty());
  assert(port.w.size() == 1 && port.w[0].type == 'o');
  assert((uint32_t)port.w[0].val == 0x00050002 && (uint32_t)port.w[0].idx == 0x00050003);
  assert(stateOf(m)["spdifout"] == 0x00050002);
  // a Source pair
  port.w.clear();
  assert(patch(m, "{\"ch\":\"spdif\",\"sel\":65540}").empty());  // USB 5+6
  assert((uint32_t)port.w[0].val == 0x00010004 && (uint32_t)port.w[0].idx == 0x00010005);
  // unchanged: no write
  port.w.clear();
  assert(patch(m, "{\"ch\":\"spdif\",\"sel\":65540}").empty());
  assert(port.w.empty());
}

static void spdifOutRejectsOddPairsAndUndoes()
{
  FakePort port;
  MatrixModel m(port, 2, 8);
  assert(!patch(m, "{\"ch\":\"spdif\",\"sel\":1}").empty());       // Analog 2+3 is no pair
  assert(!patch(m, "{\"ch\":\"spdif\",\"sel\":196608}").empty());  // ESP32 mux group is hidden
  assert(!patch(m, "{\"ch\":\"spdif\"}").empty());
  assert(port.w.empty() && stateOf(m)["spdifout"] == 0);
  assert(patch(m, "{\"ch\":\"spdif\",\"sel\":327680}").empty());
  m.undo();
  assert(stateOf(m)["spdifout"] == 0);
}

static void spdifOutSurvivesPresetRoundTrip()
{
  FakePort port;
  MatrixModel m(port, 2, 8);
  patch(m, "{\"ch\":\"spdif\",\"sel\":327684}");  // Output 5+6
  assert(m.dirty());
  std::string saved = m.savePreset();
  FakePort port2;
  MatrixModel m2(port2, 2, 8);
  assert(m2.loadPreset(saved).empty());
  assert(m2.savePreset() == saved);
  assert(stateOf(m2)["spdifout"] == 0x00050004);
  assert(only(port2, 'o').size() == 1 && (uint32_t)only(port2, 'o')[0].val == 0x00050004);
  // presets from before the SPDIF out selector load with the default
  DynamicJsonDocument doc(16384);
  assert(!deserializeJson(doc, saved));
  doc.remove("spdif");
  std::string old;
  serializeJson(doc, old);
  assert(m2.loadPreset(old).empty() && stateOf(m2)["spdifout"] == 0);
}

static void linkMembershipHasSeparateBusAndOutputScopes()
{
  FakePort port;
  MatrixModel m(port, 2, 8);
  DynamicJsonDocument st = stateOf(m);
  assert(std::string(st["buses"][0]["link"].as<const char*>()) == "");
  assert(st["links"]["bus"]["mute"] == false);
  assert(st["links"]["outs"].size() == 4);
  assert(std::string(st["links"]["outs"][0]["id"].as<const char*>()) == "A");

  assert(patch(m, "{\"ch\":\"out\",\"idx\":0,\"link\":\"A\"}").empty());
  assert(std::string(stateOf(m)["outs"][0]["link"].as<const char*>()) == "A");
  assert(patch(m, "{\"ch\":\"bus\",\"idx\":0,\"link\":\"bus\"}").empty());
  assert(std::string(stateOf(m)["buses"][0]["link"].as<const char*>()) == "bus");
  assert(!patch(m, "{\"ch\":\"bus\",\"idx\":1,\"link\":\"A\"}").empty());
  assert(!patch(m, "{\"ch\":\"out\",\"idx\":1,\"link\":\"bus\"}").empty());
}

static void linkedAspectsPropagateAndJoiningCopiesAbsoluteValues()
{
  FakePort port;
  MatrixModel m(port, 2, 8, 4);
  assert(patch(m, "{\"ch\":\"out\",\"idx\":0,\"mute\":false,\"gain\":-3,\"delayUs\":1000,\"link\":\"A\"}").empty());
  assert(addSlot(m, "\"type\":\"hp\",\"design\":\"lr\",\"order\":4,\"freq\":120", "out", 0).empty());
  assert(patch(m, "{\"ch\":\"link\",\"scope\":\"out\",\"group\":\"A\",\"mute\":true,\"filters\":true,\"gain\":true,\"delay\":true}").empty());

  port.w.clear(); port.bq.clear();
  assert(patch(m, "{\"ch\":\"out\",\"idx\":1,\"link\":\"A\"}").empty());
  DynamicJsonDocument st = stateOf(m);
  assert(st["outs"][1]["mute"] == false && st["outs"][1]["gain"] == -3);
  assert(st["outs"][1]["delaySamples"] == 48 && st["outs"][1]["slots"].size() == 1);
  assert(only(port, 'g').size() == 2 && only(port, 'g')[0].val == 0.0 && only(port, 'd').size() == 1);
  assert(port.bq.size() == 2 && port.bq[0].idx == 1);

  port.w.clear(); port.bq.clear();
  assert(patch(m, "{\"ch\":\"out\",\"idx\":1,\"mute\":true,\"gain\":-6,\"delayUs\":2000,\"slot\":{\"op\":\"set\",\"i\":0,\"bypass\":true},\"polarity\":true}").empty());
  st = stateOf(m);
  for( int i = 0; i < 2; i++ )
  {
    assert(st["outs"][i]["mute"] == true && st["outs"][i]["gain"] == -6);
    assert(st["outs"][i]["delaySamples"] == 96 && st["outs"][i]["slots"][0]["bypass"] == true);
  }
  assert(st["outs"][0]["polarity"] == false && st["outs"][1]["polarity"] == true);
  assert(only(port, 'g').size() == 2 && only(port, 'd').size() == 2);
  assert(port.bq.size() == 4);  // two stages, for both Outputs

  assert(patch(m, "{\"ch\":\"out\",\"idx\":1,\"link\":\"\"}").empty());
  assert(patch(m, "{\"ch\":\"out\",\"idx\":0,\"mute\":false}").empty());
  st = stateOf(m);
  assert(st["outs"][0]["mute"] == false && st["outs"][1]["mute"] == true); // leaving keeps its value
}

static void eachLinkAspectCanBeEnabledIndependently()
{
  FakePort port;
  MatrixModel m(port, 2, 8, 4);
  patch(m, "{\"ch\":\"out\",\"idx\":2,\"link\":\"B\"}");
  patch(m, "{\"ch\":\"out\",\"idx\":3,\"link\":\"B\"}");
  patch(m, "{\"ch\":\"link\",\"scope\":\"out\",\"group\":\"B\",\"gain\":true}");
  assert(patch(m, "{\"ch\":\"out\",\"idx\":2,\"gain\":4,\"delayUs\":1000,\"mute\":false}").empty());
  DynamicJsonDocument st = stateOf(m);
  assert(st["outs"][3]["gain"] == 4);
  assert(st["outs"][3]["delaySamples"] == 0 && st["outs"][3]["mute"] == true);
  assert(!patch(m, "{\"ch\":\"link\",\"scope\":\"bus\",\"group\":\"bus\",\"delay\":true}").empty());
}

static void enablingAnAspectMakesExistingMembersConsistent()
{
  FakePort port;
  MatrixModel m(port, 2, 8, 4);
  patch(m, "{\"ch\":\"out\",\"idx\":4,\"mute\":false,\"gain\":2,\"link\":\"D\"}");
  patch(m, "{\"ch\":\"out\",\"idx\":5,\"mute\":false,\"gain\":-12,\"link\":\"D\"}");
  port.w.clear();
  assert(patch(m, "{\"ch\":\"link\",\"scope\":\"out\",\"group\":\"D\",\"gain\":true}").empty());
  DynamicJsonDocument st = stateOf(m);
  assert(st["outs"][4]["gain"] == 2 && st["outs"][5]["gain"] == 2);
  assert(only(port, 'g').size() == 1 && only(port, 'g')[0].idx == 5);

  patch(m, "{\"ch\":\"bus\",\"idx\":0,\"link\":\"bus\"}");
  patch(m, "{\"ch\":\"bus\",\"idx\":1,\"link\":\"bus\"}");
  patch(m, "{\"ch\":\"link\",\"scope\":\"bus\",\"group\":\"bus\",\"mute\":true}");
  port.w.clear();
  assert(patch(m, "{\"ch\":\"bus\",\"idx\":1,\"mute\":true}").empty());
  assert(only(port, 'g').size() == 2);
  assert(only(port, 'g')[0].val == 0.0 && only(port, 'g')[1].val == 0.0);
}

static void linkGroupsRoundTripAndUndoTogether()
{
  FakePort port;
  MatrixModel a(port, 2, 8, 4);
  patch(a, "{\"ch\":\"out\",\"idx\":0,\"link\":\"C\"}");
  patch(a, "{\"ch\":\"out\",\"idx\":1,\"link\":\"C\"}");
  patch(a, "{\"ch\":\"link\",\"scope\":\"out\",\"group\":\"C\",\"mute\":true,\"filters\":true,\"gain\":true,\"delay\":false}");
  patch(a, "{\"ch\":\"bus\",\"idx\":0,\"link\":\"bus\"}");
  patch(a, "{\"ch\":\"link\",\"scope\":\"bus\",\"group\":\"bus\",\"filters\":true}");
  std::string saved = a.savePreset();
  FakePort port2;
  MatrixModel b(port2, 2, 8, 4);
  assert(b.loadPreset(saved).empty() && b.savePreset() == saved);
  DynamicJsonDocument st = stateOf(b);
  assert(st["links"]["outs"][2]["mute"] == true && st["links"]["outs"][2]["delay"] == false);
  assert(std::string(st["outs"][1]["link"].as<const char*>()) == "C");
  const std::string before = b.savePreset();
  patch(b, "{\"ch\":\"out\",\"idx\":0,\"mute\":false}");
  b.undo();
  assert(b.savePreset() == before);
}

int main()
{
  linkMembershipHasSeparateBusAndOutputScopes();
  linkedAspectsPropagateAndJoiningCopiesAbsoluteValues();
  eachLinkAspectCanBeEnabledIndependently();
  enablingAnAspectMakesExistingMembersConsistent();
  linkGroupsRoundTripAndUndoTogether();
  delayMicrosecondsRoundToSamples();
  delayBoundaryIs40ms();
  delayLimitFollowsThePlugin();
  invalidJsonReasonIsAccurate();
  delayBypassWritesZeroAndKeepsValue();
  polarityWritesAndIsOutputsOnly();
  gainBypassWritesUnityAndKeepsGain();
  polarityDelayAndBypassRoundTripThroughPresets();
  addPeqWritesItsBiquad();
  peqCoefficientsMatchTheDesign();
  budgetRejectsAndAddDisabled();
  slotValuesAreValidated();
  editWritesOnlyTheChangedBiquad();
  bypassWritesPassthroughAndKeepsAllocation();
  deleteRepacksAndFreesTheEnd();
  slotsRoundTripThroughPresetsAndReemit();
  undoRestoresSlots();
  slotCostPerTypeAndOrder();
  slotsArepackedInOrder();
  multiBiquadEditsAreRampedAndSingleBiquadEditsStayLive();
  bypassOfAMultiBiquadSlotKeepsItsStages();
  typeCoefficientsGiveTheExpectedResponse();
  slopeChangesTheShelf();
  slotTypeAndOrderAreValidated();
  selectableListFollowsTheFreeBudget();
  freeBudgetOfThreeOffersUpToSixthOrderButterworth();
  raisingTheOrderBeyondTheBudgetIsRejected();
  slotTypesRoundTripThroughPresets();
  nullOrNonStringTypeIsRejected();
  typeChangeResetsHiddenDesignAndOrder();
  garbageDesignAndOrderAreNeverStored();
  presetSlotFieldsAreTypeChecked();
  presetVersionIsBumpedAndOldOnesStillLoad();
  undoRestoresTheLastAcceptedPatch();
  spdifOutDefaultsAndPairs();
  spdifOutEmitsLeftAndRightMuxWrites();
  spdifOutRejectsOddPairsAndUndoes();
  spdifOutSurvivesPresetRoundTrip();
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
