// Host tests for the Filter file import into a matrix Channel (filterfilechannel.cpp): the file
// replaces the Filter row as one patch through the model, with the Channel's limits, its Link
// group and the Preset conflict, and a rejected file changes and writes nothing.
// Run: SOURCES/WEBAPP/ESP32/test/run.sh (runs all host tests)
// Links: filterfilechannel.cpp filterfile.cpp matrixmodel.cpp AudioFilterFactory.cpp
// Uses: ArduinoJson
#include <cassert>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include "../aurora/filterfilechannel.h"

// Counts the DSP writes; what is written is the model's business (test_matrixmodel.cpp).
struct CountingPort : MatrixPort
{
  int writes = 0;
  int biquads = 0;
  void writeGain(MatrixChannelKind, int, double) override { writes++; }
  void writeRoute(int, int) override { writes++; }
  void writeSource(int, uint32_t) override { writes++; }
  void writePolarity(int, bool) override { writes++; }
  void writeDelay(int, int) override { writes++; }
  void writeSpdifOut(uint32_t, uint32_t) override { writes++; }
  void waitMs(int) override {}
  void writeBiquad(MatrixChannelKind, int, int, const float*, const float*) override { writes++; biquads++; }
  void writeAddOn(const MatrixAddOn&) override { writes++; }
};

struct Import
{
  MatrixEditStatus status;
  std::vector<std::string> errors;
};

// What the firmware does with POST /matrix/filterfile: parse, then apply.
static Import import(MatrixModel& m, const char* ch, int idx, const std::string& text, int preset = 0)
{
  FilterFileRow row;
  Import r;
  r.status = parseFilterFileFor(m, ch, text.data(), text.size(), row, r.errors);
  if (r.status != MATRIX_EDIT_OK)
    return r;
  const MatrixEditResult edit = applyFilterFile(m, ch, idx, preset, row, 1000);
  r.status = edit.status;
  if (!edit.ok())
    r.errors.push_back(edit.reason);
  return r;
}

static std::string patch(MatrixModel& m, const char* json)
{
  DynamicJsonDocument doc(512);
  assert(!deserializeJson(doc, json));
  return m.apply(doc.as<JsonObject>(), 0).reason;
}

static DynamicJsonDocument stateOf(const MatrixModel& m)
{
  DynamicJsonDocument doc(65536);
  assert(!deserializeJson(doc, m.stateJson()));
  return doc;
}

