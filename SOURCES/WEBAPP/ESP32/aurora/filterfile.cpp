#include <ctype.h>
#include <math.h>
#include <cmath>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <complex>

#include "filterfile.h"
#include "AudioFilterFactory.h"
#include "matrixlimits.h"

// Mirrors parse() of SOURCES/filterfile/aurorafilter.py step by step, so both report the same
// errors in the same order.

static const double kFs = MATRIX_SAMPLE_RATE;
static const double kPi = 3.14159265358979323846;
static const int kNameLen = MATRIX_NAME_LEN;  // bytes of UTF-8
static const double kChannelGainMin = MATRIX_GAIN_MIN_DB, kChannelGainMax = MATRIX_GAIN_MAX_DB;
static const double kTolDb = 0.01, kTolDeg = 0.1, kFloorDb = -80.0;
static const char* const kDesigns = "BW6, BW12, BW18, BW24, BW30, BW36, BW42, BW48, LR12, LR24, LR48";

const char* filterFileTypeName( FilterFileType type )
{
  static const char* const names[] = { "PEQ", "LSHELF", "HSHELF", "PHASE", "HP", "LP" };
  return names[type];
}

//==============================================================================
// Text: a token is a (pointer, length) slice of the file.

struct Tok
{
  const char* p;
  size_t n;
  std::string str( void ) const { return std::string( p, n ); }
};

/*! Bytes of the whitespace character at p (as Python's str.split() sees it), else 0. */
static size_t spaceAt( const char* p, const char* end )
{
  const unsigned char c = (unsigned char)*p;
  if( c == ' ' || ( c >= '\t' && c <= '\r' ) || ( c >= 0x1c && c <= 0x1f ) )
    return 1;
  if( c == 0xc2 && end - p >= 2 && ( (unsigned char)p[1] == 0x85 || (unsigned char)p[1] == 0xa0 ) )
    return 2;
  if( end - p < 3 )
    return 0;
  const unsigned long u = ( (unsigned long)c << 16 ) | ( (unsigned long)(unsigned char)p[1] << 8 ) | (unsigned char)p[2];
  if( u == 0xe19a80 || ( u >= 0xe28080 && u <= 0xe2808a ) || u == 0xe280a8 || u == 0xe280a9 || u == 0xe280af
      || u == 0xe2819f || u == 0xe38080 )  // U+1680, U+2000-200A, U+2028, U+2029, U+202F, U+205F, U+3000
    return 3;
  return 0;
}

static Tok strip( const char* p, const char* end )
{
  for( size_t k; p < end && ( k = spaceAt( p, end ) ) > 0; p += k ) {}
  while( p < end )
  {
    // the last character: step back over UTF-8 continuation bytes
    const char* q = end - 1;
    while( q > p && ( (unsigned char)*q & 0xc0 ) == 0x80 ) q--;
    if( spaceAt( q, end ) != (size_t)( end - q ) )
      break;
    end = q;
  }
  Tok t = { p, (size_t)( end - p ) };
  return t;
}

static void split( const Tok& line, std::vector<Tok>& toks )
{
  toks.clear();
  const char* p = line.p;
  const char* end = line.p + line.n;
  while( p < end )
  {
    size_t k;
    if( ( k = spaceAt( p, end ) ) > 0 ) { p += k; continue; }
    const char* start = p;
    while( p < end && spaceAt( p, end ) == 0 ) p++;
    Tok t = { start, (size_t)( p - start ) };
    toks.push_back( t );
  }
}

static bool is( const Tok& t, const char* word )  // ASCII case-insensitive
{
  const size_t n = strlen( word );
  if( t.n != n )
    return false;
  for( size_t i = 0; i < n; i++ )
    if( tolower( (unsigned char)t.p[i] ) != word[i] )
      return false;
  return true;
}

static std::string upper( const Tok& t )
{
  std::string s = t.str();
  for( size_t i = 0; i < s.size(); i++ ) s[i] = (char)toupper( (unsigned char)s[i] );
  return s;
}

/*! Digits with single underscores between them, as Python's float() takes them; appends the digits. */
static const char* digits( const char* p, const char* end, std::string& out )
{
  if( p >= end || !isdigit( (unsigned char)*p ) )
    return NULL;
  while( p < end )
  {
    if( isdigit( (unsigned char)*p ) ) out += *p++;
    else if( *p == '_' && p + 1 < end && isdigit( (unsigned char)p[1] ) ) p++;
    else break;
  }
  return p;
}

