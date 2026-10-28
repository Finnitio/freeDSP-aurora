// Host tests for the GET /preset body (active slot, which slots have a Preset file).
// Run: SOURCES/WEBAPP/ESP32/test/run.sh (runs all host tests)
// Uses: ArduinoJson
#include <cassert>
#include <cstdio>
#include <set>
#include <string>
#include <ArduinoJson.h>
#include "../aurora/presetslots.h"

static std::string body( int pre, const std::set<std::string>& files )
{
  DynamicJsonDocument doc( 256 );
  JsonVariant root = doc.to<JsonVariant>();
  presetSlotsJson( root, pre, [&]( const std::string& path ) { return files.count( path ) > 0; } );
  std::string out;
  serializeJson( doc, out );
  return out;
}

static void slotsWithoutFileAreUnused()
{
  assert( body( 0, {} ) == "{\"pre\":0,\"used\":[false,false,false,false]}" );
}

static void slotWithFileIsUsed()
{
  assert( body( 2, { "/matrix.002.json", "/matrix.004.json" } ) ==
          "{\"pre\":2,\"used\":[false,true,false,true]}" );
}

// Other files (Add-on configs, misnamed files) do not count.
static void onlyMatrixPresetFilesCount()
{
  assert( body( 1, { "/addoncfg.001", "/matrix.json", "/matrix.01.json" } ) ==
          "{\"pre\":1,\"used\":[false,false,false,false]}" );
}

static void fileNamesFollowTheSlot()
{
  assert( matrixPresetFileName( 0 ) == "/matrix.001.json" );
  assert( matrixPresetFileName( 3 ) == "/matrix.004.json" );
}

int main()
{
  slotsWithoutFileAreUnused();
  slotWithFileIsUsed();
  onlyMatrixPresetFilesCount();
  fileNamesFollowTheSlot();
  printf( "test_presetslots: all passed\n" );
  return 0;
}
