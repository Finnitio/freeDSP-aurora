#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "filterimport.h"

static bool channelKind( const char* ch, bool& bus )
{
  if( !ch )
    return false;
  bus = strcmp( ch, "bus" ) == 0;
  return bus || strcmp( ch, "out" ) == 0;
}

MatrixEditStatus parseFilterFileFor( const MatrixModel& model, const char* ch, const char* text, size_t len,
                                     FilterFileRow& row, std::vector<std::string>& errors )
{
  bool bus;
  if( !channelKind( ch, bus ) )
  {
    errors.assign( 1, "unknown channel kind" );
    return MATRIX_EDIT_BAD_REQUEST;
  }
  FilterFileLimits limits = { bus, model.biquads(), model.maxDelay() };
  errors = parseFilterFile( text, len, limits, row );
  return errors.empty() ? MATRIX_EDIT_OK : MATRIX_EDIT_BAD_REQUEST;
}

MatrixEditResult applyFilterFile( MatrixModel& model, const char* ch, int idx, int preset, const FilterFileRow& row,
                                  uint32_t nowMs )
{
  bool bus;
  if( !channelKind( ch, bus ) )
    return { MATRIX_EDIT_BAD_REQUEST, "unknown channel kind" };
  DynamicJsonDocument doc( filterFilePatchCapacity( row ) );
  JsonObject patch = doc.to<JsonObject>();
  patch["ch"] = bus ? "bus" : "out";
  patch["idx"] = idx;
  patch["preset"] = preset;
  if( doc.capacity() == 0 || !filterFilePatch( row, bus, patch ) )
    return { MATRIX_EDIT_BAD_REQUEST, "out of memory" };
  return model.apply( patch, nowMs );
}

/*! The double of the shortest decimal that reads back as the float v: 0.7f gives 0.7. */
static double decimal( double v )
{
  const float f = (float)v;
  char buf[32];
  for( int digits = 1; digits <= 9; digits++ )
  {
    snprintf( buf, sizeof( buf ), "%.*g", digits, (double)f );
    if( strtof( buf, NULL ) == f )
      break;
  }
  return strtod( buf, NULL );
}

MatrixEditStatus exportFilterFile( const MatrixModel& model, const char* ch, int idx, std::string& text )
{
  FilterFileRow row;
  bool bus;
  if( !channelKind( ch, bus ) || !model.filterRow( ch, idx, row ) )
    return MATRIX_EDIT_BAD_REQUEST;
  row.gain = decimal( row.gain );
  for( size_t i = 0; i < row.blocks.size(); i++ )
  {
    FilterFileBlock& b = row.blocks[i];
    b.f = decimal( b.f );
    b.gain = decimal( b.gain );
    b.q = decimal( b.q );
    b.s = decimal( b.s );
  }
  text = writeFilterFile( row, bus );
  return MATRIX_EDIT_OK;
}
