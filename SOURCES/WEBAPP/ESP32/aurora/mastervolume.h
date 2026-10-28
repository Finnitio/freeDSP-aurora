#ifndef MASTERVOLUME_H_
#define MASTERVOLUME_H_

//==============================================================================
/*! Master volume of a matrix Plugin is global: not part of a Preset, stored in
 *  its own file, restored at boot and left alone by a Preset switch. Pure
 *  logic, so it can be tested on the host.
 */

#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>

#define MASTER_VOLUME_DEFAULT_DB -60.0f
#define MASTER_VOLUME_MIN_DB -120.0f
#define MASTER_VOLUME_MAX_DB 0.0f

//------------------------------------------------------------------------------
/*! The volume stored as text; the safe default if there is none or it is not a
 *  valid volume.
 */
inline float parseStoredVolume( const char* text )
{
  if( !text )
    return MASTER_VOLUME_DEFAULT_DB;
  char* end;
  float db = strtof( text, &end );
  if( end == text )
    return MASTER_VOLUME_DEFAULT_DB;
  while( isspace( (unsigned char)*end ) )
    end++;
  // the range setMasterVolume() accepts; the comparison is false for NaN
  if( *end != '\0' || !( db >= MASTER_VOLUME_MIN_DB && db <= MASTER_VOLUME_MAX_DB ) )
    return MASTER_VOLUME_DEFAULT_DB;
  return db;
}

//------------------------------------------------------------------------------
/*! Decides when to store the volume: MASTER_VOLUME_SAVE_DELAY_MS after the last
 *  change (like Autosave), and only if it differs from the stored value, so
 *  turning the encoder or dragging the slider does not wear the flash.
 */
#define MASTER_VOLUME_SAVE_DELAY_MS 2000

class MasterVolumeSaver
{
public:
  explicit MasterVolumeSaver( float storedDb ) : stored_( storedDb ), value_( storedDb ), lastChange_( 0 ) {}

  void changed( float db, uint32_t nowMs )
  {
    value_ = db;
    lastChange_ = nowMs;
  }

  bool saveDue( uint32_t nowMs ) const
  {
    return value_ != stored_ && (uint32_t)( nowMs - lastChange_ ) >= MASTER_VOLUME_SAVE_DELAY_MS;
  }

  /*! The value to store once saveDue(). */
  float value( void ) const { return value_; }

  /*! db is now on flash (the value taken when saveDue(); a later change stays due). */
  void markSaved( float db ) { stored_ = db; }

private:
  float stored_;
  float value_;
  uint32_t lastChange_;
};

#endif
