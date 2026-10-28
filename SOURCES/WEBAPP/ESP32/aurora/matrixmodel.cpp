#include <math.h>
#include <string.h>
#include <stdio.h>

#include "matrixmodel.h"
#include "AudioFilterFactory.h"

static std::string toString( const JsonDocument& doc );

static const int kPresetVersion = 3;
static const float kSampleRate = 48000.0f;
static const float kFreqMin = 20.0f, kFreqMax = 20000.0f;
static const float kQMin = 0.1f, kQMax = 20.0f;
static const float kPeqGainMin = -24.0f, kPeqGainMax = 24.0f;
static const float kSlopeMin = 0.1f, kSlopeMax = 1.0f;
static const float kGainMin = -80.0f;
static const float kGainMax = 12.0f;
static const int kNumRoutes = 3;  // Bus 1, Bus 2, Bus 1+2
static const uint32_t kPortAnalog = 0, kPortUsb = 1, kPortExp = 2, kPortSpdif = 4, kPortOut = 5;

//==============================================================================
/*! Link group of an id within a Channel's scope: "" = -1 (none), "bus" = 0 for Buses, "A".."D" = 0..3
 *  for Outputs, -2 for anything else. */
static int linkFromId( const char* id, bool isBus )
{
  if( id[0] == 0 )
    return -1;
  if( isBus )
    return strcmp( id, "bus" ) == 0 ? 0 : -2;
  return strlen( id ) == 1 && id[0] >= 'A' && id[0] < 'A' + MATRIX_OUT_LINKS ? id[0] - 'A' : -2;
}

/*! A Link group named on its own ("bus", "A".."D"); found is false for anything else, NULL
 *  included. index is the Link group within its scope (always 0 for the Buses' one). */
struct LinkGroupId
{
  bool found;
  bool isBus;
  int index;
};
static LinkGroupId linkGroupFromId( const char* id )
{
  const LinkGroupId none = { false, false, 0 };
  if( !id || id[0] == 0 ) return none;
  if( strcmp( id, "bus" ) == 0 ) return { true, true, 0 };
  const int out = linkFromId( id, false );
  if( out < 0 ) return none;
  return { true, false, out };
}

/*! The id of a Link group, inverse of linkFromId(); buf holds at least 4 chars. Returns a
 *  non-const char* so ArduinoJson copies it. */
static char* linkId( char* buf, int link, bool isBus )
{
  if( link < 0 ) buf[0] = 0;
  else if( isBus ) strcpy( buf, "bus" );
  else { buf[0] = (char)( 'A' + link ); buf[1] = 0; }
  return buf;
}

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
/*! SPDIF out pairs: Analog, USB and Exp pairs 1+2..7+8, SPDIF in, Output pairs. The code is
 *  that of the left channel; the right one is the next channel of the same port.
 */
static const int kNumSpdifSrcPairs = 13;

static uint32_t spdifPairCode( int n )
{
  if( n < 4 )  return ( kPortAnalog << 16 ) | ( 2 * n );
  if( n < 8 )  return ( kPortUsb << 16 ) | ( 2 * ( n - 4 ) );
  if( n < 12 ) return ( kPortExp << 16 ) | ( 2 * ( n - 8 ) );
  if( n < 13 ) return ( kPortSpdif << 16 );
  return ( kPortOut << 16 ) | ( 2 * ( n - 13 ) );
}

static std::string spdifPairName( int n )
{
  char buf[16];
  if( n < 12 )      snprintf( buf, sizeof( buf ), "%s %d+%d", n < 4 ? "Analog" : n < 8 ? "USB" : "Exp", 2 * ( n % 4 ) + 1, 2 * ( n % 4 ) + 2 );
  else if( n < 13 ) snprintf( buf, sizeof( buf ), "SPDIF In" );
  else              snprintf( buf, sizeof( buf ), "Output %d+%d", 2 * ( n - 13 ) + 1, 2 * ( n - 13 ) + 2 );
  return buf;
}

static bool spdifPairValid( long sel, int nout )
{
  for( int n = 0; n < kNumSpdifSrcPairs + nout / 2; n++ )
    if( (long)spdifPairCode( n ) == sel )
      return true;
  return false;
}

//==============================================================================
int MatrixModel::slotCount( void ) const
{
  int n = 0;
  for( int b = 0; b < nbus_; b++ ) n += (int)bus_[b].slots.size();
  for( int o = 0; o < nout_; o++ ) n += (int)out_[o].slots.size();
  return n;
}

//==============================================================================
static MatrixCoef passthroughCoef( void )
{
  MatrixCoef c ={ { 1, 0, 0 }, { 1, 0, 0 } };
  return c;
}

//==============================================================================
static const char* const kTypeNames[] = { "peq", "hp", "lp", "lshelf", "hshelf", "phase" };
static const int kNumTypes = 6;
static const int kMaxOrder = 8;

/*! Index of a type name, -1 for NULL or an unknown name. */
static int typeFromName( const char* name )
{
  for( int k = 0; name && k < kNumTypes; k++ )
    if( strcmp( name, kTypeNames[k] ) == 0 )
      return k;
  return -1;
}

/*! Butterworth order n = ceil(n/2) biquads, Linkwitz-Riley order n = n/2, the rest 1. */
int MatrixModel::cost( const Slot& s )
{
  if( s.type != SLOT_HP && s.type != SLOT_LP )
    return 1;
  return s.lr ? s.order / 2 : ( s.order + 1 ) / 2;
}

static bool orderValid( bool lr, int order )
{
  return lr ? ( order == 2 || order == 4 || order == 8 ) : ( order >= 1 && order <= kMaxOrder );
}

/*! The selectable types and orders: PEQ, low shelf, high shelf, Phase, then HP and LP with
 *  Butterworth 1-8 and Linkwitz-Riley 2, 4, 8. */