/*! A decimal number (point as separator, optional exponent) as Python's float() reads it, but no
 *  nan or inf (_num() of the reference). */
static bool number( const Tok& t, double& v )
{
  const char* p = t.p;
  const char* end = t.p + t.n;
  std::string s;
  if( p < end && ( *p == '+' || *p == '-' ) ) s += *p++;
  const char* q = digits( p, end, s );
  bool any = ( q != NULL );
  p = q ? q : p;
  if( p < end && *p == '.' )
  {
    s += *p++;
    q = digits( p, end, s );
    any = any || q;
    p = q ? q : p;
  }
  if( !any )
    return false;
  if( p < end && ( *p == 'e' || *p == 'E' ) )
  {
    s += *p++;
    if( p < end && ( *p == '+' || *p == '-' ) ) s += *p++;
    if( !( p = digits( p, end, s ) ) )
      return false;
  }
  if( p != end )
    return false;
  v = strtod( s.c_str(), NULL );
  return std::isfinite( v );
}

static bool validUtf8( const unsigned char* p, size_t n )
{
  for( size_t i = 0; i < n; )
  {
    const unsigned char c = p[i];
    size_t len;
    unsigned char lo = 0x80, hi = 0xbf;  // range of the second byte
    if( c < 0x80 ) { i++; continue; }
    else if( c >= 0xc2 && c <= 0xdf ) len = 2;
    else if( c >= 0xe0 && c <= 0xef ) { len = 3; if( c == 0xe0 ) lo = 0xa0; if( c == 0xed ) hi = 0x9f; }
    else if( c >= 0xf0 && c <= 0xf4 ) { len = 4; if( c == 0xf0 ) lo = 0x90; if( c == 0xf4 ) hi = 0x8f; }
    else return false;
    if( i + len > n || p[i + 1] < lo || p[i + 1] > hi )
      return false;
    for( size_t k = 2; k < len; k++ )
      if( ( p[i + k] & 0xc0 ) != 0x80 )
        return false;
    i += len;
  }
  return true;
}

/*! A file error that ends the parse: the only error listed. */
static std::vector<std::string> fatal( const std::string& msg )
{
  return std::vector<std::string>( 1, msg );
}

static std::string format( const char* fmt, ... )
{
  char buf[256];
  va_list ap;
  va_start( ap, fmt );
  vsnprintf( buf, sizeof( buf ), fmt, ap );
  va_end( ap );
  return buf;
}

//==============================================================================
// Verification

typedef AudioFilterFactory::Biquad Biquad;
typedef std::complex<double> Cplx;

static Cplx response( const Biquad* secs, int n, const Cplx& z1 )
{
  Cplx h( 1.0, 0.0 );
  for( int i = 0; i < n; i++ )
  {
    const Biquad& s = secs[i];
    h *= ( s.b0 + s.b1 * z1 + s.b2 * z1 * z1 ) / ( 1.0 + s.a1 * z1 + s.a2 * z1 * z1 );
  }
  return h;
}

static double dB( double x )
{
  return x > 0 ? 20.0 * log10( x ) : -INFINITY;
}

/*! The reference's deviation(): false if every grid point is within tolerance, else the worst. */
static bool deviation( const Biquad* file, const Biquad* ref, int n, double& worstF, double& worstDb, double& worstDeg )
{
  bool found = false;
  double score = 1.0;
  for( int k = 0;; k++ )
  {
    const double f = 10.0 * pow( 2.0, k / 24.0 );
    if( f >= kFs / 2.0 )
      return found;
    const Cplx z1 = std::polar( 1.0, -2.0 * kPi * f / kFs );
    const Cplx hr = response( ref, n, z1 ), hf = response( file, n, z1 );
    const double refDb = dB( std::abs( hr ) ), fileDb = dB( std::abs( hf ) );
    if( refDb < kFloorDb )
    {
      if( fileDb >= kFloorDb + kTolDb )
      {
        const double x = ( fileDb - refDb ) / kTolDb;
        if( x > score ) { found = true; score = x; worstF = f; worstDb = fileDb - refDb; worstDeg = 0.0; }
      }
      continue;
    }
    if( hf == Cplx( 0.0, 0.0 ) )
    {
      worstF = f; worstDb = -INFINITY; worstDeg = 0.0;
      return true;
    }
    const Cplx r = hf / hr;
    const double ddb = dB( std::abs( r ) ), ddeg = std::arg( r ) * ( 180.0 / kPi );
    const double x = fmax( fabs( ddb ) / kTolDb, fabs( ddeg ) / kTolDeg );
    if( x > score ) { found = true; score = x; worstF = f; worstDb = ddb; worstDeg = ddeg; }
  }
}

