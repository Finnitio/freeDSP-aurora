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
 *  before uploadUserParams(), which writes the volume to the DSP. */
void matrixInit( void );

/*! Call after every master volume change (encoder or web); matrixLoop() stores
 *  it once it has settled. The volume is global: a Preset switch keeps it. */
void matrixMasterVolumeChanged( void );

/*! Loads the active preset (or the defaults if there is no file) and writes it to
 *  the DSP. Saves unsaved changes of the previously active preset first. Runs on
 *  the I2C worker (or in setup()); the DSP writes happen right away. */
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

std::string matrixState( void );

/*! Active biquad coefficients of one Channel (ch "bus" or "out") for the plot; "" if unknown. */
std::string matrixCoeffs( const char* ch, int idx );

/*! Call from loop(): queues the auto-save of the Preset and the store of the
 *  master volume once the changes have settled. */
void matrixLoop( void );

#endif
