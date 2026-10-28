// Host tests for the Butterworth and Linkwitz-Riley designs of the AudioFilterFactory.
// Run: SOURCES/WEBAPP/ESP32/test/run.sh (runs all host tests)
// Links: AudioFilterFactory.cpp
#include <cassert>
#include <cmath>
#include <cstdio>
#include "../aurora/AudioFilterFactory.h"

static const double kPi = 3.14159265358979323846;
static const float kFs = 48000.0f;

// magnitude in dB of n cascaded biquads, H = (b0+b1 z^-1+b2 z^-2) / (1 + a1 z^-1 + a2 z^-2) with the
// factory's sign convention a1/a2 = negated denominator
static double magDb(const float* a, const float* b, int n, double f)
{
  const double w = 2 * kPi * f / kFs;
  double db = 0;
  for (int s = 0; s < n; s++)
  {
    const float* A = a + 3 * s;
    const float* B = b + 3 * s;
    double nr = B[0] + B[1] * std::cos(w) + B[2] * std::cos(2 * w), ni = -B[1] * std::sin(w) - B[2] * std::sin(2 * w);
    double dr = 1 - A[1] * std::cos(w) - A[2] * std::cos(2 * w), di = A[1] * std::sin(w) + A[2] * std::sin(2 * w);
    db += 10 * std::log10((nr * nr + ni * ni) / (dr * dr + di * di));
  }
  return db;
}

// Reference from the closed form of a bilinear-transformed Butterworth of order n:
// |H|^2 = 1 / (1 + t^2n), t = tan(w/2) / tan(wc/2) for LP, the inverse for HP. LR(2n) is its square.
static double refDb(bool high, int butterOrder, bool squared, double fc, double f)
{
  double t = std::tan(kPi * f / kFs) / std::tan(kPi * fc / kFs);
  if (high)
    t = 1.0 / t;
  double db = -10 * std::log10(1 + std::pow(t, 2 * butterOrder));
  return squared ? 2 * db : db;
}

static void butterworthMatchesTheClosedForm()
{
  const double fcs[] = { 200, 1000, 5000 };
  const double fs[] = { 50, 150, 300, 800, 1000, 1500, 3000, 4000, 8000, 15000 };
  for (int order = 1; order <= 8; order++)
    for (int hp = 0; hp < 2; hp++)
      for (double fc : fcs)
      {
        float a[12] = { 0 }, b[12] = { 0 };
        int n = AudioFilterFactory::makeButterworth(a, b, hp != 0, order, (float)fc, kFs);
        assert(n == (order + 1) / 2);  // biquads used
        for (double f : fs)
        {
          const double ref = refDb(hp != 0, order, false, fc, f);
          if (ref < -90)  // deep in the stopband float coefficients cannot follow
            continue;
          assert(std::fabs(magDb(a, b, n, f) - ref) < 0.05);
        }
        // -3 dB at the corner
        assert(std::fabs(magDb(a, b, n, fc) + 3.0103) < 0.05);
      }
}

static void firstOrderSectionUsesOnlyTheFirstTapPair()
{
  float a[3] = { 0 }, b[3] = { 0 };
  AudioFilterFactory::makeButterworth(a, b, false, 1, 1000.0f, kFs);
  assert(b[2] == 0 && a[2] == 0);
  // reference LP: K = tan(pi fc/fs): b0 = b1 = K/(1+K), a1 = (1-K)/(1+K) (denominator 1 - a1 z^-1)
  const double K = std::tan(kPi * 1000.0 / kFs);
  assert(std::fabs(b[0] - K / (1 + K)) < 1e-6 && std::fabs(b[1] - K / (1 + K)) < 1e-6);
  assert(std::fabs(a[1] - (1 - K) / (1 + K)) < 1e-6);
}

static void linkwitzRileyMatchesTheClosedForm()
{
  const int orders[] = { 2, 4, 8 };
  const double fs[] = { 100, 500, 800, 1000, 1250, 2000, 4000 };
  for (int order : orders)
    for (int hp = 0; hp < 2; hp++)
    {
      float a[12] = { 0 }, b[12] = { 0 };
      int n = AudioFilterFactory::makeLinkwitzRiley(a, b, hp != 0, order, 1000.0f, kFs);
      assert(n == order / 2);
      for (double f : fs)
      {
        const double ref = refDb(hp != 0, order / 2, true, 1000.0, f);
        if (ref < -90)
          continue;
        assert(std::fabs(magDb(a, b, n, f) - ref) < 0.05);
      }
      assert(std::fabs(magDb(a, b, n, 1000) + 6.0206) < 0.05);  // -6 dB at the corner
    }
}

int main()
{
  butterworthMatchesTheClosedForm();
  firstOrderSectionUsesOnlyTheFirstTapPair();
  linkwitzRileyMatchesTheClosedForm();
  printf("all tests passed\n");
  return 0;
}