static int referenceSections( const FilterFileBlock& b, Biquad* out )
{
  typedef AudioFilterFactory F;
  switch( b.type )
  {
  case FILTERFILE_PEQ:    out[0] = F::peq( b.f, b.gain, b.q, kFs ); return 1;
  case FILTERFILE_LSHELF: out[0] = F::lowShelf( b.f, b.gain, b.s, kFs ); return 1;
  case FILTERFILE_HSHELF: out[0] = F::highShelf( b.f, b.gain, b.s, kFs ); return 1;
  case FILTERFILE_PHASE:  out[0] = F::phase( b.f, b.q, kFs ); return 1;
  default:
    return b.lr ? F::linkwitzRiley( out, b.type == FILTERFILE_HP, b.order, b.f, kFs )
                : F::butterworth( out, b.type == FILTERFILE_HP, b.order, b.f, kFs );
  }
}

static int sectionCount( const FilterFileBlock& b )
{
  if( b.type != FILTERFILE_HP && b.type != FILTERFILE_LP )
    return 1;
  return b.lr ? b.order / 2 : ( b.order + 1 ) / 2;
}

//==============================================================================
// Parsing

namespace {

struct Param
{
  const char* key;   // lower case, as in the file
  const char* name;  // in messages
  double lo, hi;
  double FilterFileBlock::* field;
};

const Param kF = { "f", "f", 20.0, 20000.0, &FilterFileBlock::f };
const Param kQ = { "q", "Q", 0.1, 20.0, &FilterFileBlock::q };
const Param kGain = { "gain", "gain", -24.0, 24.0, &FilterFileBlock::gain };
const Param kS = { "s", "S", 0.1, 1.0, &FilterFileBlock::s };

/*! The parameters of a type, in the reference's order (PARAMS). */
int paramsOf( FilterFileType t, const Param* out[3] )
{
  switch( t )
  {
  case FILTERFILE_PEQ:    out[0] = &kF; out[1] = &kGain; out[2] = &kQ; return 3;
  case FILTERFILE_LSHELF:
  case FILTERFILE_HSHELF: out[0] = &kF; out[1] = &kGain; out[2] = &kS; return 3;
  case FILTERFILE_PHASE:  out[0] = &kF; out[1] = &kQ; return 2;
  default:                out[0] = &kF; return 1;
  }
}

bool typeFromName( const std::string& s, FilterFileType& t )
{
  for( int k = FILTERFILE_PEQ; k <= FILTERFILE_LP; k++ )
    if( s == filterFileTypeName( (FilterFileType)k ) )
    {
      t = (FilterFileType)k;
      return true;
    }
  return false;
}

/*! "BW6".."BW48" (order = dB/oct / 6) or "LR12", "LR24", "LR48". */
bool designFromName( const std::string& d, bool& lr, int& order )
{
  static const char* const bw[] = { "BW6", "BW12", "BW18", "BW24", "BW30", "BW36", "BW42", "BW48" };
  static const char* const lrs[] = { "LR12", "LR24", "LR48" };
  for( int k = 0; k < 8; k++ )
    if( d == bw[k] ) { lr = false; order = k + 1; return true; }
  for( int k = 0; k < 3; k++ )
    if( d == lrs[k] ) { lr = true; order = 2 << k; return true; }
  return false;
}

class Parser
{
public:
  Parser( const FilterFileLimits& limits, FilterFileRow& row ) : lim_( limits ), row_( row ) {}
  std::vector<std::string> run( const char* text, size_t len );

private:
  void err( int line, const std::string& msg );
  void closeBlock( void );
  void verify( const FilterFileBlock& b );
  bool channelLine( int n, const std::string& key, const std::vector<Tok>& toks, const Tok& rest );
  void blockLine( int n, const std::vector<Tok>& toks );

