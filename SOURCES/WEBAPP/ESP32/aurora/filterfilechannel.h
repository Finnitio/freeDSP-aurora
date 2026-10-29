#ifndef FILTERFILECHANNEL_H_
#define FILTERFILECHANNEL_H_

#include <stddef.h>
#include <stdint.h>
#include <string>
#include <vector>
#include "filterfile.h"
#include "matrixmodel.h"

/*! A Filter file and one Channel of the matrix model: the import, in two steps so that the
 *  costly check runs outside the model lock, and the export. */

/*! Step 1: parses and verifies text against the Channel's limits (ch "bus" or "out": no delay or
 *  polarity on a Bus, the Biquad budget, the Plugin's delay limit). Needs no lock: it reads only
 *  limits fixed at construction. MATRIX_EDIT_OK, or MATRIX_EDIT_BAD_REQUEST with the errors. */
MatrixEditStatus parseFilterFileFor( const MatrixModel& model, const char* ch, const char* text, size_t len,
                                     FilterFileRow& row, std::vector<std::string>& errors );

/*! Step 2: replaces the Channel's Filter row with row as one patch made against Preset `preset`,
 *  exactly like a manual edit (MatrixModel::apply): Link group, Autosave, conflict. */
MatrixEditResult applyFilterFile( MatrixModel& model, const char* ch, int idx, int preset, const FilterFileRow& row,
                                  uint32_t nowMs );

/*! Filter file export of a Filter row (MatrixModel::filterRow()) of a Bus or an Output
 *  (writeFilterFile()). The model keeps single-precision values; each is written as the shortest
 *  decimal that reads back as the same float, so an import of the text gives the same Channel
 *  (apart from gain and delay bypass, which an import switches off). Needs no model lock. */
std::string writeExportedFilterFile( FilterFileRow row, bool bus );

/*! Filter file export: text receives Channel ch ("bus" or "out") idx as a Filter file
 *  (filterRow() and writeExportedFilterFile()). Read-only: call under the model lock.
 *  MATRIX_EDIT_OK, or MATRIX_EDIT_BAD_REQUEST for an unknown Channel. */
MatrixEditStatus exportFilterFile( const MatrixModel& model, const char* ch, int idx, std::string& text );

#endif
