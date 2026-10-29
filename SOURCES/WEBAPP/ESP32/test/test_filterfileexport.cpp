// Host tests for the Filter file export of a matrix Channel (filterimport.cpp): the exported file
// passes the reference check (SOURCES/filterfile/aurorafilter.py check) and imports back into
// the same Channel.
// Run: SOURCES/WEBAPP/ESP32/test/run.sh (runs all host tests)
// Links: filterimport.cpp filterfile.cpp matrixmodel.cpp AudioFilterFactory.cpp
// Uses: ArduinoJson
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <unistd.h>
#include "../aurora/filterimport.h"

struct NullPort : MatrixPort
{
  void writeGain(MatrixChannelKind, int, double) override {}
  void writeRoute(int, int) override {}
  void writeSource(int, uint32_t) override {}
  void writePolarity(int, bool) override {}
  void writeDelay(int, int) override {}
  void writeSpdifOut(uint32_t, uint32_t) override {}
  void waitMs(int) override {}
  void writeBiquad(MatrixChannelKind, int, int, const float*, const float*) override {}
  void writeAddOn(const MatrixAddOn&) override {}
};

static void patch(MatrixModel& m, const std::string& json)
{
  DynamicJsonDocument doc(1024);
  assert(!deserializeJson(doc, json));
  const MatrixEditResult r = m.apply(doc.as<JsonObject>(), 0);
  if (!r.ok()) fprintf(stderr, "patch rejected: %s\n", r.reason.c_str());
  assert(r.ok());
}

// The Channel as stateJson() shows it, serialized: what the round trip must keep.
static std::string channelOf(const MatrixModel& m, const char* ch, int idx)
{
  DynamicJsonDocument doc(65536);
  assert(!deserializeJson(doc, m.stateJson()));
  std::string s;
  serializeJson(doc[std::string(ch) == "bus" ? "buses" : "outs"][idx], s);
  return s;
}

static std::string exported(const MatrixModel& m, const char* ch, int idx)
{
  std::string text;
  assert(exportFilterFile(m, ch, idx, text) == MATRIX_EDIT_OK);
  return text;
}

static void importInto(MatrixModel& m, const char* ch, int idx, const std::string& text)
{
  FilterFileRow row;
  std::vector<std::string> errors;
  const MatrixEditStatus s = parseFilterFileFor(m, ch, text.data(), text.size(), row, errors);
  for (size_t i = 0; i < errors.size(); i++) fprintf(stderr, "import: %s\n", errors[i].c_str());
  assert(s == MATRIX_EDIT_OK);
  assert(applyFilterFile(m, ch, idx, m.preset(), row, 0).ok());
}

// Runs the reference check on text; true if it accepts the file.
static bool referenceAccepts(const std::string& text, const char* options)
{
  const char* tmp = getenv("TMPDIR");
  if (!tmp) tmp = getenv("TEMP");
  char path[512];
  snprintf(path, sizeof(path), "%s/aurora-export-%d.txt", tmp ? tmp : "/tmp", (int)getpid());
  FILE* f = fopen(path, "wb");
  assert(f);
  fwrite(text.data(), 1, text.size(), f);
  fclose(f);

  std::string script = __FILE__;
  script = script.substr(0, script.find_last_of("/\\") + 1) + "../../../filterfile/aurorafilter.py";
  const char* py = getenv("PYTHON");
  std::string cmd = std::string(py ? py : "python3") + " \"" + script + "\" check \"" + path + "\" " + options;
  const int status = system(cmd.c_str());
  remove(path);
  return status == 0;
}

static bool has(const std::string& text, const char* line)
{
  return text.find(std::string("\n") + line + "\n") != std::string::npos;
}

// One Slot of every type, odd float values, gain and delay bypass on.
static void fillOutput(MatrixModel& m, int idx)
{
  char head[64];
  snprintf(head, sizeof(head), "{\"ch\":\"out\",\"idx\":%d,", idx);
  const std::string h = head;
  patch(m, h + "\"name\":\"Woofer L\",\"gain\":-3.3,\"delayUs\":1250,\"polarity\":true}");
  patch(m, h + "\"slot\":{\"op\":\"add\",\"type\":\"hp\",\"design\":\"bw\",\"order\":3,\"freq\":30}}");
  patch(m, h + "\"slot\":{\"op\":\"add\",\"type\":\"lp\",\"design\":\"lr\",\"order\":8,\"freq\":2500}}");
  patch(m, h + "\"slot\":{\"op\":\"add\",\"type\":\"peq\",\"freq\":120.7,\"q\":2.5,\"gain\":-4.1}}");
  patch(m, h + "\"slot\":{\"op\":\"add\",\"type\":\"hshelf\",\"freq\":8000,\"gain\":-2,\"slope\":0.7}}");
  patch(m, h + "\"slot\":{\"op\":\"set\",\"i\":3,\"bypass\":true}}");
  patch(m, h + "\"slot\":{\"op\":\"add\",\"type\":\"phase\",\"freq\":800,\"q\":0.7}}");
  patch(m, h + "\"slot\":{\"op\":\"add\",\"type\":\"lshelf\",\"freq\":19999.9,\"gain\":24,\"slope\":0.1}}");
}

