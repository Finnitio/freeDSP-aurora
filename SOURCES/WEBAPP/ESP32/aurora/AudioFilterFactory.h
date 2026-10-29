#ifndef __AUDIOFILTERFACTORY__
#define __AUDIOFILTERFACTORY__

class AudioFilterFactory
{
public:
  enum tfilterdesign
  {
    kBessel6 = 0x00,
    kBessel12,
    kBessel18,
    kBessel24,
    kButterworth12,
    kButterworth18,
    kButterworth24,
    kLinkwitzRiley12,
    kLinkwitzRiley24,
    kLinkwitzRiley36,
    kLinkwitzRiley48,
    kNumFilterDesigns
  };

  /*! One biquad section in double, normalized to a0 = 1 with the RBJ signs:
   *  H(z) = (b0 + b1 z^-1 + b2 z^-2) / (1 + a1 z^-1 + a2 z^-2). */
  struct Biquad
  {
    double b0, b1, b2, a1, a2;
  };

public:
  //============================================================================
  // The double path: the formulas of the Filter file (docs/filter-file.md), shared with
  // SOURCES/filterfile/aurorafilter.py. The matrix Plugin computes its Slots with these and
  // rounds to float only in toDsp().

  /*! Peaking EQ, RBJ cookbook alpha = sin(w0) / (2Q). */
  static Biquad peq( const double fc, const double gain, const double Q, const double fs );
  /*! Shelving filters of slope S (0 < S <= 1). */
  static Biquad lowShelf( const double fc, const double gain, const double S, const double fs );
  static Biquad highShelf( const double fc, const double gain, const double S, const double fs );
  /*! 2nd-order allpass (Phase filter). */
  static Biquad phase( const double fc, const double Q, const double fs );
  /*! Butterworth of order 1..8: an odd order starts with a 1st-order section (b2 = a2 = 0),
   *  then the 2nd-order sections, lowest Q first. Returns the number of sections (<= 4). */
  static int butterworth( Biquad out[], const bool high, const int order, const double fc, const double fs );
  /*! Linkwitz-Riley of order 2 (one section, Q = 0.5), 4 or 8 (the Butterworth of half the
   *  order twice). Returns the number of sections (<= 4). */
  static int linkwitzRiley( Biquad out[], const bool high, const int order, const double fc, const double fs );

  /*! Rounds a section to float in the DSP layout: a[0] = 1, a[1] = -a1, a[2] = -a2. */
  static void toDsp( const Biquad& s, float a[], float b[] );
  /*! n sections, 3 coefficients apart in a and b. Returns n. */
  static int toDsp( const Biquad secs[], const int n, float a[], float b[] );

  //============================================================================
  // The float interface of the legacy Plugins (adau1452.cpp) and the DSP layout above.

  /*! Calculates the biquad coefficients for a parametric EQ of the legacy Plugins.
   *
   * Uses the bandwidth form alpha = sin(w0) sinh(ln2/2 BW w0/sin(w0)), BW from Q, not peq():
   * kept so that PEQs saved with the legacy Plugins keep their sound.
   *
   * \param a Pointer to an array for storing the denominator coefficients.
   * \param b Pointer to an array for storing the numerator coefficients.
   * \param gain Gain of filter in dB.
   * \param fc Center frequency in Hz.
   * \param Q Q-factor (bandwidth) of filter.
   */
  static void makeParametricEQ( float a[], float b[], const float gain, const float fc, const float Q, const float fs );

  //============================================================================
  /*! Calculates the biquad coefficients for a low shelving filter.
   *
   * \param a Pointer to an array for storing the denominator coefficients.
   * \param b Pointer to an array for storing the numerator coefficients.
   * \param gain Gain of filter in dB.
   * \param fc Center frequency in Hz.
   * \param S Defines the slope.
   */
  static void makeLowShelv( float a[], float b[], const float gain, const float fc, const float S, const float fs );

  //============================================================================
  /*! Calculates the biquad coefficients for a high shelving filter.
   *
   * \param a Pointer to an array for storing the denominator coefficients.
   * \param b Pointer to an array for storing the numerator coefficients.
   * \param gain Gain of filter in dB.
   * \param fc Center frequency in Hz.
   * \param S Defines the slope.
   */
  static void makeHighShelv( float a[], float b[], const float gain, const float fc, const float S, const float fs );


  static void makeHighPass( float a[], float b[], const int design, const float fc, const float fs );
  static void makeLowPass( float a[], float b[], const int design, const float fc, const float fs );
  static void makeAllpass( float a[], float b[], const float fc, const float Q, const float fs );

  //============================================================================
  /*! Butterworth high/low pass of order 1..8 as ceil(order/2) cascaded biquads; the stages
   *  are 3 coefficients apart in a and b (a[0] = 1, a1/a2 negated). An odd order starts with
   *  a 1st-order section (b2 = a2 = 0). Returns the number of biquads.
   */
  static int makeButterworth( float a[], float b[], const bool high, const int order, const float fc, const float fs );

  /*! Linkwitz-Riley high/low pass of order 2, 4 or 8: two Butterworth filters of order/2 in
   *  series, order/2 biquads (stage layout as makeButterworth). Returns the number of biquads.
   */
  static int makeLinkwitzRiley( float a[], float b[], const bool high, const int order, const float fc, const float fs );
};

#endif
