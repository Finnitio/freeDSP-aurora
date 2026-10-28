// Host tests for the global master volume of matrix Plugins: the stored value
// read at boot and the debounced decision when to store a change.
// Run: SOURCES/WEBAPP/ESP32/test/run.sh (runs all host tests)
#include <cassert>
#include <cstdio>
#include "../aurora/mastervolume.h"

static void missingStoredValueGivesSafeDefault()
{
  assert( parseStoredVolume( nullptr ) == -60.0f );
  assert( parseStoredVolume( "" ) == -60.0f );
}

static void storedValueIsRestored()
{
  assert( parseStoredVolume( "-23.5" ) == -23.5f );
  assert( parseStoredVolume( "-23.5\n" ) == -23.5f );
  assert( parseStoredVolume( "0" ) == 0.0f );
  assert( parseStoredVolume( "-120" ) == -120.0f );
}

// A broken file must not come back louder than the default.
static void invalidStoredValueGivesSafeDefault()
{
  assert( parseStoredVolume( "loud" ) == -60.0f );
  assert( parseStoredVolume( "-12dB" ) == -60.0f );
  assert( parseStoredVolume( "3" ) == -60.0f );
  assert( parseStoredVolume( "-121" ) == -60.0f );
  assert( parseStoredVolume( "nan" ) == -60.0f );
}

static void changeIsStoredAfterTheDebounce()
{
  MasterVolumeSaver saver( -60.0f );
  assert( !saver.saveDue( 0 ) );
  saver.changed( -40.0f, 1000 );
  assert( !saver.saveDue( 2999 ) );
  assert( saver.saveDue( 3000 ) );
  assert( saver.value() == -40.0f );
  saver.markSaved( -40.0f );
  assert( !saver.saveDue( 10000 ) );
}

// Turning the encoder or dragging the slider stores once, the last value.
static void eachChangeRestartsTheDebounce()
{
  MasterVolumeSaver saver( -60.0f );
  saver.changed( -59.5f, 1000 );
  saver.changed( -59.0f, 2500 );
  saver.changed( -58.5f, 4000 );
  assert( !saver.saveDue( 5999 ) );
  assert( saver.saveDue( 6000 ) );
  assert( saver.value() == -58.5f );
}

static void returningToTheStoredValueStoresNothing()
{
  MasterVolumeSaver saver( -30.0f );
  saver.changed( -29.5f, 1000 );
  saver.changed( -30.0f, 1200 );
  assert( !saver.saveDue( 10000 ) );
}

// A change while the store is under way is stored again afterwards.
static void changeAfterTheValueWasTakenIsStoredToo()
{
  MasterVolumeSaver saver( -60.0f );
  saver.changed( -40.0f, 0 );
  assert( saver.saveDue( 2000 ) );
  float taken = saver.value();
  saver.changed( -35.0f, 2100 );
  saver.markSaved( taken );
  assert( !saver.saveDue( 4099 ) );
  assert( saver.saveDue( 4100 ) );
  assert( saver.value() == -35.0f );
}

static void millisWrapAroundStillStores()
{
  MasterVolumeSaver saver( -60.0f );
  saver.changed( -40.0f, 0xFFFFFF00u );
  assert( !saver.saveDue( 0x00000100u ) );
  assert( saver.saveDue( 0xFFFFFF00u + 2000u ) );
}

int main()
{
  changeIsStoredAfterTheDebounce();
  eachChangeRestartsTheDebounce();
  returningToTheStoredValueStoresNothing();
  changeAfterTheValueWasTakenIsStoredToo();
  millisWrapAroundStillStores();
  missingStoredValueGivesSafeDefault();
  storedValueIsRestored();
  invalidStoredValueGivesSafeDefault();
  printf( "test_mastervolume: all passed\n" );
  return 0;
}