  const FilterFileLimits& lim_;
  FilterFileRow& row_;
  std::vector<std::string> errors_;
  std::vector<std::string> deviations_;  // reported after the parse, as the reference does
  bool seen_[5] = { false, false, false, false, false };  // fs, name, gain, delay, polarity
  int used_ = 0;      // biquads of all blocks so far

  // the block whose coefficient lines are being read
  bool open_ = false;
  bool openOk_ = false;
  FilterFileBlock block_;
  std::string label_;
  int coefCount_ = 0;
  Biquad coefs_[4];
};

void Parser::err( int line, const std::string& msg )
{
  if( errors_.size() < FILTERFILE_MAX_ERRORS )
    errors_.push_back( line ? format( "line %d: ", line ) + msg : msg );
}

void Parser::closeBlock( void )
{
  if( !open_ )
    return;
  open_ = false;
  const int count = sectionCount( block_ );
  if( coefCount_ != count )
    err( block_.line, format( "%s needs %d coefficient line%s, found %d", label_.c_str(), count, count == 1 ? "" : "s",
                              coefCount_ ) );
  else if( openOk_ )
    verify( block_ );
}

/*! Compares the block's coefficients with Aurora's. Skipped where the result cannot change the
 *  outcome: beyond the Biquad budget (rejected anyway) or once no further error can be listed. */
void Parser::verify( const FilterFileBlock& b )
{
  if( ( lim_.biquads >= 0 && used_ > lim_.biquads ) || errors_.size() >= FILTERFILE_MAX_ERRORS
      || deviations_.size() >= FILTERFILE_MAX_ERRORS )
    return;
  Biquad ref[4];
  const int n = referenceSections( b, ref );
  double f, ddb, ddeg;
  if( deviation( coefs_, ref, n, f, ddb, ddeg ) )
    deviations_.push_back( format( "line %d: %s deviates from the reference by %.3g dB / %.3g deg at %.0f Hz", b.line,
                                   label_.c_str(), ddb, ddeg, f ) );
}

bool Parser::channelLine( int n, const std::string& key, const std::vector<Tok>& toks, const Tok& rest )
{
  static const char* const keys[] = { "name", "gain", "delay", "polarity" };
  int k = 0;
  while( k < 4 && key != keys[k] ) k++;
  if( k == 4 )
    return false;
  if( seen_[1 + k] )
  {
    err( n, key + " given twice" );
    return true;
  }
  seen_[1 + k] = true;
  double v;
  if( key == "name" )
  {
    bool control = false;
    for( size_t i = 0; i < rest.n; i++ )
      control = control || (unsigned char)rest.p[i] < 32 || rest.p[i] == 127;
    if( rest.n == 0 )
      err( n, "name is empty (leave the line out to keep the current name)" );
    else if( rest.n > (size_t)kNameLen )
      err( n, format( "name longer than %d bytes", kNameLen ) );
    else if( control )
      err( n, "name contains control characters" );
    else
    {
      row_.hasName = true;
      row_.name = rest.str();
    }
  }
  else if( key == "gain" )
  {
    if( !( toks.size() == 3 && is( toks[2], "db" ) && number( toks[1], v ) ) )
      err( n, "gain is written \"gain <number> dB\"" );
    else if( !( v >= kChannelGainMin && v <= kChannelGainMax ) )
      err( n, format( "gain %.6g dB outside %g to %g dB", v, kChannelGainMin, kChannelGainMax ) );
    else
      row_.gain = v;
  }
  else if( key == "delay" )
  {
    const bool ok = toks.size() == 3 && is( toks[2], "ms" ) && number( toks[1], v );
    if( lim_.bus )
      err( n, "a Bus has no delay" );
    else if( !ok )
      err( n, "delay is written \"delay <number> ms\"" );
    else
    {
      const double samples = v >= 0 ? floor( v * kFs / 1000.0 + 0.5 ) : -1.0;
      if( samples < 0 || samples > lim_.maxDelaySamples )
        err( n, format( "delay %.6g ms outside 0 to %g ms", v, lim_.maxDelaySamples * 1000.0 / kFs ) );
      else
        row_.delay = (int)samples;
    }
  }
  else
  {
    if( lim_.bus )
      err( n, "a Bus has no polarity" );
    else if( is( rest, "normal" ) || is( rest, "inverted" ) )
      row_.inverted = is( rest, "inverted" );
    else
      err( n, "polarity is \"normal\" or \"inverted\"" );
  }
  return true;
}

void Parser::blockLine( int n, const std::vector<Tok>& toks )
{
  FilterFileBlock b = { n, FILTERFILE_PEQ, false, 2, 0.0, 0.0, 0.0, 0.0, false };
  if( !typeFromName( upper( toks[0] ), b.type ) )
  {
    err( n, "unknown line \"" + toks[0].str() + "\"" );
    return;
  }
  size_t first = 1;
  std::string label = filterFileTypeName( b.type );
  if( b.type == FILTERFILE_HP || b.type == FILTERFILE_LP )
  {
    const std::string d = toks.size() > 1 ? upper( toks[1] ) : "";
    if( !designFromName( d, b.lr, b.order ) )
    {
      if( toks.size() > 1 )
        err( n, label + ": unknown design \"" + toks[1].str() + "\" (" + kDesigns + ")" );
      else
        err( n, label + " needs a design (" + kDesigns + ")" );
      return;
    }
    label += " " + d;
    first = 2;
  }
  const Param* params[3];
  const int np = paramsOf( b.type, params );
  bool given[3] = { false, false, false };
  bool ok = true;
  for( size_t i = first; i < toks.size(); i++ )
  {
    const Tok& a = toks[i];
    if( is( a, "bypass" ) )
    {
      if( b.bypass )
      {
        err( n, "bypass given twice" );
        ok = false;
      }
      b.bypass = true;
      continue;
    }
    const char* eq = (const char*)memchr( a.p, '=', a.n );
    const Tok key = { a.p, eq ? (size_t)( eq - a.p ) : a.n };
    int k = 0;
    while( k < np && !is( key, params[k]->key ) ) k++;
    if( !eq || k == np )
    {
      err( n, label + " does not take \"" + a.str() + "\"" );
      ok = false;
      continue;
    }
    const Param& p = *params[k];
    if( given[k] )
    {
      err( n, format( "%s given twice", p.name ) );
      ok = false;
      continue;
    }
    given[k] = true;
    const Tok val = { eq + 1, (size_t)( a.p + a.n - eq - 1 ) };
    double x;
    if( !number( val, x ) )
    {
      err( n, format( "%s is not a number", p.name ) );
      ok = false;
    }
    else if( !( x >= p.lo && x <= p.hi ) )
    {
      err( n, format( "%s %.6g outside %g to %g", p.name, x, p.lo, p.hi ) );
      ok = false;
    }
    else
      b.*( p.field ) = x;
  }
  std::string missing;
  for( int k = 0; k < np; k++ )
    if( !given[k] )
      missing += ( missing.empty() ? "" : ", " ) + std::string( params[k]->name );
  if( !missing.empty() )
  {
    err( n, label + " needs " + missing );
    ok = false;
  }
  used_ += sectionCount( b );
  if( lim_.biquads < 0 || used_ <= lim_.biquads )
    row_.blocks.push_back( b );  // beyond the budget the file is rejected: no need to keep it
  open_ = true;
  openOk_ = ok;
  block_ = b;
  label_ = label;
  coefCount_ = 0;
}

std::vector<std::string> Parser::run( const char* text, size_t len )
{
  if( len > FILTERFILE_MAX_BYTES )
    return fatal( FILTERFILE_TOO_LARGE );
  if( !validUtf8( (const unsigned char*)text, len ) )
    return fatal( "file is not UTF-8" );
  if( len >= 3 && memcmp( text, "\xef\xbb\xbf", 3 ) == 0 )
  {
    text += 3;
    len -= 3;
  }
  row_.hasName = false;
  row_.name.clear();
  row_.gain = 0.0;
  row_.delay = 0;
  row_.inverted = false;
  row_.blocks.clear();

  const char* const end = text + len;
  bool header = false, haveFs = false;
  std::vector<Tok> toks;
  int n = 0;
  for( const char* p = text; p <= end; n++ )
  {
    const char* eol = (const char*)memchr( p, '\n', end - p );
    if( !eol ) eol = end;
    const Tok line = strip( p, eol );
    p = eol + 1;
    const int lineNo = n + 1;
    if( line.n == 0 || line.p[0] == '#' )
      continue;
    split( line, toks );
    if( !header )
    {
      if( toks.size() != 2 || !is( toks[0], "aurora-filter" ) || toks[1].n != 1 || toks[1].p[0] != '1' )
      {
        if( is( toks[0], "aurora-filter" ) )
        {
          std::string v;
          for( size_t i = 1; i < toks.size(); i++ ) v += ( i > 1 ? " " : "" ) + toks[i].str();
          return fatal( format( "line %d: unsupported version %s (this is version 1)", lineNo,
                                v.empty() ? "(none)" : v.c_str() ) );
        }
        return fatal( format( "line %d: the file must start with \"aurora-filter 1\"", lineNo ) );
      }
      header = true;
      continue;
    }

    double v;
    if( number( toks[0], v ) )  // coefficient line
    {
      if( !open_ )
      {
        err( lineNo, "coefficients without a filter block" );
        continue;
      }
      double c[5];
      bool ok = toks.size() == 5;
      for( size_t i = 0; ok && i < 5; i++ ) ok = number( toks[i], c[i] );
      if( !ok )
        err( lineNo, "a coefficient line has 5 numbers: b0 b1 b2 a1 a2" );
      else
      {
        if( coefCount_ < 4 )
        {
          Biquad s = { c[0], c[1], c[2], c[3], c[4] };
          coefs_[coefCount_] = s;
        }
        coefCount_++;  // too many or too few: closeBlock()
      }
      continue;
    }

    closeBlock();
    std::string key = toks[0].str();
    for( size_t i = 0; i < key.size(); i++ ) key[i] = (char)tolower( (unsigned char)key[i] );
    const Tok rest = strip( toks[0].p + toks[0].n, line.p + line.n );

    if( key == "fs" )
    {
      if( seen_[0] )
        err( lineNo, "fs given twice" );
      seen_[0] = true;
      if( !( toks.size() == 2 && number( rest, v ) && v == kFs ) )
        err( lineNo, "fs must be 48000" );
      haveFs = true;
      continue;
    }
    if( !haveFs )
    {
      err( lineNo, "\"fs 48000\" must come right after the header" );
      haveFs = true;  // report once
    }
    if( !channelLine( lineNo, key, toks, rest ) )
      blockLine( lineNo, toks );
  }
  closeBlock();

  if( !header )
    return fatal( "the file must start with \"aurora-filter 1\"" );
  if( !haveFs )
    err( 0, "missing \"fs 48000\"" );
  for( size_t i = 0; i < deviations_.size(); i++ )
    err( 0, deviations_[i] );
  if( lim_.biquads >= 0 && used_ > lim_.biquads )
    err( 0, format( "the filters need %d biquads, the Channel has %d", used_, lim_.biquads ) );
  return errors_;
}

}  // namespace