int MatrixModel::choiceCount( void )
{
  return 4 + 2 * ( kMaxOrder + 3 );
}

MatrixModel::Slot MatrixModel::choice( int n )
{
  Slot s = { false, SLOT_PEQ, false, 2, 1000.0f, 1.0f, 0.0f, 1.0f };
  if( n < 4 )
  {
    static const SlotType t[4] = { SLOT_PEQ, SLOT_LSHELF, SLOT_HSHELF, SLOT_PHASE };
    s.type = t[n];
    return s;
  }
  n -= 4;
  s.type = n < kMaxOrder + 3 ? SLOT_HP : SLOT_LP;
  n %= kMaxOrder + 3;
  s.lr = ( n >= kMaxOrder );
  s.order = s.lr ? 2 << ( n - kMaxOrder ) : n + 1;
  return s;
}

/*! Shared final Slot rules, after either a partial edit or a complete preset Slot
 *  has been parsed. Non-crossover design/order are compatibility input and are
 *  normalized away; the returned field names let the live editor keep its
 *  specific rejection reasons while preset loading keeps its single reason. */
/*! The first of freq, q, gain, slope outside its range, else NULL. */
const char* MatrixModel::paramOutOfRange( const Slot& s )
{
  if( !( s.freq >= kFreqMin && s.freq <= kFreqMax ) )
    return "freq";
  if( !( s.q >= kQMin && s.q <= kQMax ) )
    return "q";
  if( !( s.gain >= kPeqGainMin && s.gain <= kPeqGainMax ) )
    return "gain";
  if( !( s.slope >= kSlopeMin && s.slope <= kSlopeMax ) )
    return "slope";
  return NULL;
}

const char* MatrixModel::normalizeSlot( Slot& s )
{
  if( s.type < SLOT_PEQ || s.type > SLOT_PHASE )
    return "type";
  if( s.type != SLOT_HP && s.type != SLOT_LP )
  {
    s.lr = false;
    s.order = 1;
  }
  const char* param = paramOutOfRange( s );
  if( param )
    return param;
  if( ( s.type == SLOT_HP || s.type == SLOT_LP ) && !orderValid( s.lr, s.order ) )
    return "order";
  return NULL;
}

int MatrixModel::used( const Channel& c )
{
  int n = 0;
  for( size_t i = 0; i < c.slots.size(); i++ )
    n += cost( c.slots[i] );
  return n;
}

/*! The biquads of one Slot (cost() of them, at most 4). */
int MatrixModel::slotCoefs( const Slot& s, Coef* out )
{
  float a[12], b[12];
  int n = 1;
  switch( s.type )
  {
  case SLOT_PEQ:    AudioFilterFactory::makeParametricEQ( a, b, s.gain, s.freq, s.q, kSampleRate ); break;
  case SLOT_LSHELF: AudioFilterFactory::makeLowShelv( a, b, s.gain, s.freq, s.slope, kSampleRate ); break;
  case SLOT_HSHELF: AudioFilterFactory::makeHighShelv( a, b, s.gain, s.freq, s.slope, kSampleRate ); break;
  case SLOT_PHASE:  AudioFilterFactory::makeAllpass( a, b, s.freq, s.q, kSampleRate ); break;
  case SLOT_HP:
  case SLOT_LP:
    n = s.lr ? AudioFilterFactory::makeLinkwitzRiley( a, b, s.type == SLOT_HP, s.order, s.freq, kSampleRate )
             : AudioFilterFactory::makeButterworth( a, b, s.type == SLOT_HP, s.order, s.freq, kSampleRate );
    break;
  }
  for( int k = 0; k < n; k++ )
    for( int j = 0; j < 3; j++ )
    {
      out[k].a[j] = a[3 * k + j];
      out[k].b[j] = b[3 * k + j];
    }
  return n;
}

/*! The Slots packed onto the biquads in order; unused and bypassed stages are passthrough. */
void MatrixModel::stages( const Channel& c, std::vector<Coef>& out ) const
{
  out.assign( nbiquad_, passthroughCoef() );
  int at = 0;
  for( size_t i = 0; i < c.slots.size() && at < nbiquad_; i++ )
  {
    Coef sc[4];
    const int n = slotCoefs( c.slots[i], sc );
    for( int k = 0; k < n && at + k < nbiquad_; k++ )
      if( !c.slots[i].bypass )
        out[at + k] = sc[k];
    at += n;
  }
}

/*! Writes the biquads that differ from prev (all of them if prev is NULL). */
void MatrixModel::writeStages( MatrixChannelKind kind, int idx, const Channel& c, const Channel* prev )
{
  std::vector<Coef> now, before;
  stages( c, now );
  if( prev )
    stages( *prev, before );
  for( int s = 0; s < nbiquad_; s++ )
    if( !prev || memcmp( &now[s], &before[s], sizeof( Coef ) ) != 0 )
      port_.writeBiquad( kind, idx, s, now[s].a, now[s].b );
}

/*! True if the edit rewrites any biquad: a new coefficient on running filter state can click,
 *  and a multi-biquad rewrite would briefly run a mix of old and new Slots
 *  (spec "Live apply", user story 37; ticket 29). */
bool MatrixModel::needsRamp( const Channel& before, const Channel& after ) const
{
  std::vector<Coef> was, now;
  stages( before, was );
  stages( after, now );
  for( int s = 0; s < nbiquad_; s++ )
    if( memcmp( &was[s], &now[s], sizeof( Coef ) ) != 0 )
      return true;
  return false;
}

