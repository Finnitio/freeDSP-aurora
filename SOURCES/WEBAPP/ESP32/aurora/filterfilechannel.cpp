#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "filterfilechannel.h"

MatrixEditStatus parseFilterFileFor( const MatrixModel& model, const char* ch, const char* text, size_t len,
                                     FilterFileRow& row, std::vector<std::string>& errors )
{
  MatrixChannelKind kind;
  if( !parseChannelKind( ch, kind ) )
  {
    errors.assign( 1, "unknown channel kind" );
    return MATRIX_EDIT_BAD_REQUEST;
  }
  FilterFileLimits limits = { kind == MATRIX_BUS, model.biquads(), model.maxDelay() };
  errors = parseFilterFile( text, len, limits, row );
  return errors.empty() ? MATRIX_EDIT_OK : MATRIX_EDIT_BAD_REQUEST;
}

MatrixEditResult applyFilterFile( MatrixModel& model, const char* ch, int idx, int preset, const FilterFileRow& row,
                                  uint32_t nowMs )
{
  MatrixChannelKind kind;
  if( !parseChannelKind( ch, kind ) )
    return { MATRIX_EDIT_BAD_REQUEST, "unknown channel kind" };
  const bool bus = ( kind == MATRIX_BUS );
  DynamicJsonDocument doc( filterFilePatchCapacity( row ) );
  JsonObject patch = doc.to<JsonObject>();
  patch["ch"] = channelKindName( kind );
  patch["idx"] = idx;
  patch["preset"] = preset;
  if( doc.capacity() == 0 || !filterFilePatch( row, bus, patch ) )
    return { MATRIX_EDIT_BAD_REQUEST, "out of memory" };
  return model.apply( patch, nowMs );
}

/*! The double of the shortest decimal that reads back as the float v: 0.7f gives 0.7. */
static double decimal( double v )
{
  return strtod( filterFileDecimal( v, true ).c_str(), NULL );
}

std::string writeExportedFilterFile( FilterFileRow row, bool bus )
{
  row.gain = decimal( row.gain );
  for( size_t i = 0; i < row.blocks.size(); i++ )
  {
    FilterFileBlock& b = row.blocks[i];
    b.f = decimal( b.f );
    b.gain = decimal( b.gain );
    b.q = decimal( b.q );
    b.s = decimal( b.s );
  }
  return writeFilterFile( row, bus );
}

MatrixEditStatus exportFilterFile( const MatrixModel& model, const char* ch, int idx, std::string& text )
{
  FilterFileRow row;
  MatrixChannelKind kind;
  if( !parseChannelKind( ch, kind ) || !model.filterRow( ch, idx, row ) )
    return MATRIX_EDIT_BAD_REQUEST;
  text = writeExportedFilterFile( row, kind == MATRIX_BUS );
  return MATRIX_EDIT_OK;
}