std::vector<std::string> parseFilterFile( const char* text, size_t len, const FilterFileLimits& limits, FilterFileRow& row )
{
  Parser parser( limits, row );
  return parser.run( text, len );
}

//==============================================================================
// Writing

std::string filterFileDecimal( double v, bool singlePrecision )
{
  const float f = (float)v;
  char buf[64];
  for( int decimals = 0; decimals <= 17; decimals++ )
  {
    snprintf( buf, sizeof( buf ), "%.*f", decimals, v );
    if( singlePrecision ? strtof( buf, NULL ) == f : strtod( buf, NULL ) == v )
      return buf;
  }
  snprintf( buf, sizeof( buf ), "%.17g", v );
  return buf;
}

static std::string shortest( double v )
{
  return filterFileDecimal( v, false );
}

static bool nameFits( const std::string& name )
{
  const char* p = name.data();
  const Tok t = strip( p, p + name.size() );
  if( name.empty() || name.size() > (size_t)kNameLen || t.n != name.size()
      || !validUtf8( (const unsigned char*)p, name.size() ) )
    return false;
  for( size_t i = 0; i < name.size(); i++ )
    if( (unsigned char)p[i] < 32 || p[i] == 127 )
      return false;
  return true;
}

std::string writeFilterFile( const FilterFileRow& row, bool bus )
{
  std::string s = "aurora-filter 1\nfs 48000\n";
  if( row.hasName && nameFits( row.name ) )
    s += "name " + row.name + "\n";
  s += "gain " + shortest( row.gain ) + " dB\n";
  if( !bus )
  {
    s += "delay " + shortest( row.delay / ( kFs / 1000.0 ) ) + " ms\n";
    s += row.inverted ? "polarity inverted\n" : "polarity normal\n";
  }
  for( size_t i = 0; i < row.blocks.size(); i++ )
  {
    const FilterFileBlock& b = row.blocks[i];
    s += filterFileTypeName( b.type );
    if( b.type == FILTERFILE_HP || b.type == FILTERFILE_LP )
      s += format( " %s%d", b.lr ? "LR" : "BW", 6 * b.order );
    s += " f=" + shortest( b.f );
    if( b.type == FILTERFILE_PEQ || b.type == FILTERFILE_LSHELF || b.type == FILTERFILE_HSHELF )
      s += " gain=" + shortest( b.gain );
    if( b.type == FILTERFILE_PEQ || b.type == FILTERFILE_PHASE )
      s += " Q=" + shortest( b.q );
    if( b.type == FILTERFILE_LSHELF || b.type == FILTERFILE_HSHELF )
      s += " S=" + shortest( b.s );
    s += b.bypass ? " bypass\n" : "\n";
    Biquad secs[4];
    const int n = referenceSections( b, secs );
    for( int k = 0; k < n; k++ )
      s += format( "  %.17g %.17g %.17g %.17g %.17g\n", secs[k].b0, secs[k].b1, secs[k].b2, secs[k].a1, secs[k].a2 );
  }
  return s;
}