/*! Slot edit of a patch; changes only c, and only if it returns "". */
std::string MatrixModel::applySlot( JsonObject op, Channel& c ) const
{
  const char* name = op["op"].as<const char*>();
  if( !name )
    return "missing slot op";
  const bool add = ( strcmp( name, "add" ) == 0 );
  if( !add && strcmp( name, "del" ) != 0 && strcmp( name, "set" ) != 0 )
    return "unknown slot op";

  Slot s = choice( 0 );
  int at = -1;
  if( !add )
  {
    if( !op["i"].is<int>() || op["i"].as<int>() < 0 || op["i"].as<int>() >= (int)c.slots.size() )
      return "no such slot";
    at = op["i"].as<int>();
    s = c.slots[at];
  }
  if( strcmp( name, "del" ) == 0 )
  {
    c.slots.erase( c.slots.begin() + at );  // the following Slots close the gap
    return "";
  }
  const struct { const char* key; float* v; } nums[] = {
    { "freq", &s.freq }, { "q", &s.q }, { "gain", &s.gain }, { "slope", &s.slope } };
  for( size_t k = 0; k < 4; k++ )
  {
    if( !op.containsKey( nums[k].key ) )
      continue;
    if( !op[nums[k].key].is<float>() )
      return std::string( nums[k].key ) + " must be a number";
    *nums[k].v = op[nums[k].key].as<float>();
    // range per number as it is read: the established first error of an edit
    const char* param = paramOutOfRange( s );
    if( param )
      return std::string( param ) + " out of range";
  }
  if( op.containsKey( "bypass" ) )
  {
    if( !op["bypass"].is<bool>() )
      return "bypass must be true or false";
    s.bypass = op["bypass"].as<bool>();
  }
  if( op.containsKey( "type" ) )
  {
    const int k = typeFromName( op["type"].as<const char*>() );
    if( k < 0 )
      return "no such slot type";
    if( !add && k != (int)s.type )  // a hidden design/order of the old type must not carry over
    {
      s.lr = false;
      s.order = 1;
    }
    s.type = (SlotType)k;
  }
  if( op.containsKey( "design" ) )
  {
    const char* d = op["design"].as<const char*>();
    if( !d || ( strcmp( d, "bw" ) != 0 && strcmp( d, "lr" ) != 0 ) )
      return "design must be bw or lr";
    s.lr = ( strcmp( d, "lr" ) == 0 );
  }
  if( op.containsKey( "order" ) )
  {
    if( !op["order"].is<int>() )
      return "order must be a number";
    s.order = op["order"].as<int>();
  }
  const char* invalid = normalizeSlot( s );
  if( invalid )
  {
    if( strcmp( invalid, "order" ) == 0 )
      return s.lr ? "Linkwitz-Riley order must be 2, 4 or 8" : "Butterworth order must be 1 to 8";
    return std::string( invalid ) + " out of range";
  }
  if( used( c ) - ( add ? 0 : cost( c.slots[at] ) ) + cost( s ) > nbiquad_ )
    return add ? "no biquads left" : "not enough biquads left";
  if( add )
    c.slots.push_back( s );
  else
    c.slots[at] = s;
  return "";
}

//==============================================================================
MatrixModel::MatrixModel( MatrixPort& port, int nbus, int nout, int nbiquad, int maxDelay )
  : port_( port ), nbus_( nbus < MATRIX_MAX_BUS ? nbus : MATRIX_MAX_BUS ),
    nout_( nout < MATRIX_MAX_OUT ? nout : MATRIX_MAX_OUT ), nbiquad_( nbiquad > 0 ? nbiquad : 0 ),
    maxDelay_( maxDelay > 0 && maxDelay < MATRIX_MAX_DELAY_SAMPLES ? maxDelay : MATRIX_MAX_DELAY_SAMPLES ),
    spdif_( 0 ), dirty_( false ), lastChange_( 0 ), presetSwitching_( false ), preset_( 0 )
{
  undo_.valid = false;
  reset();
}

void MatrixModel::reset( void )
{
  spdif_ = spdifPairCode( 0 );
  busLink_ = (LinkGroup){ false, false, false, false };
  for( int g = 0; g < MATRIX_OUT_LINKS; g++ )
    outLink_[g] = (LinkGroup){ false, false, false, false };
  for( int b = 0; b < nbus_; b++ )
  {
    char buf[16];
    snprintf( buf, sizeof( buf ), "Bus %d", b + 1 );
    bus_[b].name = buf;
    bus_[b].mute = false;
    bus_[b].gain = 0.0f;
    bus_[b].gainBypass = false;
    bus_[b].polarity = false;
    bus_[b].delay = 0;
    bus_[b].delayBypass = false;
    bus_[b].sel = (int)sourceCode( b );  // Analog 1, Analog 2
    bus_[b].link = -1;
    bus_[b].slots.clear();
  }
  for( int o = 0; o < nout_; o++ )
  {
    char buf[16];
    snprintf( buf, sizeof( buf ), "Output %d", o + 1 );
    out_[o].name = buf;
    out_[o].mute = true;
    out_[o].gain = 0.0f;
    out_[o].gainBypass = false;
    out_[o].polarity = false;
    out_[o].delay = 0;
    out_[o].delayBypass = false;
    out_[o].sel = o % 2;
    out_[o].link = -1;
    out_[o].slots.clear();
  }
}

void MatrixModel::saveUndo( void )
{
  undo_.valid = true;
  undo_.spdif = spdif_;
  undo_.busLink = busLink_;
  for( int g = 0; g < MATRIX_OUT_LINKS; g++ ) undo_.outLink[g] = outLink_[g];
  for( int b = 0; b < nbus_; b++ ) undo_.bus[b] = bus_[b];
  for( int o = 0; o < nout_; o++ ) undo_.out[o] = out_[o];
  undo_.dirty = dirty_;
  undo_.lastChange = lastChange_;
}

void MatrixModel::loadDefaults( void )
{
  reset();
  undo_.valid = false;
  dirty_ = false;
  applyAll();
}

