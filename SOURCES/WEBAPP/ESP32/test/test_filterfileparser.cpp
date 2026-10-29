// Host test: the firmware's Filter file parser (filterfile.cpp, docs/filter-file.md) gives the
// reference's result for every shared case (SOURCES/filterfile/cases.json, made by filecases.py
// from the cases of test_filterfile.py): the same accepted rows and the same error texts.
// Run: SOURCES/WEBAPP/ESP32/test/run.sh (runs all host tests)
// Links: filterfile.cpp AudioFilterFactory.cpp
// Uses: ArduinoJson
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <ArduinoJson.h>
#include "../aurora/filterfile.h"

static std::string readFile(const char* relative)
{
  // next to this test file: run.sh compiles it by its absolute path, so __FILE__ is one
  std::string path = __FILE__;
  path = path.substr(0, path.find_last_of("/\\") + 1) + relative;
  std::ifstream in(path.c_str(), std::ios::binary);
  assert(in && "file not found");
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

static std::string fromHex(const char* hex)
{
  std::string out;
  for (size_t i = 0; hex[i] && hex[i + 1]; i += 2)
    out += (char)strtol(std::string(hex + i, 2).c_str(), NULL, 16);
  return out;
}

static std::string designOf(const FilterFileBlock& b)
{
  if (b.type != FILTERFILE_HP && b.type != FILTERFILE_LP)
    return "";
  char buf[8];
  snprintf(buf, sizeof(buf), "%s%d", b.lr ? "LR" : "BW", b.order * 6);
  return buf;
}

// ArduinoJson's number parser can be an ulp off strtod's
static bool same(double a, double b) { return std::fabs(a - b) <= 1e-12 * std::fabs(b); }

// Compares an accepted row with the reference's; prints the first difference.
static bool sameRow(const FilterFileRow& row, JsonObject ref, std::string& why)
{
  char buf[200];
  if (ref["name"].isNull() ? row.hasName : (!row.hasName || row.name != ref["name"].as<const char*>()))
    return why = "name differs: " + row.name, false;
  if (!same(row.gain, ref["gain"]) || row.delay != ref["delay"].as<int>()
      || row.inverted != (std::string(ref["polarity"].as<const char*>()) == "inverted"))
  {
    snprintf(buf, sizeof(buf), "gain %g delay %d inverted %d", row.gain, row.delay, row.inverted);
    return why = buf, false;
  }
  JsonArray blocks = ref["blocks"];
  if (row.blocks.size() != blocks.size())
    return why = "block count differs", false;
  for (size_t i = 0; i < blocks.size(); i++)
  {
    const FilterFileBlock& b = row.blocks[i];
    JsonObject r = blocks[i];
    const std::string design = r["design"].isNull() ? "" : r["design"].as<const char*>();
    JsonObject p = r["params"];
    if (b.line != r["line"].as<int>() || std::string(filterFileTypeName(b.type)) != r["type"].as<const char*>()
        || designOf(b) != design || b.bypass != r["bypass"].as<bool>() || !same(b.f, p["f"])
        || (p.containsKey("gain") && !same(b.gain, p["gain"])) || (p.containsKey("q") && !same(b.q, p["q"]))
        || (p.containsKey("s") && !same(b.s, p["s"])))
    {
      snprintf(buf, sizeof(buf), "block %d differs: line %d %s %s f %g", (int)i, b.line, filterFileTypeName(b.type),
               designOf(b).c_str(), b.f);
      return why = buf, false;
    }
  }
  return true;
}

static void everySharedCaseHasTheReferencesOutcome()
{
  const std::string json = readFile("../../../filterfile/cases.json");
  DynamicJsonDocument doc(256 * 1024);
  assert(!deserializeJson(doc, json));
  JsonArray cases = doc["cases"];
  assert(cases.size() > 50);
  int failures = 0;
  for (JsonObject c : cases)
  {
    const char* name = c["name"];
    const std::string text = c.containsKey("hex") ? fromHex(c["hex"]) : std::string(c["text"].as<const char*>());
    FilterFileLimits lim;
    lim.bus = c["bus"] | false;
    lim.biquads = c["biquads"] | -1;
    lim.maxDelaySamples = c["max_delay_samples"] | 1920;
    FilterFileRow row;
    const std::vector<std::string> errors = parseFilterFile(text.data(), text.size(), lim, row);

    std::vector<std::string> expected;
    for (JsonVariant e : c["errors"].as<JsonArray>())
      expected.push_back(e.as<const char*>());
    if (errors != expected)
    {
      fprintf(stderr, "%s: errors differ\n", name);
      for (size_t i = 0; i < expected.size(); i++) fprintf(stderr, "  want: %s\n", expected[i].c_str());
      for (size_t i = 0; i < errors.size(); i++) fprintf(stderr, "  got:  %s\n", errors[i].c_str());
      failures++;
      continue;
    }
    std::string why;
    if (expected.empty() && !sameRow(row, c["result"], why))
    {
      fprintf(stderr, "%s: %s\n", name, why.c_str());
      failures++;
    }
  }
  assert(failures == 0);
  printf("%d shared cases agree with the reference\n", (int)cases.size());
}

// "example.txt and every vector imports": each vector as a one-block file.
static void everyVectorParsesAsAFile()
{
  const std::string json = readFile("../../../filterfile/vectors.json");
  DynamicJsonDocument doc(256 * 1024);
  assert(!deserializeJson(doc, json));
  int n = 0;
  for (JsonObject v : doc["vectors"].as<JsonArray>())
  {
    std::string text = std::string("aurora-filter 1\nfs 48000\n") + v["block"].as<const char*>() + "\n";
    for (JsonArray s : v["sections"].as<JsonArray>())
    {
      char line[200];
      snprintf(line, sizeof(line), "  %.17g %.17g %.17g %.17g %.17g\n", s[0].as<double>(), s[1].as<double>(),
               s[2].as<double>(), s[3].as<double>(), s[4].as<double>());
      text += line;
    }
    FilterFileLimits lim = { false, 4, 1920 };
    FilterFileRow row;
    const std::vector<std::string> errors = parseFilterFile(text.data(), text.size(), lim, row);
    if (!errors.empty())
      fprintf(stderr, "%s: %s\n", v["block"].as<const char*>(), errors[0].c_str());
    assert(errors.empty() && row.blocks.size() == 1);
    n++;
  }
  const std::string example = readFile("../../../filterfile/example.txt");
  FilterFileLimits lim = { false, 9, 1920 };
  FilterFileRow row;
  assert(parseFilterFile(example.data(), example.size(), lim, row).empty());
  assert(row.blocks.size() == 5 && row.name == "Woofer L" && row.delay == 60 && row.inverted);
  printf("%d vectors and example.txt parse\n", n);
}

// The reply of POST /matrix/filterfile for a rejected file.
static void theErrorsAreAJsonList()
{
  std::vector<std::string> errors;
  errors.push_back("line 3: unknown line \"NOTCH\"");
  errors.push_back("missing \"fs 48000\"");
  assert(filterFileErrorsJson(errors) == "{\"errors\":[\"line 3: unknown line \\\"NOTCH\\\"\",\"missing \\\"fs 48000\\\"\"]}");
  assert(filterFileErrorsJson(std::vector<std::string>()) == "{\"errors\":[]}");
}

int main()
{
  everySharedCaseHasTheReferencesOutcome();
  everyVectorParsesAsAFile();
  theErrorsAreAJsonList();
  printf("all tests passed\n");
  return 0;
}
