// Host tests for the matrix model (Bus Sources, Output routes, mute, gain, names, presets).
// Run: SOURCES/WEBAPP/ESP32/test/run.sh (runs all host tests)
// Links: matrixmodel.cpp AudioFilterFactory.cpp
// Uses: ArduinoJson
#include <cassert>
#include <cmath>
#include <cstdarg>
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
  void writeAddOn(const MatrixAddOn& cfg) override { w.push_back({ 'a', MATRIX_OUT, 0, (double)cfg.data }); }
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
  return m.apply(doc.as<JsonObject>(), now).reason;
}

static MatrixEditResult edit(MatrixModel& m, const char* json, uint32_t now = 0)
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

static bool isPassthrough(const FakePort::Bq& q)
{
  return q.b[0] == 1.0f && q.b[1] == 0 && q.b[2] == 0 && q.a[1] == 0 && q.a[2] == 0;
}

static bool rampHas(const MatrixRampSet& r, MatrixChannelKind kind, int idx)
{
  for (size_t i = 0; i < r.size(); i++)
    if (r[i].kind == kind && r[i].idx == idx) return true;
  return false;
}

// The Baseline (GLOSSARY): no Slots, 0 dB, Buses unmuted on Analog 1 / Analog 2,
// every Output muted and routed from Bus 1.
static void assertBaseline(const MatrixModel& m)
{
  DynamicJsonDocument st = stateOf(m);
  assert(st["buses"][0]["source"] == 0);  // Analog 1
  assert(st["buses"][1]["source"] == 1);  // Analog 2
  for (int b = 0; b < 2; b++)
  {
    assert(st["buses"][b]["mute"] == false);
    assert(st["buses"][b]["gain"] == 0);
    assert(st["buses"][b]["slots"].size() == 0);
  }
  for (int o = 0; o < 8; o++)
  {
    assert(st["outs"][o]["route"] == 0);  // Bus 1
    assert(st["outs"][o]["mute"] == true);
    assert(st["outs"][o]["gain"] == 0);
    assert(st["outs"][o]["slots"].size() == 0);
  }
  assert(std::string(st["outs"][3]["name"].as<const char*>()) == "Output 4");
}