std::string filterFileErrorsJson( const std::vector<std::string>& errors )
{
  DynamicJsonDocument doc( JSON_OBJECT_SIZE( 1 ) + JSON_ARRAY_SIZE( errors.size() ) );
  JsonArray list = doc.createNestedArray( "errors" );
  for( size_t i = 0; i < errors.size(); i++ ) list.add( errors[i].c_str() );  // by reference: errors outlives doc
  std::string s( measureJson( doc ), 0 );
  serializeJson( doc, &s[0], s.size() + 1 );
  return s;
}

//==============================================================================
// The patch

static const int kSlotFields = 8;  // type, design, order, freq, q, gain, slope, bypass

size_t filterFilePatchCapacity( const FilterFileRow& row )
{
  return JSON_OBJECT_SIZE( 12 ) + JSON_ARRAY_SIZE( row.blocks.size() ) + row.blocks.size() * JSON_OBJECT_SIZE( kSlotFields );
}

bool filterFilePatch( const FilterFileRow& row, bool bus, JsonObject patch )
{
  static const char* const types[] = { "peq", "lshelf", "hshelf", "phase", "hp", "lp" };
  if( row.hasName )
    patch["name"] = row.name.c_str();  // by reference: row outlives the patch
  patch["gain"] = row.gain;
  patch["gainBypass"] = false;
  if( !bus )
  {
    patch["polarity"] = row.inverted;
    patch["delayUs"] = row.delay * 1e6 / kFs;  // the model rounds back to the same samples
    patch["delayBypass"] = false;
  }
  JsonArray slots = patch.createNestedArray( "slots" );
  for( size_t i = 0; i < row.blocks.size(); i++ )
  {
    const FilterFileBlock& b = row.blocks[i];
    JsonObject s = slots.createNestedObject();
    s["type"] = types[b.type];
    if( b.type == FILTERFILE_HP || b.type == FILTERFILE_LP )
    {
      s["design"] = b.lr ? "lr" : "bw";
      s["order"] = b.order;
    }
    s["freq"] = b.f;
    if( b.type == FILTERFILE_PEQ || b.type == FILTERFILE_PHASE )
      s["q"] = b.q;
    if( b.type == FILTERFILE_PEQ || b.type == FILTERFILE_LSHELF || b.type == FILTERFILE_HSHELF )
      s["gain"] = b.gain;
    if( b.type == FILTERFILE_LSHELF || b.type == FILTERFILE_HSHELF )
      s["slope"] = b.s;
    s["bypass"] = b.bypass;
  }
  // the last field of the last Slot is there only if the document was large enough
  const size_t n = row.blocks.size();
  return patch.containsKey( "slots" ) && slots.size() == n && ( n == 0 || slots[n - 1].containsKey( "bypass" ) )
         && ( !row.hasName || patch.containsKey( "name" ) );
}
