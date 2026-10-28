#ifndef APSWITCH_H_
#define APSWITCH_H_

#include <stdint.h>

//==============================================================================
/*! Decides when the AP is switched off and on again (setting "AP off while
 *  connected"). Pure logic, no WiFi calls, so it can be tested
 *  on the host.
 *
 *  With the option off the AP is always up. With the option on the AP stops
 *  once the WiFi (STA) connection has lasted GRACE_MS, and starts again as
 *  soon as the connection is lost or the option is switched off. A board that
 *  never connects keeps its AP. The AP is assumed up at construction; build a
 *  fresh ApSwitch whenever the caller (re)starts the AP itself.
 */
class ApSwitch
{
public:
  enum Action { NONE, START, STOP };

  //! Time to read the WiFi IP (shown on the display and in the WiFi dialog)
  //! before the AP goes away.
  static const uint32_t GRACE_MS = 30000;

  //----------------------------------------------------------------------------
  /*! Call periodically. Returns what the caller must do with the AP.
   */
  Action update( bool apOffWhileConnected, bool connected, uint32_t nowMs )
  {
    if( !apOffWhileConnected || !connected )
    {
      counting = false;
      if( up )
        return NONE;
      up = true;
      return START;
    }

    if( !up )
      return NONE;
    if( !counting )
    {
      counting = true;
      sinceMs = nowMs;
      return NONE;
    }
    if( nowMs - sinceMs < GRACE_MS )
      return NONE;
    up = false;
    return STOP;
  }

  bool apUp() const { return up; }

private:
  bool up = true;
  bool counting = false;  //!< option on and connected since sinceMs
  uint32_t sinceMs = 0;
};

#endif