static void freshModelIsTheBaseline()
{
  FakePort port;
  MatrixModel m(port, 2, 8);
  assertBaseline(m);
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

static void presetRoundTripWritesOnlyTheDifference()
{
  FakePort port;
  MatrixModel a(port, 2, 8);
  patch(a, "{\"ch\":\"bus\",\"idx\":0,\"source\":131075}");  // Exp 4
  patch(a, "{\"ch\":\"out\",\"idx\":5,\"name\":\"Sub \\\"mono\\\"\",\"route\":2,\"mute\":false,\"gain\":-3.5}");
  patch(a, "{\"ch\":\"bus\",\"idx\":1,\"mute\":true}");
  std::string saved = a.savePreset();

  FakePort port2;
  MatrixModel b(port2, 2, 8);
  MatrixRampSet ramp;
  assert(b.loadPreset(saved, &ramp).empty());
  assert(b.savePreset() == saved);  // unchanged
  // against the Baseline: Bus 1 Source, Bus 2 mute, Output 6 route; Output 6's gain
  // comes last because its route changed (it ends the ramp of ticket 3)
  assert(port2.w.size() == 4);
  assert(port2.w[0].type == 's' && port2.w[0].idx == 0 && port2.w[0].val == 131075);
  assert(port2.w[1].type == 'g' && port2.w[1].kind == MATRIX_BUS && port2.w[1].idx == 1 && port2.w[1].val == 0.0);
  assert(port2.w[2].type == 'r' && port2.w[2].idx == 5 && port2.w[2].val == 2);
  assert(port2.w[3].type == 'g' && port2.w[3].kind == MATRIX_OUT && port2.w[3].idx == 5
         && near(port2.w[3].val, std::pow(10.0, -3.5 / 20)));
  assert(ramp.size() == 2 && rampHas(ramp, MATRIX_BUS, 0) && rampHas(ramp, MATRIX_OUT, 5));
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

static void loadDefaultsResetsAndWritesTheDifference()
{
  FakePort port;
  const int budget = 4;
  MatrixModel m(port, 2, 8, budget);
  std::string fresh = m.savePreset();
  for (int b = 0; b < 2; b++)
  {
    char json[128];
    snprintf(json, sizeof(json), "{\"ch\":\"bus\",\"idx\":%d,\"slot\":{\"op\":\"add\",\"type\":\"peq\",\"gain\":-3}}", b);
    assert(patch(m, json).empty());
  }
  for (int o = 0; o < 8; o++)
  {
    char json[128];
    snprintf(json, sizeof(json), "{\"ch\":\"out\",\"idx\":%d,\"slot\":{\"op\":\"add\",\"type\":\"peq\",\"gain\":-3}}", o);
    assert(patch(m, json).empty());
  }
  patch(m, "{\"ch\":\"out\",\"idx\":0,\"mute\":false,\"name\":\"X\"}", 10);
  patch(m, "{\"ch\":\"out\",\"idx\":3,\"route\":2}", 10);
  patch(m, "{\"ch\":\"bus\",\"idx\":0,\"source\":65538,\"gain\":-3}", 10);
  port.w.clear();
  port.bq.clear();
  MatrixRampSet ramp;
  m.loadDefaults(&ramp);
  assertBaseline(m);

  DynamicJsonDocument st = stateOf(m);
  for (int b = 0; b < 2; b++)
  {
    assert(st["buses"][b]["used"] == 0);
    assert(st["buses"][b]["free"] == budget);
  }
  for (int o = 0; o < 8; o++)
  {
    assert(st["outs"][o]["used"] == 0);
    assert(st["outs"][o]["free"] == budget);
  }
  assert(m.savePreset() == fresh);
  // Bus 1 Source, Output 4 route, then the gains of ramp-marked Channels: Bus 1 back to 0 dB, Output 1 muted
  assert(port.w.size() == 4);
  assert(port.w[0].type == 's' && port.w[0].idx == 0 && port.w[0].val == 0);
  assert(port.w[1].type == 'r' && port.w[1].idx == 3 && port.w[1].val == 0);
  assert(port.w[2].type == 'g' && port.w[2].kind == MATRIX_BUS && port.w[2].idx == 0 && port.w[2].val == 1.0);
  assert(port.w[3].type == 'g' && port.w[3].kind == MATRIX_OUT && port.w[3].idx == 0 && port.w[3].val == 0.0);
  // each Channel's one PEQ biquad back to passthrough; every Channel had a filter difference
  assert(port.bq.size() == 10);
  for (size_t i = 0; i < port.bq.size(); i++)
    assert(port.bq[i].stage == 0 && isPassthrough(port.bq[i]));
  assert(ramp.size() == 10);
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

static void presetSwitchRejectsAnOverlappingEditAndKeepsPresetAssociation()
{
  FakePort port;
  MatrixModel m(port, 2, 8);

  // An accepted edit immediately before the switch still belongs to preset A.
  assert(patch(m, "{\"ch\":\"out\",\"idx\":0,\"mute\":false,\"gain\":-3}", 1000).empty());
  const std::string presetA = m.savePreset();

  FakePort presetPort;
  MatrixModel presetModel(presetPort, 2, 8);
  assert(patch(presetModel, "{\"ch\":\"out\",\"idx\":0,\"mute\":false,\"gain\":-12}").empty());
  const std::string presetB = presetModel.savePreset();

  port.w.clear();
  m.beginPresetSwitch();
  assert(patch(m, "{\"ch\":\"out\",\"idx\":0,\"gain\":6}") == "preset switch in progress");
  assert(port.w.empty());

  // Loading the selected Preset emits its difference, Output 1's gain. The refused
  // edit cannot append a stale write behind it.
  assert(m.loadPreset(presetB).empty());
  assert(stateOf(m)["outs"][0]["gain"] == -12);
  assert(port.w.size() == 1 && port.w[0].type == 'g' && near(port.w[0].val, std::pow(10.0, -12.0 / 20)));
  m.endPresetSwitch();

  // Edits resume against Preset B and are persisted with B, while the snapshot
  // saved for A retains the accepted pre-switch edit.
  port.w.clear();
  assert(patch(m, "{\"ch\":\"out\",\"idx\":0,\"gain\":-10}", 4000).empty());
  assert(port.w.size() == 1 && port.w[0].type == 'g' && near(port.w[0].val, std::pow(10.0, -10.0 / 20)));

  DynamicJsonDocument a(16384), b(16384);
  assert(!deserializeJson(a, presetA));
  assert(!deserializeJson(b, m.savePreset()));
  assert(a["outs"][0]["gain"] == -3);
  assert(b["outs"][0]["gain"] == -10);
}

static void undoRestoresTheLastAcceptedPatch()
{
  FakePort port;
  MatrixModel m(port, 2, 8);
  patch(m, "{\"ch\":\"out\",\"idx\":1,\"name\":\"Keep\"}", 1000);
  m.markSaved();
  const std::string before = m.savePreset();
  assert(patch(m, "{\"ch\":\"out\",\"idx\":1,\"mute\":false,\"route\":1,\"name\":\"X\"}", 5000).empty());
  assert(m.dirty());
  m.undo();
  assert(m.savePreset() == before);
  assert(!m.dirty());  // dirty flag restored too
  m.undo();            // second undo is a no-op
  assert(m.savePreset() == before);
}

//==============================================================================
// Filter row: PEQ Slots on the Channel's biquads

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
  const std::string active = a.savePreset();
  patch(a, "{\"ch\":\"out\",\"idx\":1,\"slot\":{\"op\":\"set\",\"i\":0,\"bypass\":true}}");
  std::string saved = a.savePreset();
  FakePort port2;
  MatrixModel b(port2, 2, 8, 4);
  assert(b.loadPreset(active).empty());
  assert(port2.bq.size() == 1 && port2.bq[0].kind == MATRIX_OUT && port2.bq[0].idx == 1
         && port2.bq[0].stage == 0 && !isPassthrough(port2.bq[0]));  // the active PEQ's biquad only
  port2.bq.clear();
  assert(b.loadPreset(saved).empty());
  assert(b.savePreset() == saved);
  assert(port2.bq.size() == 1 && port2.bq[0].stage == 0 && isPassthrough(port2.bq[0]));  // bypassed
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

static void multiBiquadEditsAreRamped()
{
  FakePort port;
  MatrixModel m(port, 2, 8, 5);
  assert(patch(m, "{\"ch\":\"bus\",\"idx\":0,\"gain\":-6}").empty());
  assert(addSlot(m, "\"type\":\"peq\",\"freq\":200", "bus", 0).empty());
  assert(addSlot(m, "\"type\":\"lp\",\"design\":\"lr\",\"order\":4,\"freq\":2000", "bus", 0).empty());

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

// Expects one ramp of the Channel: down, one wait, the given biquad stages, back up to `up`.
static void assertRampedStages(const FakePort& port, int idx, int first, int count, double up)
{
  assert(port.events.size() == (size_t)(count + 3));
  assert(port.events[0].type == 'g' && port.events[0].idx == idx && near(port.events[0].val, 0.0));
  assert(port.events[1].type == 'w' && port.events[1].val >= MATRIX_RAMP_MS);
  for (int k = 0; k < count; k++)
    assert(port.events[k + 2].type == 'b' && port.events[k + 2].idx == idx && port.events[k + 2].val == first + k);
  assert(port.events.back().type == 'g' && port.events.back().idx == idx && near(port.events.back().val, up));
}

// Spec 37: every filter Commit that changes a value, and every Slot bypass, ramps the
// Channel down and up around the biquad writes, single-biquad Slots included.
static void parameterEditsAndBypassAreRamped()
{
  FakePort port;
  MatrixModel m(port, 2, 8, 6);
  const double up = std::pow(10.0, -6.0 / 20);
  assert(patch(m, "{\"ch\":\"bus\",\"idx\":0,\"gain\":-6}").empty());
  assert(addSlot(m, "\"type\":\"peq\",\"freq\":200", "bus", 0).empty());
  assert(addSlot(m, "\"type\":\"lp\",\"design\":\"lr\",\"order\":8,\"freq\":2000", "bus", 0).empty());

  const char* peqEdits[] = { "\"freq\":300", "\"q\":2", "\"gain\":4" };
  for (size_t e = 0; e < sizeof(peqEdits) / sizeof(peqEdits[0]); e++)
  {
    port.events.clear();
    assert(setSlot(m, 0, peqEdits[e], "bus", 0).empty());
    assertRampedStages(port, 0, 0, 1, up);
  }

  port.events.clear();
  assert(setSlot(m, 1, "\"freq\":2500", "bus", 0).empty());
  assertRampedStages(port, 0, 1, 4, up);

  port.events.clear();
  assert(setSlot(m, 1, "\"bypass\":true", "bus", 0).empty());
  assertRampedStages(port, 0, 1, 4, up);

  port.events.clear();
  assert(setSlot(m, 1, "\"bypass\":false", "bus", 0).empty());
  assertRampedStages(port, 0, 1, 4, up);

  // a rejected edit emits nothing
  port.events.clear();
  assert(!setSlot(m, 0, "\"freq\":5", "bus", 0).empty());
  assert(port.events.empty());
}

// Spec 36: a Commit that leaves every value as it was writes and ramps nothing.
static void unchangedCommitEmitsNothing()
{
  FakePort port;
  MatrixModel m(port, 2, 8, 6);
  assert(patch(m, "{\"ch\":\"out\",\"idx\":1,\"mute\":false}").empty());
  assert(addSlot(m, "\"type\":\"peq\",\"freq\":200,\"q\":1,\"gain\":3").empty());
  assert(addSlot(m, "\"type\":\"hp\",\"design\":\"lr\",\"order\":4,\"freq\":80").empty());
  port.events.clear(); port.w.clear();
  assert(setSlot(m, 0, "\"freq\":200,\"q\":1,\"gain\":3,\"bypass\":false").empty());
  assert(setSlot(m, 1, "\"type\":\"hp\",\"design\":\"lr\",\"order\":4,\"freq\":80").empty());
  assert(port.events.empty() && port.w.empty());

  // a parameter edit of a bypassed Slot changes no biquad, so nothing is written either
  assert(setSlot(m, 0, "\"bypass\":true").empty());
  port.events.clear(); port.w.clear();
  assert(setSlot(m, 0, "\"freq\":400").empty());
  assert(port.events.empty() && port.w.empty());
}

// Muted Channels are silent already: their filter edits are written without a ramp.
static void mutedChannelFilterEditsSkipTheRamp()
{
  FakePort port;
  MatrixModel m(port, 2, 8, 4);
  assert(addSlot(m, "\"type\":\"peq\",\"freq\":200").empty());  // Outputs start muted
  port.events.clear();
  assert(setSlot(m, 0, "\"freq\":300").empty());
  assert(port.events.size() == 1 && port.events[0].type == 'b');
  port.events.clear();
  assert(setSlot(m, 0, "\"bypass\":true").empty());
  assert(port.events.size() == 1 && port.events[0].type == 'b');
}

// Linked members ramp together on a parameter edit: all down, one wait, all writes, all up.
static void linkedParameterEditRampsMembersTogether()
{
  FakePort port;
  MatrixModel m(port, 2, 8, 4);
  for (int o = 2; o < 4; o++)
    assert(patch(m, ("{\"ch\":\"out\",\"idx\":" + std::to_string(o) + ",\"mute\":false,\"link\":\"B\"}").c_str()).empty());
  assert(patch(m, "{\"linkGroup\":\"B\",\"filters\":true}").empty());
  assert(addSlot(m, "\"type\":\"peq\",\"freq\":200", "out", 2).empty());
  port.events.clear();
  assert(setSlot(m, 0, "\"gain\":-3", "out", 3).empty());
  assert(port.events.size() == 2 + 1 + 2 + 2);
  assert(port.events[0].type == 'g' && port.events[0].val == 0.0 && port.events[0].idx == 2);
  assert(port.events[1].type == 'g' && port.events[1].val == 0.0 && port.events[1].idx == 3);
  assert(port.events[2].type == 'w');
  assert(port.events[3].type == 'b' && port.events[3].idx == 2);
  assert(port.events[4].type == 'b' && port.events[4].idx == 3);
  assert(port.events[5].type == 'g' && near(port.events[5].val, 1.0) && port.events[5].idx == 2);
  assert(port.events[6].type == 'g' && near(port.events[6].val, 1.0) && port.events[6].idx == 3);
}

static std::vector<Write> only(const FakePort& port, char type);

static int countEvents(const FakePort& port, char type)
{
  int n = 0;
  for (size_t k = 0; k < port.events.size(); k++)
    if (port.events[k].type == type)
      n++;
  return n;
}

// A patch ramps each Channel at most once: down, one wait, every write, up.
static void routeSwitchAndLayoutChangeRampOnce()
{
  FakePort port;
  MatrixModel m(port, 2, 8, 4);
  assert(patch(m, "{\"ch\":\"out\",\"idx\":1,\"mute\":false,\"gain\":-6}").empty());
  port.events.clear(); port.w.clear();
  assert(patch(m, "{\"ch\":\"out\",\"idx\":1,\"route\":2,\"slot\":{\"op\":\"add\",\"type\":\"lp\",\"design\":\"lr\",\"order\":4}}").empty());
  assert(countEvents(port, 'w') == 1);
  assert(countEvents(port, 'g') == 2);
  assert(port.events.front().type == 'g' && port.events.front().val == 0.0);
  assert(port.events[1].type == 'w');
  assert(port.events.back().type == 'g' && near(port.events.back().val, std::pow(10.0, -6.0 / 20)));
  assert(countEvents(port, 'b') == 2 && only(port, 'r').size() == 1);
}

// Linked members ramp together: all down, one wait, all writes, all up.
static void linkedLayoutChangeRampsMembersTogether()
{
  FakePort port;
  MatrixModel m(port, 2, 8, 4);
  for (int o = 2; o < 4; o++)
    assert(patch(m, ("{\"ch\":\"out\",\"idx\":" + std::to_string(o) + ",\"mute\":false,\"link\":\"B\"}").c_str()).empty());
  assert(patch(m, "{\"linkGroup\":\"B\",\"filters\":true}").empty());
  port.events.clear();
  assert(addSlot(m, "\"type\":\"hp\",\"design\":\"lr\",\"order\":4", "out", 2).empty());
  assert(port.events.size() == 2 + 1 + 4 + 2);
  assert(port.events[0].type == 'g' && port.events[0].val == 0.0 && port.events[0].idx == 2);
  assert(port.events[1].type == 'g' && port.events[1].val == 0.0 && port.events[1].idx == 3);
  assert(port.events[2].type == 'w');
  for (int k = 3; k < 7; k++)
    assert(port.events[k].type == 'b');
  assert(port.events[7].type == 'g' && near(port.events[7].val, 1.0) && port.events[7].idx == 2);
  assert(port.events[8].type == 'g' && near(port.events[8].val, 1.0) && port.events[8].idx == 3);
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

static void slotRulesAreTheSameForEditsAndPresets()
{
  const char* invalid[] = {
    "\"type\":\"bogus\"",
    "\"type\":\"hp\",\"design\":\"bessel\"",
    "\"type\":\"hp\",\"design\":\"lr\",\"order\":6",
    "\"freq\":19",
    "\"q\":20.1",
    "\"gain\":24.1",
    "\"type\":\"lshelf\",\"slope\":0.09",
  };
  for (size_t k = 0; k < sizeof(invalid) / sizeof(invalid[0]); k++)
  {
    FakePort editPort;
    MatrixModel edited(editPort, 2, 8, 8);
    assert(!addSlot(edited, invalid[k]).empty());

    FakePort sourcePort;
    MatrixModel source(sourcePort, 2, 8, 8);
    assert(addSlot(source, "\"type\":\"peq\"").empty());
    DynamicJsonDocument doc(16384);
    assert(!deserializeJson(doc, source.savePreset()));
    JsonObject slot = doc["outs"][1]["slots"][0];
    if (k == 0) slot["type"] = "bogus";
    if (k == 1) { slot["type"] = "hp"; slot["design"] = "bessel"; }
    if (k == 2) { slot["type"] = "hp"; slot["design"] = "lr"; slot["order"] = 6; }
    if (k == 3) slot["freq"] = 19;
    if (k == 4) slot["q"] = 20.1;
    if (k == 5) slot["gain"] = 24.1;
    if (k == 6) { slot["type"] = "lshelf"; slot["slope"] = 0.09; }
    std::string preset;
    serializeJson(doc, preset);
    FakePort loadPort;
    MatrixModel loaded(loadPort, 2, 8, 8);
    assert(loaded.loadPreset(preset) == "bad slot");
  }

  // Keep the live editor's established first-error behavior when more than one
  // final invariant is invalid in the same patch.
  FakePort invalidPort;
  MatrixModel invalidModel(invalidPort, 2, 8, 8);
  assert(addSlot(invalidModel, "\"type\":\"hp\",\"design\":\"lr\",\"order\":6,\"freq\":19") == "freq out of range");
  assert(addSlot(invalidModel, "\"type\":\"peq\"").empty());
  assert(setSlot(invalidModel, 0, "\"freq\":5,\"bypass\":\"x\"") == "freq out of range");
  assert(setSlot(invalidModel, 0, "\"freq\":5,\"type\":\"bogus\"") == "freq out of range");
  assert(setSlot(invalidModel, 0, "\"q\":50,\"design\":\"x\"") == "q out of range");
  assert(setSlot(invalidModel, 0, "\"freq\":5,\"q\":\"x\"") == "freq out of range");

  // Non-crossover design/order are accepted for compatibility, then normalized
  // through both paths so a later type change cannot revive hidden settings.
  FakePort editPort;
  MatrixModel edited(editPort, 2, 8, 8);
  assert(addSlot(edited, "\"type\":\"phase\",\"design\":\"lr\",\"order\":8").empty());
  DynamicJsonDocument doc(16384);
  assert(!deserializeJson(doc, edited.savePreset()));
  doc["outs"][1]["slots"][0]["design"] = "lr";
  doc["outs"][1]["slots"][0]["order"] = 8;
  std::string preset;
  serializeJson(doc, preset);
  FakePort loadPort;
  MatrixModel loaded(loadPort, 2, 8, 8);
  assert(loaded.loadPreset(preset).empty());
  DynamicJsonDocument st = stateOf(loaded);
  assert(!st["outs"][1]["slots"][0].containsKey("design"));
  assert(!st["outs"][1]["slots"][0].containsKey("order"));
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
  // only the differing block: the bypassed delay is 0 samples on the DSP as before
  assert(only(port2, 'p').size() == 1 && only(port2, 'd').empty());
  assert(only(port2, 'p')[0].idx == 4 && only(port2, 'p')[0].val == 1.0);
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
  assert(patch(m, "{\"linkGroup\":\"A\",\"mute\":true,\"filters\":true,\"gain\":true,\"delay\":true}").empty());

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

// Spec 45: a joiner takes over the group's linked settings, so a patch that joins and
// also sets a linked aspect is ambiguous and rejected; unlinked aspects are fine.
static void joiningRejectsLinkedSettingsInTheSamePatch()
{
  FakePort port;
  MatrixModel m(port, 2, 8, 4);
  assert(patch(m, "{\"ch\":\"out\",\"idx\":0,\"mute\":false,\"gain\":-3,\"link\":\"A\"}").empty());
  assert(patch(m, "{\"linkGroup\":\"A\",\"mute\":true,\"gain\":true}").empty());
  const std::string before = m.savePreset();
  port.w.clear();
  std::string err = patch(m, "{\"ch\":\"out\",\"idx\":1,\"gain\":4,\"link\":\"A\"}");
  assert(err == "a Channel joining a Link group takes over its linked gain");
  err = patch(m, "{\"ch\":\"out\",\"idx\":1,\"link\":\"A\",\"mute\":true}");
  assert(err == "a Channel joining a Link group takes over its linked mute");
  assert(port.w.empty() && m.savePreset() == before);

  // delay is not linked in A: allowed, and the gain is taken over
  assert(patch(m, "{\"ch\":\"out\",\"idx\":1,\"delayUs\":1000,\"link\":\"A\"}").empty());
  DynamicJsonDocument st = stateOf(m);
  assert(st["outs"][1]["gain"] == -3 && st["outs"][1]["delaySamples"] == 48);
  // already a member: editing a linked aspect together with the same link is an ordinary edit
  assert(patch(m, "{\"ch\":\"out\",\"idx\":1,\"gain\":2,\"link\":\"A\"}").empty());
  assert(stateOf(m)["outs"][0]["gain"] == 2);
  // first member of an empty group: nothing to take over
  assert(patch(m, "{\"linkGroup\":\"C\",\"gain\":true}").empty());
  assert(patch(m, "{\"ch\":\"out\",\"idx\":5,\"gain\":-1,\"link\":\"C\"}").empty());
}

static void eachLinkAspectCanBeEnabledIndependently()
{
  FakePort port;
  MatrixModel m(port, 2, 8, 4);
  patch(m, "{\"ch\":\"out\",\"idx\":2,\"link\":\"B\"}");
  patch(m, "{\"ch\":\"out\",\"idx\":3,\"link\":\"B\"}");
  patch(m, "{\"linkGroup\":\"B\",\"gain\":true}");
  assert(patch(m, "{\"ch\":\"out\",\"idx\":2,\"gain\":4,\"delayUs\":1000,\"mute\":false}").empty());
  DynamicJsonDocument st = stateOf(m);
  assert(st["outs"][3]["gain"] == 4);
  assert(st["outs"][3]["delaySamples"] == 0 && st["outs"][3]["mute"] == true);
  assert(!patch(m, "{\"linkGroup\":\"bus\",\"delay\":true}").empty());
}

static void enablingAnAspectMakesExistingMembersConsistent()
{
  FakePort port;
  MatrixModel m(port, 2, 8, 4);
  patch(m, "{\"ch\":\"out\",\"idx\":4,\"mute\":false,\"gain\":2,\"link\":\"D\"}");
  patch(m, "{\"ch\":\"out\",\"idx\":5,\"mute\":false,\"gain\":-12,\"link\":\"D\"}");
  port.w.clear();
  assert(patch(m, "{\"linkGroup\":\"D\",\"gain\":true}").empty());
  DynamicJsonDocument st = stateOf(m);
  assert(st["outs"][4]["gain"] == 2 && st["outs"][5]["gain"] == 2);
  assert(only(port, 'g').size() == 1 && only(port, 'g')[0].idx == 5);

  patch(m, "{\"ch\":\"bus\",\"idx\":0,\"link\":\"bus\"}");
  patch(m, "{\"ch\":\"bus\",\"idx\":1,\"link\":\"bus\"}");
  patch(m, "{\"linkGroup\":\"bus\",\"mute\":true}");
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
  patch(a, "{\"linkGroup\":\"C\",\"mute\":true,\"filters\":true,\"gain\":true,\"delay\":false}");
  patch(a, "{\"ch\":\"bus\",\"idx\":0,\"link\":\"bus\"}");
  patch(a, "{\"linkGroup\":\"bus\",\"filters\":true}");
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

// A Link group edit is addressed by the Link group's id; the Channel field never names one.
static void linkGroupEditsAreAddressedAsLinkGroups()
{
  FakePort port;
  MatrixModel m(port, 2, 8, 4);
  const std::string before = m.savePreset();
  assert(patch(m, "{\"ch\":\"link\",\"scope\":\"out\",\"group\":\"A\",\"mute\":true}") == "unknown channel kind");
  assert(patch(m, "{\"linkGroup\":\"E\",\"mute\":true}") == "no such Link group");
  assert(patch(m, "{\"linkGroup\":\"\",\"mute\":true}") == "no such Link group");
  assert(patch(m, "{\"linkGroup\":3,\"mute\":true}") == "no such Link group");
  assert(patch(m, "{\"linkGroup\":\"A\",\"ch\":\"out\",\"mute\":true}") == "unknown field ch");
  assert(patch(m, "{\"linkGroup\":\"A\"}") == "missing linked aspect");
  assert(patch(m, "{\"linkGroup\":\"A\",\"mute\":1}") == "mute must be true or false");
  assert(patch(m, "{\"linkGroup\":\"bus\",\"delay\":true}") == "Buses have no delay link");
  assert(patch(m, "{\"ch\":\"out\",\"idx\":1,\"link\":\"bus\"}") == "Link group outside the Channel's scope");
  assert(m.savePreset() == before && port.w.empty());

  MatrixEditResult r = edit(m, "{\"linkGroup\":\"bus\",\"gain\":true,\"preset\":0}");
  assert(r.ok());
  assert(stateOf(m)["links"]["bus"]["gain"] == true);
  r = edit(m, "{\"linkGroup\":\"B\",\"gain\":true,\"preset\":1}");
  assert(r.status == MATRIX_EDIT_CONFLICT);
  assert(stateOf(m)["links"]["outs"][1]["gain"] == false);
}

// Presets saved before the Link group request was renamed (format v3) keep their
// Link groups, memberships and linked aspects. Fixture written by commit 7930f62.
static void v3PresetKeepsLinkGroupsAndLinkedAspects()
{
  const char* v3 = R"({"v":3,"spdif":0,"links":{"bus":{"mute":false,"filters":true,"gain":false},"outs":[{"mute":true,"filters":false,"gain":true,"delay":false},{"mute":false,"filters":false,"gain":false,"delay":false},{"mute":false,"filters":false,"gain":false,"delay":false},{"mute":false,"filters":false,"gain":false,"delay":true}]},"buses":[{"name":"Bus 1","source":0,"mute":false,"gain":0,"gainBypass":false,"link":"bus","slots":[]},{"name":"Bus 2","source":1,"mute":false,"gain":0,"gainBypass":false,"link":"bus","slots":[]}],"outs":[{"name":"Output 1","route":0,"mute":false,"gain":-3,"gainBypass":false,"link":"A","polarity":false,"delaySamples":0,"delayBypass":false,"slots":[]},{"name":"Output 2","route":1,"mute":false,"gain":-3,"gainBypass":false,"link":"A","polarity":false,"delaySamples":0,"delayBypass":false,"slots":[]},{"name":"Output 3","route":0,"mute":true,"gain":0,"gainBypass":false,"link":"","polarity":false,"delaySamples":0,"delayBypass":false,"slots":[]},{"name":"Output 4","route":1,"mute":true,"gain":0,"gainBypass":false,"link":"","polarity":false,"delaySamples":0,"delayBypass":false,"slots":[]},{"name":"Output 5","route":0,"mute":true,"gain":0,"gainBypass":false,"link":"D","polarity":false,"delaySamples":0,"delayBypass":false,"slots":[]},{"name":"Output 6","route":1,"mute":true,"gain":0,"gainBypass":false,"link":"","polarity":false,"delaySamples":0,"delayBypass":false,"slots":[]},{"name":"Output 7","route":0,"mute":true,"gain":0,"gainBypass":false,"link":"","polarity":false,"delaySamples":0,"delayBypass":false,"slots":[]},{"name":"Output 8","route":1,"mute":true,"gain":0,"gainBypass":false,"link":"","polarity":false,"delaySamples":0,"delayBypass":false,"slots":[]}]})";
  FakePort port;
  MatrixModel m(port, 2, 8, 4);
  assert(m.loadPreset(v3).empty());
  assert(m.savePreset() == v3);
  DynamicJsonDocument st = stateOf(m);
  assert(st["links"]["bus"]["filters"] == true && st["links"]["bus"]["mute"] == false);
  assert(st["links"]["outs"][0]["mute"] == true && st["links"]["outs"][0]["gain"] == true);
  assert(st["links"]["outs"][0]["filters"] == false && st["links"]["outs"][0]["delay"] == false);
  assert(st["links"]["outs"][3]["delay"] == true);
  assert(std::string(st["buses"][1]["link"].as<const char*>()) == "bus");
  assert(std::string(st["outs"][1]["link"].as<const char*>()) == "A");
  assert(std::string(st["outs"][4]["link"].as<const char*>()) == "D");
  assert(std::string(st["outs"][2]["link"].as<const char*>()) == "");

  // Linked gain still moves Link group A together; unlinked delay does not.
  assert(patch(m, "{\"ch\":\"out\",\"idx\":1,\"gain\":-6,\"delayUs\":1000}").empty());
  st = stateOf(m);
  assert(st["outs"][0]["gain"] == -6 && st["outs"][0]["delaySamples"] == 0);
}

static void editResultsDistinguishSuccessBadRequestAndConflict()
{
  FakePort port;
  MatrixModel m(port, 2, 8);
  m.setPreset(2);

  MatrixEditResult r = edit(m, "{\"ch\":\"out\",\"idx\":0,\"gain\":-3}");
  assert(r.status == MATRIX_EDIT_OK && r.reason.empty());
  r = edit(m, "{\"ch\":\"out\",\"idx\":0,\"gain\":-3,\"preset\":2}");
  assert(r.status == MATRIX_EDIT_OK);

  r = edit(m, "{\"ch\":\"out\",\"idx\":0,\"gain\":99}");
  assert(r.status == MATRIX_EDIT_BAD_REQUEST && !r.reason.empty());

  // An edit made against another Preset is a conflict, whatever else it carries.
  port.w.clear();
  r = edit(m, "{\"ch\":\"out\",\"idx\":0,\"gain\":-6,\"preset\":1}");
  assert(r.status == MATRIX_EDIT_CONFLICT && r.reason == "preset changed; refresh the matrix");
  r = edit(m, "{\"ch\":\"out\",\"idx\":0,\"gain\":99,\"preset\":\"2\"}");
  assert(r.status == MATRIX_EDIT_CONFLICT && r.reason == "preset changed; refresh the matrix");
  assert(port.w.empty());

  // During a Preset switch every edit is a conflict, and a second switch cannot start.
  assert(m.beginPresetSwitch());
  assert(!m.beginPresetSwitch());
  r = edit(m, "{\"ch\":\"out\",\"idx\":0,\"gain\":99}");
  assert(r.status == MATRIX_EDIT_CONFLICT && r.reason == "preset switch in progress");
  assert(port.w.empty());
  m.endPresetSwitch();
  assert(edit(m, "{\"ch\":\"out\",\"idx\":0,\"gain\":-6}").status == MATRIX_EDIT_OK);
  assert(m.beginPresetSwitch());
}

// The /matrix reply: a full worker queue is "try again" (503, as for /preset), not a bad patch.
static void editStatusesMapToDistinctHttpReplies()
{
  assert(matrixEditHttpStatus(MATRIX_EDIT_OK) == 200);
  assert(matrixEditHttpStatus(MATRIX_EDIT_BAD_REQUEST) == 400);
  assert(matrixEditHttpStatus(MATRIX_EDIT_CONFLICT) == 409);
  assert(matrixEditHttpStatus(MATRIX_EDIT_BUSY) == 503);
}

static void stateCarriesTheActivePreset()
{
  FakePort port;
  MatrixModel m(port, 2, 8);
  assert(stateOf(m)["preset"] == 0);
  m.setPreset(3);
  assert(stateOf(m)["preset"] == 3);
  assert(stateOf(m)["nout"] == 8);
}

// Ticket 02: a Preset load writes only the blocks that differ from the current state.

static void loadingTheCurrentStateEmitsNothing()
{
  FakePort port;
  MatrixModel m(port, 2, 8, 4);
  assert(patch(m, "{\"ch\":\"out\",\"idx\":2,\"mute\":false,\"route\":2,\"slot\":{\"op\":\"add\",\"type\":\"peq\",\"freq\":500,\"gain\":-3}}").empty());
  const std::string same = m.savePreset();
  port.w.clear(); port.bq.clear(); port.events.clear();
  MatrixRampSet ramp;
  ramp.push_back({ MATRIX_BUS, 1, 0.0 });  // stale content is replaced
  assert(m.loadPreset(same, &ramp).empty());
  assert(port.w.empty() && port.bq.empty());
  assert(ramp.empty());
  m.loadDefaults();  // and the Baseline from the Baseline
  port.w.clear(); port.bq.clear();
  m.loadDefaults(&ramp);
  assert(port.w.empty() && port.bq.empty() && ramp.empty());
}

// A Preset made from the model's current state plus one patch; the model itself is unchanged.
static std::string presetWith(const MatrixModel& m, int nbiquad, const char* json)
{
  FakePort scratch;
  MatrixModel other(scratch, 2, 8, nbiquad);
  assert(other.loadPreset(m.savePreset()).empty());
  assert(patch(other, json).empty());
  return other.savePreset();
}

static void onePeqFrequencyChangeWritesOnlyThatBiquad()
{
  FakePort port;
  MatrixModel m(port, 2, 8, 4);
  assert(patch(m, "{\"ch\":\"out\",\"idx\":1,\"mute\":false}").empty());
  for (int k = 0; k < 3; k++)
    assert(patch(m, "{\"ch\":\"out\",\"idx\":1,\"slot\":{\"op\":\"add\",\"freq\":1000,\"q\":1,\"gain\":6}}").empty());
  const std::string next = presetWith(m, 4, "{\"ch\":\"out\",\"idx\":1,\"slot\":{\"op\":\"set\",\"i\":1,\"freq\":2000}}");
  port.w.clear(); port.bq.clear();
  MatrixRampSet ramp;
  assert(m.loadPreset(next, &ramp).empty());
  assert(port.w.empty());
  assert(port.bq.size() == 1 && port.bq[0].kind == MATRIX_OUT && port.bq[0].idx == 1 && port.bq[0].stage == 1);
  assert(ramp.size() == 1 && ramp[0].kind == MATRIX_OUT && ramp[0].idx == 1 && near(ramp[0].linear, 1.0));
}

static void slotLayoutChangeWritesTheShiftedBiquadsAndRamps()
{
  FakePort port;
  MatrixModel m(port, 2, 8, 6);
  const char* adds[] = {
    "{\"ch\":\"bus\",\"idx\":0,\"slot\":{\"op\":\"add\",\"freq\":100,\"gain\":3}}",
    "{\"ch\":\"bus\",\"idx\":0,\"slot\":{\"op\":\"add\",\"type\":\"hp\",\"order\":4,\"freq\":80}}",
    "{\"ch\":\"bus\",\"idx\":0,\"slot\":{\"op\":\"add\",\"freq\":3000,\"gain\":-2}}" };
  for (int k = 0; k < 3; k++) assert(patch(m, adds[k]).empty());
  // stages: PEQ 0 | HP4 1,2 | PEQ 3. Deleting the first PEQ shifts the rest down by one.
  const std::string next = presetWith(m, 6, "{\"ch\":\"bus\",\"idx\":0,\"slot\":{\"op\":\"del\",\"i\":0}}");
  port.w.clear(); port.bq.clear();
  MatrixRampSet ramp;
  assert(m.loadPreset(next, &ramp).empty());
  assert(port.w.empty());  // gain unchanged: no write
  assert(port.bq.size() == 4);
  for (int s = 0; s < 4; s++)
    assert(port.bq[s].kind == MATRIX_BUS && port.bq[s].idx == 0 && port.bq[s].stage == s);
  assert(isPassthrough(port.bq[3]) && !isPassthrough(port.bq[2]));
  assert(ramp.size() == 1 && rampHas(ramp, MATRIX_BUS, 0));
}

static void onlySourceRouteAndFilterDifferencesMarkTheRamp()
{
  FakePort port;
  MatrixModel m(port, 2, 8, 2);
  assert(patch(m, "{\"ch\":\"out\",\"idx\":0,\"mute\":false}").empty());

  // mute, gain, delay, polarity and SPDIF out alone: written, no ramp
  FakePort sp;
  MatrixModel quiet(sp, 2, 8, 2);
  assert(quiet.loadPreset(m.savePreset()).empty());
  assert(patch(quiet, "{\"ch\":\"out\",\"idx\":0,\"gain\":-6}").empty());
  assert(patch(quiet, "{\"ch\":\"out\",\"idx\":2,\"mute\":false,\"polarity\":true,\"delayUs\":1000}").empty());
  assert(patch(quiet, "{\"ch\":\"bus\",\"idx\":1,\"mute\":true}").empty());
  assert(patch(quiet, "{\"ch\":\"spdif\",\"sel\":65540}").empty());  // USB 5+6
  port.w.clear(); port.bq.clear();
  MatrixRampSet ramp;
  ramp.push_back({ MATRIX_OUT, 7, 1.0 });  // replaced by the (empty) set
  assert(m.loadPreset(quiet.savePreset(), &ramp).empty());
  assert(ramp.empty());
  assert(port.bq.empty());
  int g = 0, p = 0, d = 0, o = 0;
  for (size_t i = 0; i < port.w.size(); i++)
  {
    g += port.w[i].type == 'g';
    p += port.w[i].type == 'p';
    d += port.w[i].type == 'd';
    o += port.w[i].type == 'o';
  }
  assert(g == 3 && p == 1 && d == 1 && o == 1 && port.w.size() == 6);
  const Write& spdif = port.w.back();
  assert(spdif.type == 'o' && spdif.val == 65540 && spdif.idx == 65541);

  // Source, route and filter differences each mark their Channel
  FakePort sp2;
  MatrixModel loud(sp2, 2, 8, 2);
  assert(loud.loadPreset(m.savePreset()).empty());
  assert(patch(loud, "{\"ch\":\"bus\",\"idx\":1,\"source\":131075}").empty());
  assert(patch(loud, "{\"ch\":\"out\",\"idx\":4,\"route\":1}").empty());
  assert(patch(loud, "{\"ch\":\"out\",\"idx\":6,\"slot\":{\"op\":\"add\",\"gain\":4}}").empty());
  assert(m.loadPreset(loud.savePreset(), &ramp).empty());
  assert(ramp.size() == 3 && rampHas(ramp, MATRIX_BUS, 1) && rampHas(ramp, MATRIX_OUT, 4) && rampHas(ramp, MATRIX_OUT, 6));
}

static void failedLoadLeavesModelAndWritesUntouched()
{
  FakePort port;
  MatrixModel m(port, 2, 8, 2);
  assert(patch(m, "{\"ch\":\"out\",\"idx\":3,\"mute\":false,\"route\":2,\"slot\":{\"op\":\"add\",\"gain\":4}}", 100).empty());
  const std::string before = m.savePreset();
  // valid up to a bad Slot in the last Output; everything before it would differ
  FakePort sp;
  MatrixModel other(sp, 2, 8, 2);
  assert(patch(other, "{\"ch\":\"bus\",\"idx\":0,\"source\":131075}").empty());
  DynamicJsonDocument doc(16384);
  assert(!deserializeJson(doc, other.savePreset()));
  doc["outs"][7]["slots"].add("garbage");
  std::string bad;
  serializeJson(doc, bad);
  port.w.clear(); port.bq.clear();
  MatrixRampSet ramp;
  ramp.push_back({ MATRIX_OUT, 7, 0.5 });
  assert(!m.loadPreset(bad, &ramp).empty());
  assert(!m.loadPreset("not json", &ramp).empty());
  assert(m.savePreset() == before && m.dirty());
  assert(port.w.empty() && port.bq.empty());
  assert(ramp.size() == 1 && ramp[0].idx == 7 && ramp[0].linear == 0.5);
}

// Ticket 03: a Preset switch takes the shared ramp around the difference.

// Records every write in order as text: "g out 1 0", "w 20", "b out 1 0", "r 4 1", "s 1 131075" ...
struct OrderPort : MatrixPort
{
  std::vector<std::string> log;
  void add(const char* fmt, ...) __attribute__((format(printf, 2, 3)))
  {
    char buf[64];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    log.push_back(buf);
  }
  static const char* k(MatrixChannelKind kind) { return kind == MATRIX_BUS ? "bus" : "out"; }
  void writeGain(MatrixChannelKind kind, int idx, double linear) override { add("g %s %d %.4f", k(kind), idx, linear); }
  void writeRoute(int out, int route) override { add("r %d %d", out, route); }
  void writeSource(int bus, uint32_t sel) override { add("s %d %u", bus, (unsigned)sel); }
  void writePolarity(int out, bool invert) override { add("p %d %d", out, invert ? 1 : 0); }
  void writeDelay(int out, int samples) override { add("d %d %d", out, samples); }
  void waitMs(int ms) override { add("w %d", ms); }
  void writeSpdifOut(uint32_t left, uint32_t right) override { add("o %u %u", (unsigned)left, (unsigned)right); }
  void writeBiquad(MatrixChannelKind kind, int idx, int stage, const float*, const float*) override { add("b %s %d %d", k(kind), idx, stage); }
  void writeAddOn(const MatrixAddOn& cfg) override { add("a %02x %02x %02x", cfg.addr, cfg.reg, cfg.data); }
};

static std::vector<std::string> lines(std::initializer_list<const char*> l)
{
  return std::vector<std::string>(l.begin(), l.end());
}

static void presetSwitchRampsDownWritesTheDifferenceAndRampsUp()
{
  OrderPort port;
  MatrixModel m(port, 2, 8, 2);
  assert(patch(m, "{\"ch\":\"out\",\"idx\":1,\"mute\":false}").empty());
  assert(patch(m, "{\"ch\":\"out\",\"idx\":4,\"mute\":false}").empty());
  // Output 2: one more PEQ and -6 dB; Output 5: route Bus 2; Output 3: polarity only (no ramp)
  std::string next = presetWith(m, 2, "{\"ch\":\"out\",\"idx\":1,\"gain\":-6,\"slot\":{\"op\":\"add\",\"freq\":1000,\"gain\":6}}");
  {
    FakePort scratch;
    MatrixModel other(scratch, 2, 8, 2);
    assert(other.loadPreset(next).empty());
    assert(patch(other, "{\"ch\":\"out\",\"idx\":4,\"route\":1}").empty());
    assert(patch(other, "{\"ch\":\"out\",\"idx\":2,\"polarity\":true}").empty());
    next = other.savePreset();
  }
  port.log.clear();
  assert(m.switchPreset(next).empty());
  char wait[16];
  snprintf(wait, sizeof(wait), "w %d", MATRIX_RAMP_MS);
  const std::vector<std::string> want = lines({
    "g out 1 0.0000", "g out 4 0.0000", wait,  // ramp down every marked Channel once
    "b out 1 0", "p 2 1", "r 4 1",             // the difference, in Channel order
    "g out 1 0.5012", "g out 4 1.0000" });     // ramp up to the loaded gains
  if (port.log != want)
    for (size_t i = 0; i < port.log.size(); i++) fprintf(stderr, "%s\n", port.log[i].c_str());
  assert(port.log == want);
}

// Mute, gain, delay, polarity and SPDIF differences alone: written without ramp or wait.
static void presetSwitchWithoutMarkedChannelsEmitsNoRamp()
{
  OrderPort port;
  MatrixModel m(port, 2, 8, 2);
  assert(patch(m, "{\"ch\":\"out\",\"idx\":0,\"mute\":false}").empty());
  const std::string next = presetWith(m, 2, "{\"ch\":\"out\",\"idx\":0,\"gain\":-6,\"polarity\":true,\"delayUs\":1000}");
  port.log.clear();
  assert(m.switchPreset(next).empty());
  assert(port.log == lines({ "g out 0 0.5012", "p 0 1", "d 0 48" }));
  port.log.clear();
  assert(m.switchPreset(next).empty());  // the same Preset again: nothing at all
  assert(port.log.empty());
}

// A marked Channel that was silent needs no ramp down; one silent after the load stays at 0.
static void presetSwitchRampsOnlyAudibleChannels()
{
  OrderPort port;
  MatrixModel m(port, 2, 8, 2);
  assert(patch(m, "{\"ch\":\"out\",\"idx\":3,\"mute\":false}").empty());
  // Output 1 (muted before) unmutes on Bus 2; Output 4 (audible before) is muted on Bus 2;
  // Output 6 stays muted on Bus 2
  std::string next;
  {
    FakePort scratch;
    MatrixModel other(scratch, 2, 8, 2);
    assert(other.loadPreset(m.savePreset()).empty());
    assert(patch(other, "{\"ch\":\"out\",\"idx\":0,\"mute\":false,\"route\":1}").empty());
    assert(patch(other, "{\"ch\":\"out\",\"idx\":3,\"mute\":true,\"route\":1}").empty());
    assert(patch(other, "{\"ch\":\"out\",\"idx\":5,\"route\":1}").empty());
    next = other.savePreset();
  }
  port.log.clear();
  assert(m.switchPreset(next).empty());
  char wait[16];
  snprintf(wait, sizeof(wait), "w %d", MATRIX_RAMP_MS);
  assert(port.log == lines({ "g out 3 0.0000", wait, "r 0 1", "r 3 1", "r 5 1", "g out 0 1.0000" }));

  // only silent Channels marked: no ramp down, no wait
  port.log.clear();
  assert(m.switchPreset(presetWith(m, 2, "{\"ch\":\"out\",\"idx\":5,\"route\":0}")).empty());
  assert(port.log == lines({ "r 5 0" }));
}

static void failedPresetSwitchWritesNothing()
{
  OrderPort port;
  MatrixModel m(port, 2, 8, 2);
  assert(patch(m, "{\"ch\":\"out\",\"idx\":3,\"mute\":false,\"route\":2}", 100).empty());
  const std::string before = m.savePreset();
  port.log.clear();
  assert(!m.switchPreset("not json").empty());
  assert(!m.switchPreset("{\"v\":3,\"buses\":[],\"outs\":[]}").empty());
  assert(port.log.empty() && m.savePreset() == before && m.dirty());
}

// A Preset without a (valid) file switches to the Baseline through the same ramp.
static void switchToDefaultsTakesTheSharedRamp()
{
  OrderPort port;
  MatrixModel m(port, 2, 8, 2);
  assert(patch(m, "{\"ch\":\"bus\",\"idx\":0,\"source\":131075}").empty());
  assert(patch(m, "{\"ch\":\"out\",\"idx\":1,\"mute\":false,\"gain\":-6}").empty());
  port.log.clear();
  m.switchToDefaults();
  char wait[16];
  snprintf(wait, sizeof(wait), "w %d", MATRIX_RAMP_MS);
  assert(port.log == lines({ "g bus 0 0.0000", wait, "s 0 0", "g out 1 0.0000", "g bus 0 1.0000" }));
  assert(!m.dirty());
  port.log.clear();
  m.switchToDefaults();
  assert(port.log.empty());
}

// Import (Export's inverse): a Preset file replaces the active Preset through the
// Preset-switch path (difference inside the shared ramp) and is then Autosaved.
static void importReplacesTheActivePresetThroughTheSharedRampAndIsAutosaved()
{
  OrderPort port;
  MatrixModel m(port, 2, 8, 2);
  m.setPreset(1);
  assert(patch(m, "{\"ch\":\"out\",\"idx\":1,\"mute\":false}", 100).empty());
  m.markSaved();
  const std::string file = presetWith(m, 2, "{\"ch\":\"out\",\"idx\":1,\"gain\":-6,\"slot\":{\"op\":\"add\",\"freq\":1000,\"gain\":6}}");
  port.log.clear();
  const MatrixEditResult r = m.importPreset(file, 1, 5000);
  assert(r.ok() && r.reason.empty());
  char wait[16];
  snprintf(wait, sizeof(wait), "w %d", MATRIX_RAMP_MS);
  assert(port.log == lines({ "g out 1 0.0000", wait, "b out 1 0", "g out 1 0.5012" }));
  assert(m.savePreset() == file);
  assert(m.preset() == 1);
  // marked dirty: Autosave stores it once it has settled
  assert(m.dirty());
  assert(!m.saveDue(6999));
  assert(m.saveDue(7000));
}

static void invalidImportIsRejectedWithoutChangingState()
{
  OrderPort port;
  MatrixModel m(port, 2, 8, 2);
  m.setPreset(0);
  assert(patch(m, "{\"ch\":\"out\",\"idx\":3,\"mute\":false,\"route\":2}", 100).empty());
  m.markSaved();
  const std::string before = m.savePreset();
  port.log.clear();
  const char* bad[] = { "not json", "{\"v\":1,\"buses\":[],\"outs\":[]}", "{\"v\":99}", "" };
  for (const char* json : bad)
  {
    const MatrixEditResult r = m.importPreset(json, 0, 5000);
    assert(r.status == MATRIX_EDIT_BAD_REQUEST && !r.reason.empty());
  }
  assert(port.log.empty() && m.savePreset() == before && !m.dirty());
}

static void importDuringASwitchOrForAnotherPresetIsAConflict()
{
  OrderPort port;
  MatrixModel m(port, 2, 8, 2);
  m.setPreset(2);
  const std::string file = presetWith(m, 2, "{\"ch\":\"out\",\"idx\":0,\"mute\":false}");
  const std::string before = m.savePreset();
  port.log.clear();
  assert(m.importPreset(file, 1, 0).status == MATRIX_EDIT_CONFLICT);  // made against Preset B
  assert(m.beginPresetSwitch());
  assert(m.importPreset(file, 2, 0).status == MATRIX_EDIT_CONFLICT);
  m.endPresetSwitch();
  assert(port.log.empty() && m.savePreset() == before && !m.dirty());
}

// When its writes cannot be queued, the firmware takes the import back (model only).
static void undoRevertsAnImport()
{
  OrderPort port;
  MatrixModel m(port, 2, 8, 2);
  assert(patch(m, "{\"ch\":\"out\",\"idx\":0,\"mute\":false}", 100).empty());  // not yet autosaved
  const std::string before = m.savePreset();
  const std::string file = presetWith(m, 2, "{\"ch\":\"out\",\"idx\":0,\"gain\":-3}");
  assert(m.importPreset(file, 0, 5000).ok());
  m.undo();
  assert(m.savePreset() == before);
  assert(m.dirty() && m.saveDue(2100) && !m.saveDue(2099));  // the earlier edit still waits for Autosave
}

// Ticket 04: the Add-on B config (SPDIF mux) is per Preset and diffed on a switch.

static const MatrixAddOn kMuxSpdif1 = { true, 0x40, 0x01, 0x01 };
static const MatrixAddOn kMuxSpdif2 = { true, 0x40, 0x01, 0x02 };

static void presetSwitchWritesTheAddOnConfigOnlyWhenItDiffers()
{
  OrderPort port;
  MatrixModel m(port, 2, 8, 2);
  m.setAddOn(kMuxSpdif1);  // what the boot wrote: no write
  assert(patch(m, "{\"ch\":\"out\",\"idx\":1,\"mute\":false}").empty());
  const std::string same = m.savePreset();
  port.log.clear();
  assert(m.switchPreset(same, &kMuxSpdif1).empty());  // same config: nothing over I2C
  assert(port.log.empty());

  // different config: exactly one write, between ramp down and ramp up
  const std::string next = presetWith(m, 2, "{\"ch\":\"out\",\"idx\":1,\"route\":1}");
  assert(m.switchPreset(next, &kMuxSpdif2).empty());
  char wait[16];
  snprintf(wait, sizeof(wait), "w %d", MATRIX_RAMP_MS);
  assert(port.log == lines({ "g out 1 0.0000", wait, "r 1 1", "a 40 01 02", "g out 1 1.0000" }));
  assert(m.addOn().data == 0x02);

  // a Preset without its own config keeps the current one
  port.log.clear();
  assert(m.switchPreset(next).empty());
  assert(port.log.empty() && m.addOn().data == 0x02);
  m.switchToDefaults(&kMuxSpdif1);
  assert(port.log == lines({ "g out 1 0.0000", wait, "r 1 0", "a 40 01 01" }));
}

// A mux change is a live edit: written at once, no ramp, autosaved like any other change.
static void addOnEditIsWrittenAndAutosaved()
{
  OrderPort port;
  MatrixModel m(port, 2, 8, 2);
  m.setAddOn(kMuxSpdif1);
  assert(edit(m, "{\"ch\":\"addon\",\"i2c\":[64,1,2]}", 1000).ok());
  assert(port.log == lines({ "a 40 01 02" }));
  assert(m.addOn() == kMuxSpdif2 && m.dirty());
  assert(!m.saveDue(2999) && m.saveDue(3000));
  m.markSaved();

  port.log.clear();
  assert(edit(m, "{\"ch\":\"addon\",\"i2c\":[64,1,2]}", 4000).ok());  // unchanged: nothing
  assert(port.log.empty() && !m.dirty());

  m.undo();  // the unchanged edit was the last one: nothing to revert
  assert(edit(m, "{\"ch\":\"addon\",\"i2c\":[64,1,1]}", 5000).ok());
  m.undo();
  assert(m.addOn() == kMuxSpdif2 && !m.dirty());

  port.log.clear();
  assert(edit(m, "{\"ch\":\"addon\",\"i2c\":[64,1,256]}").status == MATRIX_EDIT_BAD_REQUEST);
  assert(edit(m, "{\"ch\":\"addon\",\"i2c\":[64,1]}").status == MATRIX_EDIT_BAD_REQUEST);
  assert(m.beginPresetSwitch());
  assert(edit(m, "{\"ch\":\"addon\",\"i2c\":[64,1,1]}").status == MATRIX_EDIT_CONFLICT);
  m.endPresetSwitch();
  assert(port.log.empty() && m.addOn() == kMuxSpdif2);
}

int main()
{
  editResultsDistinguishSuccessBadRequestAndConflict();
  editStatusesMapToDistinctHttpReplies();
  stateCarriesTheActivePreset();
  linkMembershipHasSeparateBusAndOutputScopes();
  linkedAspectsPropagateAndJoiningCopiesAbsoluteValues();
  joiningRejectsLinkedSettingsInTheSamePatch();
  eachLinkAspectCanBeEnabledIndependently();
  enablingAnAspectMakesExistingMembersConsistent();
  linkGroupsRoundTripAndUndoTogether();
  linkGroupEditsAreAddressedAsLinkGroups();
  v3PresetKeepsLinkGroupsAndLinkedAspects();
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
  multiBiquadEditsAreRamped();
  parameterEditsAndBypassAreRamped();
  unchangedCommitEmitsNothing();
  mutedChannelFilterEditsSkipTheRamp();
  linkedParameterEditRampsMembersTogether();
  routeSwitchAndLayoutChangeRampOnce();
  linkedLayoutChangeRampsMembersTogether();
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
  slotRulesAreTheSameForEditsAndPresets();
  presetVersionIsBumpedAndOldOnesStillLoad();
  undoRestoresTheLastAcceptedPatch();
  spdifOutDefaultsAndPairs();
  spdifOutEmitsLeftAndRightMuxWrites();
  spdifOutRejectsOddPairsAndUndoes();
  spdifOutSurvivesPresetRoundTrip();
  freshModelIsTheBaseline();
  applyAllEmitsFullWriteSet();
  muteAndGainWrites();
  busMuteWritesBusGain();
  routeChangeRampsDownSwitchesRampsUp();
  sourceChangeRampsTheBus();
  invalidPatchesAreRejectedWithoutSideEffects();
  namesAreStoredWithoutDspWrites();
  presetRoundTripWritesOnlyTheDifference();
  badPresetIsRejectedAndKeepsState();
  loadDefaultsResetsAndWritesTheDifference();
  saveIsDueTwoSecondsAfterTheLastChange();
  presetSwitchRejectsAnOverlappingEditAndKeepsPresetAssociation();
  loadingTheCurrentStateEmitsNothing();
  onePeqFrequencyChangeWritesOnlyThatBiquad();
  slotLayoutChangeWritesTheShiftedBiquadsAndRamps();
  onlySourceRouteAndFilterDifferencesMarkTheRamp();
  failedLoadLeavesModelAndWritesUntouched();
  presetSwitchRampsDownWritesTheDifferenceAndRampsUp();
  presetSwitchWithoutMarkedChannelsEmitsNoRamp();
  presetSwitchRampsOnlyAudibleChannels();
  failedPresetSwitchWritesNothing();
  switchToDefaultsTakesTheSharedRamp();
  importReplacesTheActivePresetThroughTheSharedRampAndIsAutosaved();
  invalidImportIsRejectedWithoutChangingState();
  importDuringASwitchOrForAnotherPresetIsAConflict();
  undoRevertsAnImport();
  presetSwitchWritesTheAddOnConfigOnlyWhenItDiffers();
  addOnEditIsWrittenAndAutosaved();
  printf("all tests passed\n");
  return 0;
}
