#ifndef MATRIXMODEL_H_
#define MATRIXMODEL_H_

#include <stdint.h>
#include <string>
#include <vector>
#include <ArduinoJson.h>

/*! State and rules of the matrix plugin: two Buses (each with a Source), eight
 *  Outputs (each routed to Bus 1, Bus 2 or Bus 1+2), per Channel name, mute and
 *  gain. No Arduino or I2C dependencies: every change to the DSP goes through the
 *  injected MatrixPort, so the model is tested on the host with a fake port.
 */

enum MatrixChannelKind { MATRIX_BUS, MATRIX_OUT };

/*! Writes the model asks the DSP for. */
struct MatrixPort
{
  /*! Gain block of a Channel; linear 0.0 = mute. */
  virtual void writeGain( MatrixChannelKind kind, int idx, double linear ) = 0;
  /*! Route selector of an Output: 0 = Bus 1, 1 = Bus 2, 2 = Bus 1+2. */
  virtual void writeRoute( int out, int route ) = 0;
  /*! Source mux of a Bus; sel = (port << 16) | channel as in the input select. */
  virtual void writeSource( int bus, uint32_t sel ) = 0;
  /*! Polarity block of an Output. */
  virtual void writePolarity( int out, bool invert ) = 0;
  /*! Delay block of an Output in whole samples (48 kHz). */
  virtual void writeDelay( int out, int samples ) = 0;
  /*! SPDIF output mux: left and right codes (port << 16) | channel as in the input select. */
  virtual void writeSpdifOut( uint32_t left, uint32_t right ) = 0;
  /*! Pauses the write sequence, e.g. until a gain ramp has finished. */
  virtual void waitMs( int ms ) = 0;
  /*! One biquad of a Channel (stage 0 = first). a[3], b[3] as made by the
   *  AudioFilterFactory (a[0] = 1, a1/a2 already negated for the DSP). */
  virtual void writeBiquad( MatrixChannelKind kind, int idx, int stage, const float a[3], const float b[3] ) = 0;
};

/*! Coefficients of one biquad as made by the AudioFilterFactory. */
struct MatrixCoef
{
  float a[3], b[3];
};

#define MATRIX_MAX_BUS 2
#define MATRIX_MAX_OUT 8
#define MATRIX_NAME_LEN 16
#define MATRIX_OUT_LINKS 4  // Output Link groups A..D
#define MATRIX_MAX_DELAY_SAMPLES 1920  // 40 ms at 48 kHz
#define MATRIX_SAVE_DELAY_MS 2000
#define MATRIX_RAMP_MS 20  // gain slew to settle before a route/Source switch or a biquad write

/*! Why a live edit was rejected, or that it was accepted. */
enum MatrixEditStatus
{
  MATRIX_EDIT_OK,
  MATRIX_EDIT_BAD_REQUEST,  // the patch itself is invalid
  MATRIX_EDIT_CONFLICT,     // a Preset switch is in progress or the patch was made against another Preset
  MATRIX_EDIT_BUSY          // the worker queue is full; the patch was undone and may be sent again
};

/*! HTTP status of the /matrix reply; 503 for busy, as /preset answers a full worker queue. */
inline int matrixEditHttpStatus( MatrixEditStatus status )
{
  switch( status )
  {
    case MATRIX_EDIT_OK:       return 200;
    case MATRIX_EDIT_CONFLICT: return 409;
    case MATRIX_EDIT_BUSY:     return 503;
    default:                   return 400;
  }
}

/*! Outcome of a live edit; reason is "" when accepted, else the text for the user. */
struct MatrixEditResult
{
  MatrixEditStatus status;
  std::string reason;
  bool ok( void ) const { return status == MATRIX_EDIT_OK; }
};

class MatrixModel
{
public:
  /*! Starts with the defaults (no writes): Bus 1 = Analog 1, Bus 2 = Analog 2,
   *  Outputs alternate Bus 1 / Bus 2, all Outputs muted, 0 dB. maxDelay: the plugin's
   *  Output delay limit in samples (0 = unknown), capped at MATRIX_MAX_DELAY_SAMPLES. */
  MatrixModel( MatrixPort& port, int nbus, int nout, int nbiquad = 0, int maxDelay = 0 );

  /*! Applies a patch {"ch":"bus"|"out","idx":n, name, mute, gain, link, source (Bus), route (Output),
   *  gainBypass, and on Outputs only polarity (bool), delayUs (rounded to whole samples, max 40 ms)
   *  and delayBypass,
   *  slot: {"op":"add", type, design, order, freq, q, gain, slope} | {"op":"del","i"} | {"op":"set","i", the same, bypass}}.
   *  type: "peq" (default), "hp", "lp", "lshelf", "hshelf", "phase"; HP/LP: design "bw" (order 1-8, default)
   *  or "lr" (order 2, 4, 8), default order 2; shelf slope 0.1-1.
   *  SPDIF out: {"ch":"spdif","sel":code} with the left code of one of the offered pairs (stateJson "spdifopts").
   *  Linked aspects of a Link group: {"linkGroup":"bus"|"A".."D", "mute"|"filters"|"gain"|"delay":bool}
   *  ("bus" is the Buses' Link group, A..D the Outputs'). Buses cannot link delay. A Channel
   *  joining a Link group takes over its linked aspects, so a patch that joins cannot also set them.
   *  Slots sit on the Channel's biquads in the order added, each taking cost() of them: Butterworth
   *  order n = ceil(n/2), Linkwitz-Riley order n = n/2, everything else 1. An add or a type/order
   *  change that would exceed the budget is rejected.
   *  An optional "preset" field names the Preset the patch was made against; if it is not
   *  the active one, or a Preset switch is in progress, the patch is a conflict.
   *  A rejected patch changes nothing. */
  MatrixEditResult apply( JsonObject patch, uint32_t nowMs );