double MatrixModel::linear( const Channel& c ) const
{
  return c.mute ? 0.0 : c.gainBypass ? 1.0 : pow( 10.0, c.gain / 20.0 );
}

/*! Writes what differs from the snapshot. Channels that switch their route or Source, or rewrite
 *  any biquad, ramp together: all down, one wait, every write, all up. */
void MatrixModel::writeChanges( const Snapshot& before )
{
  bool ramp[MATRIX_MAX_BUS + MATRIX_MAX_OUT];
  bool anyRamp = false;
  for( int n = 0; n < nbus_ + nout_; n++ )
  {
    const MatrixChannelKind kind = n < nbus_ ? MATRIX_BUS : MATRIX_OUT;
    const int idx = n < nbus_ ? n : n - nbus_;
    const Channel& was = kind == MATRIX_BUS ? before.bus[idx] : before.out[idx];
    const Channel& now = kind == MATRIX_BUS ? bus_[idx] : out_[idx];
    ramp[n] = linear( now ) != 0.0 && ( now.sel != was.sel || needsRamp( was, now ) );
    if( ramp[n] )
    {
      port_.writeGain( kind, idx, 0.0 );
      anyRamp = true;
    }
  }
  if( anyRamp )
    port_.waitMs( MATRIX_RAMP_MS );
  for( int n = 0; n < nbus_ + nout_; n++ )
  {
    const MatrixChannelKind kind = n < nbus_ ? MATRIX_BUS : MATRIX_OUT;
    const int idx = n < nbus_ ? n : n - nbus_;
    const Channel& was = kind == MATRIX_BUS ? before.bus[idx] : before.out[idx];
    const Channel& now = kind == MATRIX_BUS ? bus_[idx] : out_[idx];
    if( !ramp[n] && linear( now ) != linear( was ) ) port_.writeGain( kind, idx, linear( now ) );
    if( now.sel != was.sel )
    {
      if( kind == MATRIX_BUS ) port_.writeSource( idx, (uint32_t)now.sel ); else port_.writeRoute( idx, now.sel );
    }
    if( kind == MATRIX_OUT )
    {
      if( now.polarity != was.polarity ) port_.writePolarity( idx, now.polarity );
      if( ( now.delayBypass ? 0 : now.delay ) != ( was.delayBypass ? 0 : was.delay ) )
        port_.writeDelay( idx, now.delayBypass ? 0 : now.delay );
    }
    bool slotsChanged = now.slots.size() != was.slots.size();
    for( size_t i = 0; !slotsChanged && i < now.slots.size(); i++ )
    {
      const Slot& a = now.slots[i]; const Slot& b = was.slots[i];
      slotsChanged = a.bypass != b.bypass || a.type != b.type || a.lr != b.lr || a.order != b.order
        || a.freq != b.freq || a.q != b.q || a.gain != b.gain || a.slope != b.slope;
    }
    if( slotsChanged ) writeStages( kind, idx, now, &was );
  }
  for( int n = 0; n < nbus_ + nout_; n++ )
  {
    const MatrixChannelKind kind = n < nbus_ ? MATRIX_BUS : MATRIX_OUT;
    const int idx = n < nbus_ ? n : n - nbus_;
    if( ramp[n] ) port_.writeGain( kind, idx, linear( kind == MATRIX_BUS ? bus_[idx] : out_[idx] ) );
  }
}

void MatrixModel::snapshot( Snapshot& s ) const
{
  for( int b = 0; b < nbus_; b++ ) s.bus[b] = bus_[b];
  for( int o = 0; o < nout_; o++ ) s.out[o] = out_[o];
}
//==============================================================================
bool MatrixModel::beginPresetSwitch( void )
{
  if( presetSwitching_ )
    return false;
  presetSwitching_ = true;
  return true;
}

MatrixEditResult MatrixModel::apply( JsonObject patch, uint32_t nowMs )
{
  if( patch.containsKey( "preset" ) )
  {
    if( !patch["preset"].is<int>() || patch["preset"].as<int>() != preset_ )
      return { MATRIX_EDIT_CONFLICT, "preset changed; refresh the matrix" };
    patch.remove( "preset" );
  }
  if( presetSwitching_ )
    return { MATRIX_EDIT_CONFLICT, "preset switch in progress" };
  std::string reason = applyPatch( patch, nowMs );
  return { reason.empty() ? MATRIX_EDIT_OK : MATRIX_EDIT_BAD_REQUEST, reason };
}

