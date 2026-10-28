#ifndef ADDONS_H_
#define ADDONS_H_
enum taddonid
{
  ADDON_CUSTOM = 0x00,
  ADDON_A = 0x01,
  ADDON_B = 0x02,
  ADDON_C = 0x03,
  ADDON_D = 0x04
};

void updateAddOn( void );

/*! Add-on B (SPDIF mux) config register write; addr is the 8-bit address as stored in
 *  /addoncfg.00N. Becomes currentAddOnCfg. Takes the I2C lock. */
void writeAddOnBConfig( uint8_t addr, uint8_t reg, uint8_t data );

#endif
