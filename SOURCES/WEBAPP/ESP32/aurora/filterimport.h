#ifndef FILTERIMPORT_H_
#define FILTERIMPORT_H_

#include <stddef.h>
#include <stdint.h>
#include <string>
#include <vector>
#include "filterfile.h"
#include "matrixmodel.h"

/*! Filter file import into one Channel of the matrix model, in two steps so that the costly
 *  check runs outside the model lock. */

/*! Step 1: parses and verifies text against the Channel's limits (ch "bus" or "out": no delay or
 *  polarity on a Bus, the Biquad budget, the Plugin's delay limit). Needs no lock: it reads only
 *  limits fixed at construction. MATRIX_EDIT_OK, or MATRIX_EDIT_BAD_REQUEST with the errors. */
MatrixEditStatus parseFilterFileFor( const MatrixModel& model, const char* ch, const char* text, size_t len,
                                     FilterFileRow& row, std::vector<std::string>& errors );

/*! Step 2: replaces the Channel's Filter row with row as one patch made against Preset `preset`,
 *  exactly like a manual edit (MatrixModel::apply): Link group, Autosave, conflict. */
MatrixEditResult applyFilterFile( MatrixModel& model, const char* ch, int idx, int preset, const FilterFileRow& row,
                                  uint32_t nowMs );

#endif
