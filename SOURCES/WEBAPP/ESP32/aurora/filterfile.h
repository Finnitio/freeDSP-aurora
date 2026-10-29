#ifndef FILTERFILE_H_
#define FILTERFILE_H_

#include <stddef.h>
#include <string>
#include <vector>
#include <ArduinoJson.h>

/*! Filter file import (docs/filter-file.md): parses and verifies the text of a Filter file and
 *  turns it into one matrix patch for one Channel. The reference is
 *  SOURCES/filterfile/aurorafilter.py; the error texts are the same, so the host test runs the
 *  shared cases of SOURCES/filterfile/cases.json. No Arduino dependencies.
 */

#define FILTERFILE_MAX_BYTES 16384
#define FILTERFILE_STR_( x ) #x
#define FILTERFILE_STR( x ) FILTERFILE_STR_( x )
/*! The one error of a file larger than FILTERFILE_MAX_BYTES. */
#define FILTERFILE_TOO_LARGE "file larger than " FILTERFILE_STR( FILTERFILE_MAX_BYTES ) " bytes"
#define FILTERFILE_MAX_ERRORS 20

enum FilterFileType { FILTERFILE_PEQ, FILTERFILE_LSHELF, FILTERFILE_HSHELF, FILTERFILE_PHASE, FILTERFILE_HP, FILTERFILE_LP };

/*! "PEQ", "LSHELF", "HSHELF", "PHASE", "HP" or "LP". */
const char* filterFileTypeName( FilterFileType type );

/*! One filter block, i.e. one Slot. */
struct FilterFileBlock
{
  int line;
  FilterFileType type;
  bool lr;     // HP/LP: Linkwitz-Riley (order 2, 4, 8) instead of Butterworth (1-8)
  int order;
  double f, gain, q, s;  // the parameters the type takes; the others are 0
  bool bypass;
};

/*! What the target Channel allows. */
struct FilterFileLimits
{
  bool bus;             // a Bus: no delay, no polarity
  int biquads;          // the Channel's Biquad budget; < 0: unchecked
  int maxDelaySamples;  // the Plugin's Output delay limit
};

/*! An accepted file: the Channel's whole Filter row. */
struct FilterFileRow
{
  bool hasName;  // false: keep the current name
  std::string name;
  double gain;   // dB
  int delay;     // samples
  bool inverted;
  std::vector<FilterFileBlock> blocks;
};

/*! Parses and verifies text (len bytes, need not be NUL-terminated). Returns the errors, at most
 *  FILTERFILE_MAX_ERRORS of "line N: ..." or a message without a line; none: row holds the file.
 *  Blocks beyond the Biquad budget are counted but not verified (the file is rejected anyway),
 *  which bounds the work for a large file. */
std::vector<std::string> parseFilterFile( const char* text, size_t len, const FilterFileLimits& limits, FilterFileRow& row );

/*! v in the fewest decimals that read back as v ("30", "-3.3"); singlePrecision: as the float
 *  (float)v, so 0.7f gives "0.7". %.17g if that needs too many. */
std::string filterFileDecimal( double v, bool singlePrecision );

/*! The text of row as a Filter file for a Bus (no delay, no polarity) or an Output: every line
 *  written, the coefficients Aurora computes from the parameters with %.17g, the other numbers in
 *  the fewest digits that read back as the same double. The name is left out if the file cannot
 *  carry it (empty, too long, control characters, surrounding blanks, not UTF-8). */
std::string writeFilterFile( const FilterFileRow& row, bool bus );

/*! {"errors":[...]}, the reply to a rejected import. */
std::string filterFileErrorsJson( const std::vector<std::string>& errors );

/*! JsonDocument capacity for filterFilePatch() plus "ch", "idx" and "preset". */
size_t filterFilePatchCapacity( const FilterFileRow& row );

/*! The matrix patch fields (MatrixModel::apply) that replace a Channel's Filter row: name (if the
 *  file has one), gain, gainBypass off, on an Output polarity, delayUs and delayBypass off, and
 *  "slots", the whole list of Slots. The caller adds "ch", "idx" and "preset". The patch refers
 *  to row's name, so row must outlive it. False if the document was too small (see
 *  filterFilePatchCapacity()). */
bool filterFilePatch( const FilterFileRow& row, bool bus, JsonObject patch );

#endif
