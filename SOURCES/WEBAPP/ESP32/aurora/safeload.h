#ifndef SAFELOAD_H_
#define SAFELOAD_H_

#include <stddef.h>
#include <stdint.h>

/*! Atomic 5-word (one biquad) parameter write through the ADAU1452 software
 *  safeload module of the SigmaStudio program. No Arduino or I2C dependencies.
 *
 *  Sequence: the 5 words go into data_SafeLoad[0..4] (consecutive parameter
 *  addresses, one burst), then the target address goes into address_SafeLoad.
 *  That last write arms the transfer: the DSP copies the 5 words to
 *  target..target+4 at a frame boundary, so the filter never runs with a mix
 *  of old and new coefficients.
 */

/*! Parameter-RAM write of len bytes, auto-incrementing, one I2C transaction. */
struct SafeloadPort
{
  virtual void write( uint16_t reg, const uint8_t* data, size_t len ) = 0;
};

/*! Addresses of the safeload module (plugin.ini "safeload"). 0 = not available. */
struct SafeloadCfg
{
  uint16_t data;  //!< data_SafeLoad (first of 5 words)
  uint16_t addr;  //!< address_SafeLoad
};

static inline void safeloadPutWord( uint8_t* out, uint32_t w )
{
  out[0] = ( w >> 24 ) & 0xFF;
  out[1] = ( w >> 16 ) & 0xFF;
  out[2] = ( w >> 8 ) & 0xFF;
  out[3] = w & 0xFF;
}

/*! Writes 5 words (B2 B1 B0 A2 A1) to target..target+4. Without safeload
 *  addresses (older plugin.ini) they are written directly, one by one. */
static inline void safeloadBiquad( SafeloadPort& port, const SafeloadCfg& cfg,
                                   uint16_t target, const uint32_t words[5] )
{
  uint8_t buf[20];
  for( int ii = 0; ii < 5; ii++ )
    safeloadPutWord( buf + 4 * ii, words[ii] );

  if( cfg.data == 0 || cfg.addr == 0 )
  {
    for( int ii = 0; ii < 5; ii++ )
      port.write( target + ii, buf + 4 * ii, 4 );
    return;
  }

  port.write( cfg.data, buf, 20 );
  uint8_t tgt[4];
  safeloadPutWord( tgt, target );
  port.write( cfg.addr, tgt, 4 );
}

#endif
