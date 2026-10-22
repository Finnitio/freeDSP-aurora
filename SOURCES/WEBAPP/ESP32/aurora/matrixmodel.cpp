#include <math.h>
#include <string.h>
#include <stdio.h>

#include "matrixmodel.h"

static const int kPresetVersion = 1;
static const float kGainMin = -80.0f;
static const float kGainMax = 12.0f;
static const int kNumRoutes = 3;  // Bus 1, Bus 2, Bus 1+2
static const uint32_t kPortAnalog = 0, kPortUsb = 1, kPortExp = 2, kPortSpdif = 4;

//==============================================================================
/*! Selectable Sources: Analog 1-8, USB 1-8, Exp 1-8, SPDIF L/R. The ESP32 mux
 *  group exists on the DSP but is not offered.
 */
static uint32_t sourceCode( int n )
{
  if( n < 8 )  return ( kPortAnalog << 16 ) | n;
  if( n < 16 ) return ( kPortUsb << 16 ) | ( n - 8 );
  if( n < 24 ) return ( kPortExp << 16 ) | ( n - 16 );
  return ( kPortSpdif << 16 ) | ( n - 24 );
}
static const int kNumSources = 26;

static bool sourceValid( long sel )
{
  for( int n = 0; n < kNumSources; n++ )
    if( (long)sourceCode( n ) == sel )
      return true;
  return false;
}

static std::string sourceName( int n )
{
  char buf[16];
  if( n < 8 )       snprintf( buf, sizeof( buf ), "Analog %d", n + 1 );
  else if( n < 16 ) snprintf( buf, sizeof( buf ), "USB %d", n - 7 );
  else if( n < 24 ) snprintf( buf, sizeof( buf ), "Exp %d", n - 15 );
  else              snprintf( buf, sizeof( buf ), n == 24 ? "SPDIF L" : "SPDIF R" );
  return buf;
}

//==============================================================================
MatrixModel::MatrixModel( MatrixPort& port, int nbus, int nout )
  : port_( port ), nbus_( nbus < MATRIX_MAX_BUS ? nbus : MATRIX_MAX_BUS ),
    nout_( nout < MATRIX_MAX_OUT ? nout : MATRIX_MAX_OUT ), dirty_( false ), lastChange_( 0 )
{
  reset();
}

void MatrixModel::reset( void )
{
  for( int b = 0; b < nbus_; b++ )
  {
    char buf[16];
    snprintf( buf, sizeof( buf ), "Bus %d", b + 1 );
    bus_[b].name = buf;
    bus_[b].mute = false;
    bus_[b].gain = 0.0f;
    bus_[b].sel = (int)sourceCode( b );  // Analog 1, Analog 2
  }
  for( int o = 0; o < nout_; o++ )
  {
    char buf[16];
    snprintf( buf, sizeof( buf ), "Output %d", o + 1 );
    out_[o].name = buf;
    out_[o].mute = true;
    out_[o].gain = 0.0f;
    out_[o].sel = o % 2;
  }
}

void MatrixModel::loadDefaults( void )
{
  reset();
  dirty_ = false;
  applyAll();
}

double MatrixModel::linear( const Channel& c ) const
{
  return c.mute ? 0.0 : pow( 10.0, c.gain / 20.0 );
}

