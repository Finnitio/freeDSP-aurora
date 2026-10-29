#ifndef PRESETSLOTS_H_
#define PRESETSLOTS_H_

#include <stdint.h>
#include <string>
#include <ArduinoJson.h>
#include "config.h"

//==============================================================================
/*! Path of the matrix Preset file of slot `preset` (0-based): /matrix.001.json
 *  for slot A. Pure logic, so it can be tested on the host.
 */
inline std::string matrixPresetFileName( uint8_t preset )
{
  static_assert( NUMPRESETS < 10, "matrixPresetFileName() writes the slot as one digit" );
  return std::string( "/matrix.00" ) + (char)( '1' + preset ) + ".json";  // no std::to_string on the ESP32 core
}

//==============================================================================
/*! Body of GET /preset: "pre" the active slot, "used" per slot whether its
 *  matrix Preset file exists. exists( const std::string& path ) asks the file
 *  system (SPIFFS on the device).
 */
template <class Exists>
void presetSlotsJson( JsonVariant root, int pre, Exists exists )
{
  root["pre"] = pre;
  JsonArray used = root.createNestedArray( "used" );
  for( uint8_t ii = 0; ii < NUMPRESETS; ii++ )
    used.add( (bool)exists( matrixPresetFileName( ii ) ) );
}

/*! The same for the matrix Plugin, plus "names": the Preset name of each slot
 *  ("" = none). name( uint8_t slot ) returns it as a std::string.
 */
template <class Exists, class Name>
void presetSlotsJson( JsonVariant root, int pre, Exists exists, Name name )
{
  presetSlotsJson( root, pre, exists );
  JsonArray names = root.createNestedArray( "names" );
  for( uint8_t ii = 0; ii < NUMPRESETS; ii++ )
  {
    std::string n = name( ii );
    names.add( &n[0] );  // char*: ArduinoJson copies it (no std::string support on the ESP32)
  }
}

#endif
