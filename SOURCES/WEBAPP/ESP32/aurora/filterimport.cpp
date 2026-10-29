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
