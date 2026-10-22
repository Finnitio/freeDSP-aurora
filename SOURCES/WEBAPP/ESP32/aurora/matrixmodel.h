#ifndef MATRIXMODEL_H_
#define MATRIXMODEL_H_

#include <stdint.h>
#include <string>
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
};

#define MATRIX_MAX_BUS 2
#define MATRIX_MAX_OUT 8
#define MATRIX_NAME_LEN 16
#define MATRIX_SAVE_DELAY_MS 2000

class MatrixModel
{
public:
  /*! Starts with the defaults (no writes): Bus 1 = Analog 1, Bus 2 = Analog 2,
   *  Outputs alternate Bus 1 / Bus 2, all Outputs muted, 0 dB. */
  MatrixModel( MatrixPort& port, int nbus, int nout );

  /*! Applies a patch {"ch":"bus"|"out","idx":n, name, mute, gain, source (Bus), route (Output)}.
   *  Returns "" on success, else the rejection reason; a rejected patch changes nothing. */
  std::string apply( JsonObject patch, uint32_t nowMs );

  /*! Full state for the web page (Channels plus the selectable lists). */
  std::string stateJson( void ) const;

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

  struct Channel
  {
    std::string name;
    bool mute;
    float gain;  // dB
    int sel;     // Bus: Source code; Output: route
  };

  double linear( const Channel& c ) const;

  MatrixPort& port_;
  int nbus_, nout_;
  Channel bus_[MATRIX_MAX_BUS];
  Channel out_[MATRIX_MAX_OUT];
  bool dirty_;
  uint32_t lastChange_;
};

#endif