std::string MatrixModel::applyLinkGroup( JsonObject patch, uint32_t nowMs )
{
  // The linked aspects a patch may set; Buses cannot link delay.
  struct LinkedAspect
  {
    const char* key;
    bool LinkGroup::* flag;
    bool onBuses;
  };
  static const LinkedAspect kAspects[] = {
    { "mute", &LinkGroup::mute, true },
    { "filters", &LinkGroup::filters, true },
    { "gain", &LinkGroup::gain, true },
    { "delay", &LinkGroup::delay, false },
  };

  const LinkGroupId id = linkGroupFromId( patch["linkGroup"].as<const char*>() );
  if( !id.found ) return "no such Link group";
  const bool isBus = id.isBus;
  const int linkGroup = id.index;
  LinkGroup next = isBus ? busLink_ : outLink_[linkGroup];
  bool touched = false;
  for( JsonPair kv : patch )
  {
    const char* key = kv.key().c_str();
    if( strcmp( key, "linkGroup" ) == 0 ) continue;
    const LinkedAspect* aspect = NULL;
    for( const LinkedAspect& a : kAspects )
      if( strcmp( key, a.key ) == 0 ) aspect = &a;
    if( !aspect )
      return std::string( "unknown field " ) + key;
    if( !kv.value().is<bool>() ) return std::string( key ) + " must be true or false";
    const bool v = kv.value().as<bool>();
    if( isBus && v && !aspect->onBuses ) return "Buses have no delay link";
    next.*( aspect->flag ) = v;
    touched = true;
  }
  if( !touched ) return "missing linked aspect";

  Snapshot before;
  snapshot( before );
  LinkGroup old = isBus ? busLink_ : outLink_[linkGroup];
  saveUndo();
  ( isBus ? busLink_ : outLink_[linkGroup] ) = next;

  // When an aspect becomes linked, the first member supplies its absolute value.
  Channel* anchor = NULL;
  const int count = isBus ? nbus_ : nout_;
  for( int i = 0; i < count && !anchor; i++ )
  {
    Channel& c = isBus ? bus_[i] : out_[i];
    if( c.link == linkGroup ) anchor = &c;
  }
  if( anchor ) for( int i = 0; i < count; i++ )
  {
    Channel& c = isBus ? bus_[i] : out_[i];
    if( c.link != linkGroup || &c == anchor ) continue;
    if( next.mute && !old.mute ) c.mute = anchor->mute;
    if( next.gain && !old.gain ) { c.gain = anchor->gain; c.gainBypass = anchor->gainBypass; }
    if( next.delay && !old.delay ) { c.delay = anchor->delay; c.delayBypass = anchor->delayBypass; }
    if( next.filters && !old.filters ) c.slots = anchor->slots;
  }
  writeChanges( before );
  dirty_ = true;
  lastChange_ = nowMs;
  return "";
}

std::string MatrixModel::applyPatch( JsonObject patch, uint32_t nowMs )
{
  if( patch.containsKey( "linkGroup" ) )
    return applyLinkGroup( patch, nowMs );
  const char* kind = patch["ch"].as<const char*>();
  if( !kind )
    return "missing ch";
  if( strcmp( kind, "spdif" ) == 0 )
  {
    if( !patch["sel"].is<long>() || !spdifPairValid( patch["sel"].as<long>(), nout_ ) )
      return "no such SPDIF out";
    const uint32_t sel = (uint32_t)patch["sel"].as<long>();
    saveUndo();
    if( sel != spdif_ )
    {
      spdif_ = sel;
      port_.writeSpdifOut( spdif_, spdif_ + 1 );
      dirty_ = true;
      lastChange_ = nowMs;
    }
    return "";
  }
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
  bool slotChanged = false;
  bool muteChanged = false, gainChanged = false, delayChanged = false;
  bool linkChanged = false;

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
      muteChanged = true;
    }
    else if( strcmp( key, "gain" ) == 0 )
    {
      if( !v.is<float>() )
        return "gain must be a number";
      const float g = v.as<float>();
      if( !( g >= kGainMin && g <= kGainMax ) )
        return "gain out of range";
      next.gain = g;
      gainChanged = true;
    }
    else if( strcmp( key, "gainBypass" ) == 0 )
    {
      if( !v.is<bool>() )
        return "gainBypass must be true or false";
      next.gainBypass = v.as<bool>();
      gainChanged = true;
    }
    else if( strcmp( key, "polarity" ) == 0 || strcmp( key, "delayUs" ) == 0 || strcmp( key, "delayBypass" ) == 0 )
    {
      if( isBus )
        return std::string( "buses have no " ) + key;
      if( strcmp( key, "polarity" ) == 0 )
      {
        if( !v.is<bool>() )
          return "polarity must be true or false";
        next.polarity = v.as<bool>();
      }
      else if( strcmp( key, "delayBypass" ) == 0 )
      {
        if( !v.is<bool>() )
          return "delayBypass must be true or false";
        next.delayBypass = v.as<bool>();
        delayChanged = true;
      }
      else
      {
        if( !v.is<float>() )
          return "delayUs must be a number";
        const double us = v.as<double>();
        if( !( us >= 0.0 && us <= 1e6 ) )
          return "delay out of range";
        const int samples = (int)floor( us * kSampleRate / 1e6 + 0.5 );
        if( samples > maxDelay_ )
          return "delay out of range";
        next.delay = samples;
        delayChanged = true;
      }
    }
    else if( strcmp( key, "slot" ) == 0 )
    {
      if( !v.is<JsonObject>() )
        return "slot must be an object";
      std::string err = applySlot( v.as<JsonObject>(), next );
      if( !err.empty() )
        return err;
      slotChanged = true;
    }
    else if( strcmp( key, "link" ) == 0 )
    {
      const char* id = v.as<const char*>();
      if( !id ) return "link must be a string";
      const int linkGroup = linkFromId( id, isBus );
      if( linkGroup == -2 ) return "Link group outside the Channel's scope";
      next.link = linkGroup;
      linkChanged = next.link != prev.link;
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

  // A joining Channel takes the linked aspects from the Link group's first existing member, so it
  // cannot set them in the same patch.
  if( linkChanged && next.link >= 0 )
  {
    const Channel* list = isBus ? bus_ : out_;
    const int count = isBus ? nbus_ : nout_;
    const LinkGroup& linked = isBus ? busLink_ : outLink_[next.link];
    for( int i = 0; i < count; i++ ) if( i != idx && list[i].link == next.link )
    {
      const char* taken = linked.mute && muteChanged ? "mute" : linked.gain && gainChanged ? "gain"
        : linked.delay && delayChanged ? "delay" : linked.filters && slotChanged ? "filters" : NULL;
      if( taken ) return std::string( "a Channel joining a Link group takes over its linked " ) + taken;
      if( linked.mute ) next.mute = list[i].mute;
      if( linked.gain ) { next.gain = list[i].gain; next.gainBypass = list[i].gainBypass; }
      if( linked.delay ) { next.delay = list[i].delay; next.delayBypass = list[i].delayBypass; }
      if( linked.filters ) next.slots = list[i].slots;
      break;
    }
  }

  Snapshot before;
  snapshot( before );
  saveUndo();
  (isBus ? bus_[idx] : out_[idx]) = next;

  if( next.link >= 0 )
  {
    Channel* list = isBus ? bus_ : out_;
    const int count = isBus ? nbus_ : nout_;
    const LinkGroup& linked = isBus ? busLink_ : outLink_[next.link];
    for( int i = 0; i < count; i++ ) if( i != idx && list[i].link == next.link )
    {
      if( linked.mute && muteChanged ) list[i].mute = next.mute;
      if( linked.gain && gainChanged ) { list[i].gain = next.gain; list[i].gainBypass = next.gainBypass; }
      if( linked.delay && delayChanged ) { list[i].delay = next.delay; list[i].delayBypass = next.delayBypass; }
      if( linked.filters && slotChanged ) list[i].slots = next.slots;
    }
  }
  writeChanges( before );

  const bool switched = next.sel != prev.sel;
  if( slotChanged || linkChanged || next.name != prev.name || next.mute != prev.mute || next.gain != prev.gain || switched
      || next.gainBypass != prev.gainBypass || next.polarity != prev.polarity || next.delay != prev.delay
      || next.delayBypass != prev.delayBypass )
  {
    dirty_ = true;
    lastChange_ = nowMs;
  }
  return "";
}

