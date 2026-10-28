#ifndef MATRIX_H_
#define MATRIX_H_

#include <string>
#include <ArduinoJson.h>
#include "matrixmodel.h"

/*! Firmware side of the matrix plugin: owns the MatrixModel, turns its writes
 *  into DSP parameter writes (on the I2C worker) and keeps the preset files.
 */

bool isMatrixPlugin( void );

/*! Creates the model from the plugin meta data and restores the stored master
 *  volume (the safe default if there is none). Call after readPluginMeta() and
 *  before setMasterVolume() writes the volume to the DSP in setup(). */
void matrixInit( void );

/*! Call after every master volume change (encoder or web); matrixLoop() stores
 *  it once it has settled. The volume is global: a Preset switch keeps it. */
void matrixMasterVolumeChanged( void );

/*! Writes the Baseline to the DSP in full, so the model matches the freshly
 *  loaded DSP program. Call once right after the DSP program upload, while the
 *  DAC is still muted (in setup(), before resetDAC(false)). */
void matrixWriteBaseline( void );

/*! Loads the active Preset (or the defaults if there is no file) and writes only
 *  the difference to the DSP, inside one shared gain ramp (MatrixModel::switchPreset);
 *  no DAC mute, no legacy parameter upload. Saves unsaved changes of the previously
 *  active Preset first. Runs on the I2C worker (or in setup()); the DSP writes
 *  happen right away. A switch calls it through applyPresetSwitch(). */
void matrixLoadPreset( void );

/*! Closes/reopens the live-edit gate around a Preset switch. Begin must happen
 *  before the switch is queued, so later web edits are rejected instead of
 *  landing behind the selected Preset's writes. */
bool matrixBeginPresetSwitch( void );
void matrixEndPresetSwitch( void );

/*! Applies a live edit and returns whether it was accepted, a bad request or a
 *  conflict (Preset switch in progress or stale "preset"); on success state is the
 *  new state. The DSP writes are queued for the I2C worker, so this is safe in a
 *  web handler. */
MatrixEditResult matrixApply( JsonObject patch, std::string& state );

/*! Import of an exported Preset file (Export's inverse) into the active Preset, made
 *  against Preset `preset`: the difference inside the shared ramp, then Autosave
 *  (MatrixModel::importPreset). The file's Add-on B config is applied only with Add-on B.
 *  Accepted, bad request or conflict like matrixApply(); the DSP writes are queued for
 *  the I2C worker, so this is safe in a web handler. json (NUL-terminated) is parsed in
 *  place, outside the model lock, and is changed by the call. */
MatrixEditResult matrixImport( char* json, int preset );

/*! Export: the active Preset as a Preset file, including changes Autosave has not stored yet. */
std::string matrixPresetJson( void );

/*! Add-on B (SPDIF mux) config change from POST /addoncfg: a live edit like matrixApply()
 *  (a conflict during a Preset switch; the write is queued for the I2C worker, no DAC
 *  mute), autosaved with the active Preset to /addoncfg.00N. Safe in a web handler. */
MatrixEditResult matrixSetAddOn( uint8_t addr, uint8_t reg, uint8_t data );

std::string matrixState( void );

/*! Active biquad coefficients of one Channel (ch "bus" or "out") for the plot; "" if unknown. */
std::string matrixCoeffs( const char* ch, int idx );

/*! Call from loop(): queues the Autosave of the Preset and the store of the
 *  master volume once the changes have settled. */
void matrixLoop( void );

#endif