//==============================================================================
std::string MatrixModel::apply( JsonObject patch, uint32_t nowMs )
{
  const char* kind = patch["ch"].as<const char*>();
  if( !kind )
    return "missing ch";
  const bool isBus = ( strcmp( kind, "bus" ) == 0 );
  if( !isBus && strcmp( kind, "out" ) != 0 )
    return "unknown channel kind";
  if( !patch["idx"].is<int>() )
    return "missing idx";
  const int idx = patch["idx"].as<int>();
  if( idx < 0 || idx >= ( isBus ? nbus_ : nout_ ) )
    return "no such channel";

  Channel next = isBus ? bus_[idx] : out_[idx];
  const Channel prev = next;

  for( JsonPair kv : patch )
  {
    const char* key = kv.key().c_str();
    JsonVariant v = kv.value();
    if( strcmp( key, "ch" ) == 0 || strcmp( key, "idx" ) == 0 )
      continue;
    if( strcmp( key, "name" ) == 0 )
    {
      const char* s = v.as<const char*>();
      if( !s )
        return "name must be a string";
      if( strlen( s ) > MATRIX_NAME_LEN )
        return "name too long";
      next.name = s;
    }
    else if( strcmp( key, "mute" ) == 0 )
    {
      if( !v.is<bool>() )
        return "mute must be true or false";
      next.mute = v.as<bool>();
    }
    else if( strcmp( key, "gain" ) == 0 )
    {
      if( !v.is<float>() )
        return "gain must be a number";
      const float g = v.as<float>();
      if( !( g >= kGainMin && g <= kGainMax ) )
        return "gain out of range";
      next.gain = g;
    }
    else if( strcmp( key, "source" ) == 0 && isBus )
    {
      if( !v.is<long>() || !sourceValid( v.as<long>() ) )
        return "no such source";
      next.sel = (int)v.as<long>();
    }
    else if( strcmp( key, "route" ) == 0 && !isBus )
    {
      if( !v.is<int>() || v.as<int>() < 0 || v.as<int>() >= kNumRoutes )
        return "no such route";
      next.sel = v.as<int>();
    }
    else
      return std::string( "unknown field " ) + key;
  }

  (isBus ? bus_[idx] : out_[idx]) = next;
  const MatrixChannelKind ck = isBus ? MATRIX_BUS : MATRIX_OUT;
  const double lin = linear( next );
  const bool switched = ( next.sel != prev.sel );

  if( switched && lin != 0.0 )
  {
    // ramp this Channel down, switch, ramp up; the others keep playing
    port_.writeGain( ck, idx, 0.0 );
    if( isBus ) port_.writeSource( idx, (uint32_t)next.sel ); else port_.writeRoute( idx, next.sel );
    port_.writeGain( ck, idx, lin );
  }
  else
  {
    if( lin != linear( prev ) )
      port_.writeGain( ck, idx, lin );
    if( switched )
    {
      if( isBus ) port_.writeSource( idx, (uint32_t)next.sel ); else port_.writeRoute( idx, next.sel );
    }
  }

  if( next.name != prev.name || next.mute != prev.mute || next.gain != prev.gain || switched )
  {
    dirty_ = true;
    lastChange_ = nowMs;
  }
  return "";
}

//==============================================================================
void MatrixModel::applyAll( void )
{
  for( int b = 0; b < nbus_; b++ )
  {
    port_.writeSource( b, (uint32_t)bus_[b].sel );
    port_.writeGain( MATRIX_BUS, b, linear( bus_[b] ) );
  }
  for( int o = 0; o < nout_; o++ )
  {
    port_.writeRoute( o, out_[o].sel );
    port_.writeGain( MATRIX_OUT, o, linear( out_[o] ) );
  }
}

//==============================================================================
static void putChannel( JsonArray arr, const char* name, bool mute, float gain, const char* selKey, int sel )
{
  JsonObject o = arr.createNestedObject();
  o["name"] = name;
  o[selKey] = sel;
  o["mute"] = mute;
  o["gain"] = gain;
}

static std::string toString( const JsonDocument& doc )
{
  std::string s( measureJson( doc ), 0 );
  serializeJson( doc, &s[0], s.size() + 1 );
  return s;
}