void MatrixModel::undo( void )
{
  if( !undo_.valid )
    return;
  spdif_ = undo_.spdif;
  busLink_ = undo_.busLink;
  for( int g = 0; g < MATRIX_OUT_LINKS; g++ ) outLink_[g] = undo_.outLink[g];
  for( int b = 0; b < nbus_; b++ ) bus_[b] = undo_.bus[b];
  for( int o = 0; o < nout_; o++ ) out_[o] = undo_.out[o];
  dirty_ = undo_.dirty;
  lastChange_ = undo_.lastChange;
  undo_.valid = false;
}

//==============================================================================
void MatrixModel::applyAll( void )
{
  for( int b = 0; b < nbus_; b++ )
  {
    port_.writeSource( b, (uint32_t)bus_[b].sel );
    port_.writeGain( MATRIX_BUS, b, linear( bus_[b] ) );
    writeStages( MATRIX_BUS, b, bus_[b], NULL );
  }
  for( int o = 0; o < nout_; o++ )
  {
    port_.writeRoute( o, out_[o].sel );
    port_.writeGain( MATRIX_OUT, o, linear( out_[o] ) );
    port_.writePolarity( o, out_[o].polarity );
    port_.writeDelay( o, out_[o].delayBypass ? 0 : out_[o].delay );
    writeStages( MATRIX_OUT, o, out_[o], NULL );
  }
  port_.writeSpdifOut( spdif_, spdif_ + 1 );
}

std::string MatrixModel::coeffsJson( const char* ch, int idx ) const
{
  const bool isBus = ( strcmp( ch, "bus" ) == 0 );
  if( !isBus && strcmp( ch, "out" ) != 0 )
    return "";
  if( idx < 0 || idx >= ( isBus ? nbus_ : nout_ ) )
    return "";
  const Channel& c = isBus ? bus_[idx] : out_[idx];
  std::vector<Coef> st;
  stages( c, st );
  DynamicJsonDocument doc( 512 + 160 * nbiquad_ );
  doc["fs"] = (int)kSampleRate;
  JsonArray bq = doc.createNestedArray( "bq" );
  int at = 0;
  for( size_t i = 0; i < c.slots.size(); i++ )
  {
    const int n = cost( c.slots[i] );
    for( int s = at; s < at + n && !c.slots[i].bypass; s++ )
    {
      // the DSP stores 8.24, convertTo824() truncates: floor to its grid; order b0 b1 b2 a1 a2
      const double v[5] = { st[s].b[0], st[s].b[1], st[s].b[2], st[s].a[1], st[s].a[2] };
      JsonArray row = bq.createNestedArray();
      for( int k = 0; k < 5; k++ )
        row.add( floor( v[k] * 16777216.0 ) / 16777216.0 );
    }
    at += n;
  }
  return toString( doc );
}

//==============================================================================
void MatrixModel::putChannel( JsonArray arr, const Channel& c, const char* selKey, bool withLimits ) const
{
  JsonObject o = arr.createNestedObject();
  o["name"] = c.name.c_str();
  o[selKey] = c.sel;
  o["mute"] = c.mute;
  o["gain"] = c.gain;
  o["gainBypass"] = c.gainBypass;
  char id[4];
  o["link"] = linkId( id, c.link, strcmp( selKey, "source" ) == 0 );
  if( strcmp( selKey, "route" ) == 0 )  // Outputs only
  {
    o["polarity"] = c.polarity;
    o["delaySamples"] = c.delay;
    o["delayBypass"] = c.delayBypass;
    if( withLimits )
    {
      o["delayUs"] = c.delay * 1e6 / kSampleRate;
      o["delayMm"] = c.delay * 343000.0 / kSampleRate;
    }
  }
  JsonArray slots = o.createNestedArray( "slots" );
  for( size_t i = 0; i < c.slots.size(); i++ )
  {
    JsonObject s = slots.createNestedObject();
    const Slot& sl = c.slots[i];
    s["type"] = kTypeNames[sl.type];
    if( sl.type == SLOT_HP || sl.type == SLOT_LP )
    {
      s["design"] = sl.lr ? "lr" : "bw";
      s["order"] = sl.order;
    }
    s["freq"] = sl.freq;
    s["q"] = sl.q;
    s["gain"] = sl.gain;
    s["slope"] = sl.slope;
    s["bypass"] = sl.bypass;
    if( withLimits )
      s["avail"] = nbiquad_ - used( c ) + cost( sl );  // biquads this Slot may take on a type/order change
  }
  if( withLimits )
  {
    const int free = nbiquad_ - used( c );
    o["budget"] = nbiquad_;
    o["used"] = used( c );
    o["free"] = free;
  }
}