static void anOutputRoundTrips()
{
  NullPort port;
  MatrixModel m(port, 2, 8, 10, 1920);
  fillOutput(m, 2);
  const std::string text = exported(m, "out", 2);
  assert(referenceAccepts(text, "--biquads 10"));

  MatrixModel back(port, 2, 8, 10, 1920);
  importInto(back, "out", 2, text);
  assert(channelOf(back, "out", 2) == channelOf(m, "out", 2));
}

static void theFileReadsLikeTheExample()
{
  NullPort port;
  MatrixModel m(port, 2, 8, 10, 1920);
  fillOutput(m, 0);
  const std::string text = exported(m, "out", 0);
  assert(text.compare(0, 25, "aurora-filter 1\nfs 48000\n") == 0);
  assert(has(text, "name Woofer L"));
  assert(has(text, "gain -3.3 dB"));
  assert(has(text, "delay 1.25 ms"));
  assert(has(text, "polarity inverted"));
  assert(has(text, "HP BW18 f=30"));
  assert(has(text, "LP LR48 f=2500"));
  assert(has(text, "PEQ f=120.7 gain=-4.1 Q=2.5"));
  assert(has(text, "HSHELF f=8000 gain=-2 S=0.7 bypass"));
  assert(has(text, "PHASE f=800 Q=0.7"));
  assert(has(text, "LSHELF f=19999.9 gain=24 S=0.1"));
}

static void aBusRoundTripsWithoutDelayAndPolarity()
{
  NullPort port;
  MatrixModel m(port, 2, 8, 4, 1920);
  patch(m, "{\"ch\":\"bus\",\"idx\":1,\"name\":\"Sub\",\"gain\":11.9,"
           "\"slot\":{\"op\":\"add\",\"type\":\"lp\",\"design\":\"lr\",\"order\":4,\"freq\":80}}");
  const std::string text = exported(m, "bus", 1);
  assert(text.find("delay") == std::string::npos && text.find("polarity") == std::string::npos);
  assert(referenceAccepts(text, "--bus --biquads 4"));

  MatrixModel back(port, 2, 8, 4, 1920);
  importInto(back, "bus", 1, text);
  assert(channelOf(back, "bus", 1) == channelOf(m, "bus", 1));
}

static void anOddDelayComesBackToTheSameSample()
{
  NullPort port;
  MatrixModel m(port, 2, 8, 10, 1920);
  for (int samples = 1; samples <= 1920; samples += 7)
  {
    char json[96];
    snprintf(json, sizeof(json), "{\"ch\":\"out\",\"idx\":0,\"delayUs\":%.17g}", samples * 1e6 / 48000.0);
    patch(m, json);
    MatrixModel back(port, 2, 8, 10, 1920);
    importInto(back, "out", 0, exported(m, "out", 0));
    assert(channelOf(back, "out", 0) == channelOf(m, "out", 0));
  }
}

static void aNameTheFileCannotCarryIsLeftOut()
{
  NullPort port;
  MatrixModel m(port, 2, 8, 10, 1920);
  const char* const names[] = { "", " padded", "tab\\there", "bad\xff" };
  for (int k = 0; k < 4; k++)
  {
    patch(m, std::string("{\"ch\":\"out\",\"idx\":1,\"name\":\"") + names[k] + "\"}");
    const std::string text = exported(m, "out", 1);
    assert(text.find("\nname") == std::string::npos);
    assert(referenceAccepts(text, ""));
  }
}

static void unknownChannelsAreBadRequests()
{
  NullPort port;
  MatrixModel m(port, 2, 8, 10, 1920);
  std::string text;
  assert(exportFilterFile(m, "out", 8, text) == MATRIX_EDIT_BAD_REQUEST);
  assert(exportFilterFile(m, "bus", 2, text) == MATRIX_EDIT_BAD_REQUEST);
  assert(exportFilterFile(m, "spdif", 0, text) == MATRIX_EDIT_BAD_REQUEST);
}

static void decimalsAreTheShortestThatReadBack()
{
  assert(filterFileDecimal(30.0, false) == "30");
  assert(filterFileDecimal(-3.3, false) == "-3.3");
  assert(filterFileDecimal((double)0.7f, true) == "0.7");    // the float 0.7f
  assert(filterFileDecimal((double)0.7f, false) != "0.7");   // the double of 0.7f is not 0.7
  assert(strtof(filterFileDecimal((double)0.7f, false).c_str(), NULL) == 0.7f);
  assert(filterFileDecimal((double)1234.567f, true) == "1234.567");
}

int main()
{
  decimalsAreTheShortestThatReadBack();
  anOutputRoundTrips();
  theFileReadsLikeTheExample();
  aBusRoundTripsWithoutDelayAndPolarity();
  anOddDelayComesBackToTheSameSample();
  aNameTheFileCannotCarryIsLeftOut();
  unknownChannelsAreBadRequests();
  printf("all tests passed\n");
  return 0;
}
