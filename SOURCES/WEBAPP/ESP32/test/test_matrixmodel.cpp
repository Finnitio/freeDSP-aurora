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
  void writeGain(MatrixChannelKind kind, int idx, double linear) override { w.push_back({ 'g', kind, idx, linear }); }
  void writeRoute(int out, int route) override { w.push_back({ 'r', MATRIX_OUT, out, (double)route }); }
  void writeSource(int bus, uint32_t sel) override { w.push_back({ 's', MATRIX_BUS, bus, (double)sel }); }
  void writePolarity(int out, bool invert) override { w.push_back({ 'p', MATRIX_OUT, out, invert ? 1.0 : 0.0 }); }
  void writeDelay(int out, int samples) override { w.push_back({ 'd', MATRIX_OUT, out, (double)samples }); }
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
  DynamicJsonDocument doc(16384);
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
  // 2 x (source + gain) + 8 x (route + gain + polarity + delay)
  assert(port.w.size() == 4 + 32);
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
  assert(port2.w.size() == 4 + 32);  // full write set
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
  assert(port.w.size() == 4 + 32);
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
  assert(st["outs"][1]["addable"].size() == 0);
  assert(st["outs"][0]["addable"].size() == 1);
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

int main()
{
  delayMicrosecondsRoundToSamples();
  delayBoundaryIs40ms();
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