static std::string toString( const JsonDocument& doc )
{
  std::string s( measureJson( doc ), 0 );
  serializeJson( doc, &s[0], s.size() + 1 );
  return s;
}

std::string MatrixModel::stateJson( void ) const
{
  DynamicJsonDocument doc( 20480 + 288 * slotCount() );
  doc["preset"] = preset_;
  doc["nbus"] = nbus_;
  doc["nout"] = nout_;
  doc["nbiquad"] = nbiquad_;
  doc["gainmin"] = kGainMin;
  doc["gainmax"] = kGainMax;
  doc["delaymax"] = maxDelay_;
  JsonObject peq = doc.createNestedObject( "peq" );
  peq["fmin"] = kFreqMin;
  peq["fmax"] = kFreqMax;
  peq["qmin"] = kQMin;
  peq["qmax"] = kQMax;
  peq["gmin"] = kPeqGainMin;
  peq["gmax"] = kPeqGainMax;
  doc["slopemin"] = kSlopeMin;
  doc["slopemax"] = kSlopeMax;
  JsonArray types = doc.createNestedArray( "types" );  // Slot types in the order of the page's dropdown
  for( int n = 0; n < kNumTypes; n++ )
    types.add( kTypeNames[n] );
  JsonArray choices = doc.createNestedArray( "choices" );  // every selectable type/order and its biquad cost
  for( int n = 0; n < choiceCount(); n++ )
  {
    const Slot s = choice( n );
    JsonObject o = choices.createNestedObject();
    o["t"] = kTypeNames[s.type];
    if( s.type == SLOT_HP || s.type == SLOT_LP )
    {
      o["d"] = s.lr ? "lr" : "bw";
      o["o"] = s.order;
    }
    o["c"] = cost( s );
  }
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
  JsonObject links = doc.createNestedObject( "links" );
  JsonObject busLink = links.createNestedObject( "bus" );
  busLink["id"] = "bus"; busLink["mute"] = busLink_.mute; busLink["filters"] = busLink_.filters;
  busLink["gain"] = busLink_.gain; busLink["delay"] = false;
  JsonArray outLinks = links.createNestedArray( "outs" );
  for( int g = 0; g < MATRIX_OUT_LINKS; g++ )
  {
    JsonObject link = outLinks.createNestedObject();
    char id[4];
    link["id"] = linkId( id, g, false ); link["mute"] = outLink_[g].mute; link["filters"] = outLink_[g].filters;
    link["gain"] = outLink_[g].gain; link["delay"] = outLink_[g].delay;
  }
  doc["spdifout"] = (long)spdif_;
  JsonArray spdifOpts = doc.createNestedArray( "spdifopts" );
  std::string spdifNames[kNumSpdifSrcPairs + MATRIX_MAX_OUT / 2];
  for( int n = 0; n < kNumSpdifSrcPairs + nout_ / 2; n++ )
  {
    spdifNames[n] = spdifPairName( n );
    JsonObject s = spdifOpts.createNestedObject();
    s["n"] = spdifNames[n].c_str();
    s["v"] = (long)spdifPairCode( n );
  }
  JsonArray buses = doc.createNestedArray( "buses" );
  for( int b = 0; b < nbus_; b++ )
    putChannel( buses, bus_[b], "source", true );
  JsonArray outs = doc.createNestedArray( "outs" );
  for( int o = 0; o < nout_; o++ )
    putChannel( outs, out_[o], "route", true );
  return toString( doc );
}

std::string MatrixModel::savePreset( void ) const
{
  DynamicJsonDocument doc( 5120 + 256 * slotCount() );
  doc["v"] = kPresetVersion;
  doc["spdif"] = (long)spdif_;
  JsonObject links = doc.createNestedObject( "links" );
  JsonObject busLink = links.createNestedObject( "bus" );
  busLink["mute"] = busLink_.mute; busLink["filters"] = busLink_.filters; busLink["gain"] = busLink_.gain;
  JsonArray outLinks = links.createNestedArray( "outs" );
  for( int g = 0; g < MATRIX_OUT_LINKS; g++ )
  {
    JsonObject link = outLinks.createNestedObject();
    link["mute"] = outLink_[g].mute; link["filters"] = outLink_[g].filters;
    link["gain"] = outLink_[g].gain; link["delay"] = outLink_[g].delay;
  }
  JsonArray buses = doc.createNestedArray( "buses" );
  for( int b = 0; b < nbus_; b++ )
    putChannel( buses, bus_[b], "source", false );
  JsonArray outs = doc.createNestedArray( "outs" );
  for( int o = 0; o < nout_; o++ )
    putChannel( outs, out_[o], "route", false );
  return toString( doc );
}