static std::string exampleFile()
{
  std::string path = __FILE__;
  path = path.substr(0, path.find_last_of("/\\") + 1) + "../../../filterfile/example.txt";
  std::ifstream in(path.c_str(), std::ios::binary);
  assert(in);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

static const char* const kHead = "aurora-filter 1\nfs 48000\n";
// A valid PEQ block (reference coefficients from vectors.json).
static const char* const kPeq =
  "PEQ f=1000 gain=3 Q=1\n"
  "  1.021474096402299 -1.8796730201312601 0.8744185481232504 -1.8796730201312601 0.8958926445255495\n";

static void theExampleReplacesAnOutputsFilterRow()
{
  CountingPort port;
  MatrixModel m(port, 2, 8, 10, 1920);
  assert(patch(m, "{\"ch\":\"out\",\"idx\":0,\"gainBypass\":true,\"delayBypass\":true,"
                  "\"slot\":{\"op\":\"add\",\"type\":\"peq\",\"freq\":50}}").empty());
  port.writes = port.biquads = 0;

  const Import r = import(m, "out", 0, exampleFile());
  assert(r.status == MATRIX_EDIT_OK && r.errors.empty());

  DynamicJsonDocument st = stateOf(m);
  JsonObject o = st["outs"][0];
  assert(std::string(o["name"].as<const char*>()) == "Woofer L");
  assert(o["gain"] == -3.0f && o["gainBypass"] == false);
  assert(o["delaySamples"] == 60 && o["delayBypass"] == false && o["polarity"] == true);
  JsonArray slots = o["slots"];
  assert(slots.size() == 5);  // the LR48 is one Slot
  assert(std::string(slots[0]["type"].as<const char*>()) == "hp" && slots[0]["order"] == 3);
  assert(std::string(slots[1]["design"].as<const char*>()) == "lr" && slots[1]["order"] == 8);
  assert(slots[1]["freq"] == 2500.0f);
  assert(slots[2]["q"] == 2.5f && slots[2]["gain"] == -4.0f);
  assert(std::string(slots[3]["type"].as<const char*>()) == "hshelf");
  assert(std::fabs(slots[3]["slope"].as<double>() - 0.7) < 1e-6);
  assert(slots[3]["bypass"] == true && slots[4]["bypass"] == false);
  assert(o["used"] == 9);
  assert(port.biquads > 0);  // the DSP gets the new filters
  assert(m.dirty());         // Autosave stores it
}

static void missingLinesAreNeutralAndTheNameIsKept()
{
  CountingPort port;
  MatrixModel m(port, 2, 8, 10, 1920);
  assert(patch(m, "{\"ch\":\"out\",\"idx\":3,\"name\":\"Tweeter\",\"gain\":-6,\"delayUs\":1000,\"polarity\":true,"
                  "\"slot\":{\"op\":\"add\",\"type\":\"peq\",\"freq\":50}}").empty());
  assert(import(m, "out", 3, kHead).status == MATRIX_EDIT_OK);
  JsonObject o = stateOf(m)["outs"][3];
  assert(std::string(o["name"].as<const char*>()) == "Tweeter");
  assert(o["gain"] == 0 && o["delaySamples"] == 0 && o["polarity"] == false && o["slots"].size() == 0);
}

static void aRejectedFileChangesAndWritesNothing()
{
  CountingPort port;
  MatrixModel m(port, 2, 8, 10, 1920);
  assert(patch(m, "{\"ch\":\"out\",\"idx\":0,\"gain\":-6,\"slot\":{\"op\":\"add\",\"type\":\"peq\",\"freq\":50}}").empty());
  m.markSaved();
  const std::string before = m.stateJson();
  port.writes = 0;

  std::string bad = std::string(kHead) + "gain -3 dB\n" + kPeq;
  bad.replace(bad.find("gain=3"), 6, "gain=4");  // the coefficients no longer match
  const Import r = import(m, "out", 0, bad);
  assert(r.status == MATRIX_EDIT_BAD_REQUEST);
  assert(r.errors.size() == 1 && r.errors[0].find("line 4: PEQ deviates") == 0);
  assert(m.stateJson() == before && port.writes == 0 && !m.dirty());
}

static void theChannelsLimitsApply()
{
  CountingPort port;
  MatrixModel m(port, 2, 8, 1, 240);
  std::string two = std::string(kHead) + kPeq + kPeq;
  Import r = import(m, "out", 0, two);
  assert(r.errors.size() == 1 && r.errors[0] == "the filters need 2 biquads, the Channel has 1");
  r = import(m, "out", 0, std::string(kHead) + "delay 10 ms\n");
  assert(r.errors.size() == 1 && r.errors[0] == "line 3: delay 10 ms outside 0 to 5 ms");
  r = import(m, "bus", 1, std::string(kHead) + "delay 1 ms\npolarity inverted\n");
  assert(r.errors.size() == 2 && r.errors[0] == "line 3: a Bus has no delay");

  r = import(m, "bus", 1, std::string(kHead) + "name Sub\ngain 2 dB\n" + kPeq);
  assert(r.status == MATRIX_EDIT_OK);
  JsonObject b = stateOf(m)["buses"][1];
  assert(std::string(b["name"].as<const char*>()) == "Sub" && b["gain"] == 2 && b["slots"].size() == 1);
}

static void unknownChannelsAreBadRequests()
{
  CountingPort port;
  MatrixModel m(port, 2, 8, 10, 1920);
  Import r = import(m, "out", 8, kHead);
  assert(r.status == MATRIX_EDIT_BAD_REQUEST && r.errors[0] == "no such channel");
  r = import(m, "spdif", 0, kHead);
  assert(r.status == MATRIX_EDIT_BAD_REQUEST && r.errors[0] == "unknown channel kind");
}

static void theLinkGroupTakesTheLinkedAspects()
{
  CountingPort port;
  MatrixModel m(port, 2, 8, 10, 1920);
  assert(patch(m, "{\"ch\":\"out\",\"idx\":0,\"link\":\"A\"}").empty());
  assert(patch(m, "{\"ch\":\"out\",\"idx\":1,\"link\":\"A\"}").empty());
  assert(patch(m, "{\"linkGroup\":\"A\",\"filters\":true,\"gain\":true}").empty());

  assert(import(m, "out", 0, std::string(kHead) + "name Left\ngain -2 dB\ndelay 1 ms\n" + kPeq).status == MATRIX_EDIT_OK);
  DynamicJsonDocument st = stateOf(m);
  JsonObject other = st["outs"][1];
  assert(other["slots"].size() == 1 && other["gain"] == -2);
  assert(other["delaySamples"] == 0);  // delay is not linked here
  assert(std::string(other["name"].as<const char*>()) != "Left");
}

static void aPresetConflictRejectsTheImport()
{
  CountingPort port;
  MatrixModel m(port, 2, 8, 10, 1920);
  m.setPreset(1);
  const std::string before = m.stateJson();
  Import r = import(m, "out", 0, std::string(kHead) + kPeq, 0);
  assert(r.status == MATRIX_EDIT_CONFLICT && m.stateJson() == before);
  assert(m.beginPresetSwitch());
  r = import(m, "out", 0, std::string(kHead) + kPeq, 1);
  assert(r.status == MATRIX_EDIT_CONFLICT && m.stateJson() == before);
  m.endPresetSwitch();
  assert(import(m, "out", 0, std::string(kHead) + kPeq, 1).status == MATRIX_EDIT_OK);
}

int main()
{
  theExampleReplacesAnOutputsFilterRow();
  missingLinesAreNeutralAndTheNameIsKept();
  aRejectedFileChangesAndWritesNothing();
  theChannelsLimitsApply();
  unknownChannelsAreBadRequests();
  theLinkGroupTakesTheLinkedAspects();
  aPresetConflictRejectsTheImport();
  printf("all tests passed\n");
  return 0;
}