  /*! Reject live edits between these calls. The firmware closes this gate
   *  before a Preset switch enters the worker queue, so an edit from the old
   *  Preset cannot be queued behind the selected Preset's full write set.
   *  begin returns false (and changes nothing) if a switch is already in progress. */
  bool beginPresetSwitch( void );
  void endPresetSwitch( void ) { presetSwitching_ = false; }

  /*! The Preset the state belongs to; reported by stateJson() and checked by apply(). */
  void setPreset( int preset ) { preset_ = preset; }
  int preset( void ) const { return preset_; }

  /*! Reverts the last accepted apply() (model only, not the DSP writes), e.g. when
   *  the writes could not be queued. A second call does nothing. */
  void undo( void );

  /*! Full state for the web page (Channels plus the selectable lists). */
  std::string stateJson( void ) const;

  /*! Coefficients of a Channel's active (not bypassed) biquads for the plot:
   *  {"fs":48000,"bq":[[b0,b1,b2,a1,a2],...]}, rounded to the DSP's 8.24 grid;
   *  H(z) = (b0+b1 z^-1+b2 z^-2) / (1-a1 z^-1-a2 z^-2). "" if there is no such Channel. */
  std::string coeffsJson( const char* ch, int idx ) const;

  /*! Versioned preset JSON, and its inverse. load() returns "" or the reason
   *  and, on success, writes everything to the DSP. */
  std::string savePreset( void ) const;
  std::string loadPreset( const std::string& json );

  /*! Writes the whole state to the DSP. */
  void applyAll( void );

  /*! Back to the defaults, written to the DSP (preset slot without a file). */
  void loadDefaults( void );

  /*! True once, MATRIX_SAVE_DELAY_MS after the last change; markSaved() clears it. */
  bool saveDue( uint32_t nowMs ) const;
  bool dirty( void ) const { return dirty_; }
  void markSaved( void );

private:
  void reset( void );
  void saveUndo( void );
  std::string applyPatch( JsonObject patch, uint32_t nowMs );
  std::string applyLinkGroup( JsonObject patch, uint32_t nowMs );

  enum SlotType { SLOT_PEQ, SLOT_HP, SLOT_LP, SLOT_LSHELF, SLOT_HSHELF, SLOT_PHASE };

  struct Slot
  {
    bool bypass;
    SlotType type;
    bool lr;     // HP/LP: Linkwitz-Riley instead of Butterworth
    int order;   // HP/LP: Butterworth 1-8, Linkwitz-Riley 2, 4, 8
    float freq, q, gain;
    float slope;  // Shelves
  };

  static int cost( const Slot& s );
  static int choiceCount( void );
  static Slot choice( int n );  // the n-th selectable type/order, other fields default
  static const char* paramOutOfRange( const Slot& s );  // NULL if freq, q, gain, slope are in range
  static const char* normalizeSlot( Slot& s );  // NULL if the final Slot invariants hold
  static int slotCoefs( const Slot& s, MatrixCoef* out );

  struct Channel
  {
    std::string name;
    bool mute;
    float gain;  // dB
    bool gainBypass;
    bool polarity;     // Outputs only
    int delay;         // samples, Outputs only
    bool delayBypass;  // Outputs only
    int sel;    // Bus: Source code; Output: route
    int link;   // -1 = none; Bus uses 0, Outputs use 0..3 (A..D)
    std::vector<Slot> slots;
  };

  struct LinkGroup
  {
    bool mute, filters, gain, delay;
  };

  typedef MatrixCoef Coef;

  double linear( const Channel& c ) const;
  static int used( const Channel& c );  // biquads taken by the Slots
  int slotCount( void ) const;
  std::string applySlot( JsonObject op, Channel& c ) const;
  void stages( const Channel& c, std::vector<Coef>& out ) const;
  void writeStages( MatrixChannelKind kind, int idx, const Channel& c, const Channel* prev );
  struct Snapshot
  {
    Channel bus[MATRIX_MAX_BUS];
    Channel out[MATRIX_MAX_OUT];
  };
  bool needsRamp( const Channel& before, const Channel& after ) const;
  void snapshot( Snapshot& s ) const;
  void writeChanges( const Snapshot& before );
  void putChannel( JsonArray arr, const Channel& c, const char* selKey, bool withLimits ) const;
  std::string readChannel( JsonObject o, bool isBus, Channel& c ) const;

  MatrixPort& port_;
  int nbus_, nout_, nbiquad_;
  int maxDelay_;  // samples: the plugin's maxdly, at most MATRIX_MAX_DELAY_SAMPLES
  Channel bus_[MATRIX_MAX_BUS];
  Channel out_[MATRIX_MAX_OUT];
  LinkGroup busLink_;
  LinkGroup outLink_[MATRIX_OUT_LINKS];
  uint32_t spdif_;  // left code of the SPDIF out pair
  bool dirty_;
  uint32_t lastChange_;
  bool presetSwitching_;
  int preset_;

  struct Undo
  {
    bool valid;
    uint32_t spdif;
    Channel bus[MATRIX_MAX_BUS];
    Channel out[MATRIX_MAX_OUT];
    LinkGroup busLink;
    LinkGroup outLink[MATRIX_OUT_LINKS];
    bool dirty;
    uint32_t lastChange;
  } undo_;
};

#endif