std::string MatrixModel::readChannel( JsonObject o, bool isBus, Channel& c ) const
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
    c.sel = (int)o["source"].as<long>();
  }
  else
  {
    if( !o["route"].is<int>() || o["route"].as<int>() < 0 || o["route"].as<int>() >= kNumRoutes )
      return "bad route";
    c.sel = o["route"].as<int>();
  }
  // optional: presets from before polarity, delay and bypass have none
  c.gainBypass = o["gainBypass"] | false;
  c.polarity = false;
  c.delay = 0;
  c.delayBypass = false;
  if( !isBus )
  {
    c.polarity = o["polarity"] | false;
    c.delay = o["delaySamples"] | 0;
    c.delayBypass = o["delayBypass"] | false;
    if( c.delay < 0 || c.delay > maxDelay_ )
      return "bad delay";
  }
  c.slots.clear();
  c.link = -1;
  if( o.containsKey( "link" ) )
  {
    const char* id = o["link"].as<const char*>();
    if( !id ) return "bad link";
    c.link = linkFromId( id, isBus );
    if( c.link == -2 ) return "Link group outside the Channel's scope";
  }
  JsonArray slots = o["slots"];  // optional: presets from before the Filter row have none
  for( JsonObject s : slots )
  {
    Slot sl = choice( 0 );
    if( !s["freq"].is<float>() || !s["q"].is<float>() || !s["gain"].is<float>() || !s["bypass"].is<bool>() )
      return "bad slot";
    sl.freq = s["freq"];
    sl.q = s["q"];
    sl.gain = s["gain"];
    sl.bypass = s["bypass"];
    // optional: presets from before the Slot types are all PEQs
    if( s.containsKey( "type" ) )
    {
      const int k = typeFromName( s["type"].as<const char*>() );
      if( k < 0 )
        return "bad slot";
      sl.type = (SlotType)k;
    }
    // optional, but not coerced when present
    sl.lr = false;
    if( s.containsKey( "design" ) )
    {
      const char* d = s["design"].as<const char*>();
      if( !d || ( strcmp( d, "bw" ) != 0 && strcmp( d, "lr" ) != 0 ) )
        return "bad slot";
      sl.lr = ( strcmp( d, "lr" ) == 0 );
    }
    sl.order = 2;
    if( s.containsKey( "order" ) )
    {
      if( !s["order"].is<int>() )
        return "bad slot";
      sl.order = s["order"].as<int>();
    }
    sl.slope = 1.0f;
    if( s.containsKey( "slope" ) )
    {
      if( !s["slope"].is<float>() )
        return "bad slot";
      sl.slope = s["slope"].as<float>();
    }
    if( normalizeSlot( sl ) )
      return "bad slot";
    c.slots.push_back( sl );
  }
  if( used( c ) > nbiquad_ )
    return "too many slots";
  c.name = n;
  c.mute = o["mute"].as<bool>();
  c.gain = o["gain"].as<float>();
  return "";
}

std::string MatrixModel::loadPreset( const std::string& json )
{
  // a smaller document usually fits; the 4x one only when that ran out of memory
  DynamicJsonDocument docS( 2 * json.size() + 1024 );
  DeserializationError jerr = deserializeJson( docS, json.c_str() );
  DynamicJsonDocument* big = NULL;
  if( jerr == DeserializationError::NoMemory )
  {
    big = new DynamicJsonDocument( 4 * json.size() + 1024 );
    jerr = deserializeJson( *big, json.c_str() );
  }
  struct Free { DynamicJsonDocument* p; ~Free() { delete p; } } freeBig = { big };
  if( jerr == DeserializationError::NoMemory )
    return "not enough memory for the preset";
  if( jerr )
    return "preset is not valid JSON";
  DynamicJsonDocument& doc = big ? *big : docS;
  const int version = doc["v"] | 0;
  if( version < 1 || version > kPresetVersion )  // version 1 (PEQ-only Slots) still loads
    return "unknown preset version";
  JsonArray buses = doc["buses"];
  JsonArray outs = doc["outs"];
  if( (int)buses.size() != nbus_ || (int)outs.size() != nout_ )
    return "preset has the wrong channel count";

  uint32_t spdif = spdifPairCode( 0 );  // optional: presets from before the SPDIF out selector have none
  if( doc.containsKey( "spdif" ) )
  {
    if( !doc["spdif"].is<long>() || !spdifPairValid( doc["spdif"].as<long>(), nout_ ) )
      return "bad spdif";
    spdif = (uint32_t)doc["spdif"].as<long>();
  }

  LinkGroup busLink = { false, false, false, false };
  LinkGroup outLink[MATRIX_OUT_LINKS] = {};
  if( doc.containsKey( "links" ) )
  {
    JsonObject links = doc["links"];
    JsonObject b = links["bus"];
    JsonArray outsLinks = links["outs"];
    if( b.isNull() || outsLinks.size() != MATRIX_OUT_LINKS ) return "bad links";
    const char* keys[] = { "mute", "filters", "gain" };
    bool* busValues[] = { &busLink.mute, &busLink.filters, &busLink.gain };
    for( int k = 0; k < 3; k++ )
    {
      if( !b[keys[k]].is<bool>() ) return "bad links";
      *busValues[k] = b[keys[k]].as<bool>();
    }
    for( int g = 0; g < MATRIX_OUT_LINKS; g++ )
    {
      JsonObject o = outsLinks[g];
      const char* outKeys[] = { "mute", "filters", "gain", "delay" };
      bool* values[] = { &outLink[g].mute, &outLink[g].filters, &outLink[g].gain, &outLink[g].delay };
      for( int k = 0; k < 4; k++ )
      {
        if( !o[outKeys[k]].is<bool>() ) return "bad links";
        *values[k] = o[outKeys[k]].as<bool>();
      }
    }
  }

  Channel nb[MATRIX_MAX_BUS], no[MATRIX_MAX_OUT];
  for( int b = 0; b < nbus_; b++ )
  {
    std::string err = readChannel( buses[b], true, nb[b] );
    if( !err.empty() )
      return err;
  }
  for( int o = 0; o < nout_; o++ )
  {
    std::string err = readChannel( outs[o], false, no[o] );
    if( !err.empty() )
      return err;
  }
  for( int b = 0; b < nbus_; b++ ) bus_[b] = nb[b];
  for( int o = 0; o < nout_; o++ ) out_[o] = no[o];
  busLink_ = busLink;
  for( int g = 0; g < MATRIX_OUT_LINKS; g++ ) outLink_[g] = outLink[g];
  spdif_ = spdif;
  undo_.valid = false;
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
