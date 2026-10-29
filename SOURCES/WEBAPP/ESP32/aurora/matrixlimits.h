#ifndef MATRIXLIMITS_H_
#define MATRIXLIMITS_H_

/*! Limits of a matrix Channel that the matrix model and the Filter file share. No dependencies,
 *  so filterfile.cpp needs no matrix model to know them. */

#define MATRIX_SAMPLE_RATE 48000
#define MATRIX_NAME_LEN 16       // bytes of UTF-8: Channel and Preset names
#define MATRIX_GAIN_MIN_DB -80   // Channel gain
#define MATRIX_GAIN_MAX_DB 12

#endif