std::string MatrixModel::stateJson( void ) const
{
  DynamicJsonDocument doc( 8192 );
  doc["nbus"] = nbus_;
  doc["nout"] = nout_;
  doc["gainmin"] = kGainMin;
  doc["gainmax"] = kGainMax;
  JsonArray srcs = doc.createNestedArray( "sources" );
  std::string names[kNumSources];
  for( int n = 0; n < kNumSources; n++ )
  {
    names[n] = sourceName( n );
    JsonObject s = srcs.createNestedObject();
    s["n"] = names[n].c_str();
    s["v"] = (long)sourceCode( n );
  }
  JsonArray routes = doc.createNestedArray( "routes" );
  routes.add( "Bus 1" );
  routes.add( "Bus 2" );
  routes.add( "Bus 1+2" );
  JsonArray buses = doc.createNestedArray( "buses" );
  for( int b = 0; b < nbus_; b++ )
    putChannel( buses, bus_[b].name.c_str(), bus_[b].mute, bus_[b].gain, "source", bus_[b].sel );
  JsonArray outs = doc.createNestedArray( "outs" );
  for( int o = 0; o < nout_; o++ )
    putChannel( outs, out_[o].name.c_str(), out_[o].mute, out_[o].gain, "route", out_[o].sel );
  return toString( doc );
}

std::string MatrixModel::savePreset( void ) const
{
  DynamicJsonDocument doc( 4096 );
  doc["v"] = kPresetVersion;
  JsonArray buses = doc.createNestedArray( "buses" );
  for( int b = 0; b < nbus_; b++ )
    putChannel( buses, bus_[b].name.c_str(), bus_[b].mute, bus_[b].gain, "source", bus_[b].sel );
  JsonArray outs = doc.createNestedArray( "outs" );
  for( int o = 0; o < nout_; o++ )
    putChannel( outs, out_[o].name.c_str(), out_[o].mute, out_[o].gain, "route", out_[o].sel );
  return toString( doc );
}

static std::string readChannel( JsonObject o, bool isBus, std::string& name,
                                bool& mute, float& gain, int& sel )
{
  const char* n = o["name"].as<const char*>();
  if( !n || strlen( n ) > MATRIX_NAME_LEN )
    return "bad name";
  if( !o["mute"].is<bool>() )
    return "bad mute";
  if( !o["gain"].is<float>() || !( o["gain"].as<float>() >= kGainMin && o["gain"].as<float>() <= kGainMax ) )
    return "bad gain";
  if( isBus )
  {
    if( !o["source"].is<long>() || !sourceValid( o["source"].as<long>() ) )
      return "bad source";
    sel = (int)o["source"].as<long>();
  }
  else
  {
    if( !o["route"].is<int>() || o["route"].as<int>() < 0 || o["route"].as<int>() >= kNumRoutes )
      return "bad route";
    sel = o["route"].as<int>();
  }
  name = n;
  mute = o["mute"].as<bool>();
  gain = o["gain"].as<float>();
  return "";
}

std::string MatrixModel::loadPreset( const std::string& json )
{
  DynamicJsonDocument doc( 4096 );
  if( deserializeJson( doc, json.c_str() ) )
    return "preset is not valid JSON";
  if( doc["v"] != kPresetVersion )
    return "unknown preset version";
  JsonArray buses = doc["buses"];
  JsonArray outs = doc["outs"];
  if( (int)buses.size() != nbus_ || (int)outs.size() != nout_ )
    return "preset has the wrong channel count";

  Channel nb[MATRIX_MAX_BUS], no[MATRIX_MAX_OUT];
  for( int b = 0; b < nbus_; b++ )
  {
    std::string err = readChannel( buses[b], true, nb[b].name, nb[b].mute, nb[b].gain, nb[b].sel );
    if( !err.empty() )
      return err;
  }
  for( int o = 0; o < nout_; o++ )
  {
    std::string err = readChannel( outs[o], false, no[o].name, no[o].mute, no[o].gain, no[o].sel );
    if( !err.empty() )
      return err;
  }
  for( int b = 0; b < nbus_; b++ ) bus_[b] = nb[b];
  for( int o = 0; o < nout_; o++ ) out_[o] = no[o];
  dirty_ = false;
  applyAll();
  return "";
}

//==============================================================================
bool MatrixModel::saveDue( uint32_t nowMs ) const
{
  return dirty_ && (uint32_t)( nowMs - lastChange_ ) >= MATRIX_SAVE_DELAY_MS;
}

void MatrixModel::markSaved( void )
{
  dirty_ = false;
}
