// Host test: the AudioFilterFactory's double path reproduces every Filter file test vector
// (SOURCES/filterfile/vectors.json, docs/filter-file.md) within the Filter file tolerance.
// Run: SOURCES/WEBAPP/ESP32/test/run.sh (runs all host tests)
// Links: AudioFilterFactory.cpp
// Uses: ArduinoJson
#include <cassert>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <ArduinoJson.h>
#include "../aurora/AudioFilterFactory.h"

typedef AudioFilterFactory::Biquad Biquad;
typedef std::complex<double> cplx;

static const double kPi = 3.14159265358979323846;
static const double kFs = 48000.0;

// H(z) of cascaded sections, RBJ signs: (b0 + b1 z^-1 + b2 z^-2) / (1 + a1 z^-1 + a2 z^-2)
static cplx response(const std::vector<Biquad>& secs, double f)
{
  const cplx z1 = std::polar(1.0, -2 * kPi * f / kFs), z2 = z1 * z1;
  cplx h = 1;
  for (size_t i = 0; i < secs.size(); i++)
  {
    const Biquad& s = secs[i];
    h *= (s.b0 + s.b1 * z1 + s.b2 * z2) / (1.0 + s.a1 * z1 + s.a2 * z2);
  }
  return h;
}

// The Filter file check (docs/filter-file.md, Verification): 0.01 dB / 0.1 deg where the
// reference is at or above -80 dB, else the file must stay below -79.99 dB.
static bool withinTolerance(const std::vector<Biquad>& file, const std::vector<Biquad>& ref, std::string& why)
{
  for (int k = 0;; k++)
  {
    const double f = 10.0 * std::pow(2.0, k / 24.0);
    if (f >= 24000)
      return true;
    const cplx hf = response(file, f), hr = response(ref, f);
    const double refDb = 20 * std::log10(std::abs(hr));
    char buf[160];
    if (refDb >= -80)
    {
      const double dDb = 20 * std::log10(std::abs(hf / hr)), dDeg = std::arg(hf / hr) * 180 / kPi;
      if (std::fabs(dDb) <= 0.01 && std::fabs(dDeg) <= 0.1)
        continue;
      snprintf(buf, sizeof(buf), "%.4g dB / %.4g deg at %.0f Hz", dDb, dDeg, f);
    }
    else
    {
      if (20 * std::log10(std::abs(hf)) < -79.99)
        continue;
      snprintf(buf, sizeof(buf), "above the -80 dB floor at %.0f Hz", f);
    }
    why = buf;
    return false;
  }
}

static double param(const std::string& line, const char* name)
{
  const std::string key = std::string(" ") + name + "=";
  const size_t at = line.find(key);
  assert(at != std::string::npos);
  return std::atof(line.c_str() + at + key.size());
}

// The factory's sections for a block line such as "PEQ f=120 gain=-4 Q=2.5" or "HP LR48 f=20".
static std::vector<Biquad> factory(const std::string& line)
{
  std::istringstream in(line);
  std::string type, design;
  in >> type >> design;
  Biquad s[4];
  int n = 1;
  if (type == "PEQ")
    s[0] = AudioFilterFactory::peq(param(line, "f"), param(line, "gain"), param(line, "Q"), kFs);
  else if (type == "LSHELF")
    s[0] = AudioFilterFactory::lowShelf(param(line, "f"), param(line, "gain"), param(line, "S"), kFs);
  else if (type == "HSHELF")
    s[0] = AudioFilterFactory::highShelf(param(line, "f"), param(line, "gain"), param(line, "S"), kFs);
  else if (type == "PHASE")
    s[0] = AudioFilterFactory::phase(param(line, "f"), param(line, "Q"), kFs);
  else
  {
    assert(type == "HP" || type == "LP");
    const bool high = type == "HP";
    const int order = std::atoi(design.c_str() + 2) / 6;
    if (design.compare(0, 2, "LR") == 0)
      n = AudioFilterFactory::linkwitzRiley(s, high, order, param(line, "f"), kFs);
    else
    {
      assert(design.compare(0, 2, "BW") == 0);
      n = AudioFilterFactory::butterworth(s, high, order, param(line, "f"), kFs);
    }
  }
  return std::vector<Biquad>(s, s + n);
}

static std::string readVectors()
{
  // next to this test file: run.sh compiles it by its absolute path, so __FILE__ is one
  std::string path = __FILE__;
  path = path.substr(0, path.find_last_of("/\\") + 1) + "../../../filterfile/vectors.json";
  std::ifstream in(path.c_str());
  assert(in && "vectors.json not found");
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

static void everyVectorIsReproduced()
{
  const std::string text = readVectors();
  DynamicJsonDocument doc(256 * 1024);
  assert(!deserializeJson(doc, text));
  assert(doc["fs"] == 48000);
  JsonArray vectors = doc["vectors"];
  assert(vectors.size() > 0);
  int failures = 0;
  for (JsonObject v : vectors)
  {
    const std::string block = v["block"].as<const char*>();
    std::vector<Biquad> file;
    for (JsonArray c : v["sections"].as<JsonArray>())
    {
      Biquad s = { c[0], c[1], c[2], c[3], c[4] };
      file.push_back(s);
    }
    const std::vector<Biquad> ours = factory(block);
    std::string why;
    if (ours.size() != file.size())
    {
      fprintf(stderr, "%s: %d sections, the vector has %d\n", block.c_str(), (int)ours.size(), (int)file.size());
      failures++;
    }
    else if (!withinTolerance(file, ours, why))
    {
      fprintf(stderr, "%s deviates: %s\n", block.c_str(), why.c_str());
      failures++;
    }
  }
  assert(failures == 0);
  printf("%d vectors reproduced\n", (int)vectors.size());
}

// Leaving the factory towards the DSP: rounded to float, a[0] = 1 and a1/a2 negated.
static void toDspRoundsAndNegatesTheDenominator()
{
  const Biquad s = { 0.1, 0.2, 0.3, -1.5, 0.75 };
  float a[3], b[3];
  AudioFilterFactory::toDsp(s, a, b);
  assert(b[0] == 0.1f && b[1] == 0.2f && b[2] == 0.3f);
  assert(a[0] == 1.0f && a[1] == 1.5f && a[2] == -0.75f);
}

int main()
{
  everyVectorIsReproduced();
  toDspRoundsAndNegatesTheDenominator();
  printf("all tests passed\n");
  return 0;
}
