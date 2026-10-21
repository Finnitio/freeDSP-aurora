#ifndef STARETRY_H_
#define STARETRY_H_

#include <stdint.h>

//==============================================================================
/*! Decides when the STA side (re)connects to the saved network.
 *  Pure logic, no WiFi calls, so it can be tested on the host.
 */
class StaRetry
{
public:
  enum Action { NONE, BEGIN, STOP };

  static const uint32_t ATTEMPT_TIMEOUT_MS = 15000;
  static const int MAX_FAILURES = 3;
  static const uint32_t RETRY_PAUSE_MS = 60000;

  //----------------------------------------------------------------------------
  /*! Call periodically. Returns what the caller must do with the STA side.
   */
  Action update( bool connected, uint32_t nowMs )
  {
    if( connected )
    {
      state = CONNECTED;
      failures = 0;
      return NONE;
    }

    switch( state )
    {
    case START:
    case CONNECTED:  // link lost: start a fresh attempt
      return begin( nowMs );

    case CONNECTING:
      if( nowMs - stateSinceMs < ATTEMPT_TIMEOUT_MS )
        return NONE;
      if( ++failures < MAX_FAILURES )
        return begin( nowMs );
      state = STOPPED;
      stateSinceMs = nowMs;
      return STOP;

    case STOPPED:
      if( nowMs - stateSinceMs < RETRY_PAUSE_MS )
        return NONE;
      failures = MAX_FAILURES - 1;  // one attempt, then stop again
      return begin( nowMs );

    default:
      return NONE;
    }
  }

private:
  enum State { START, CONNECTING, CONNECTED, STOPPED };

  Action begin( uint32_t nowMs )
  {
    state = CONNECTING;
    stateSinceMs = nowMs;
    return BEGIN;
  }

  State state = START;
  uint32_t stateSinceMs = 0;
  int failures = 0;
};

#endif
