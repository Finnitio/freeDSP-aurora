#ifndef BODYBUFFER_H_
#define BODYBUFFER_H_

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum BodyResult { BODY_MORE, BODY_COMPLETE, BODY_REJECTED };

//==============================================================================
/*! Reassembles a request body that arrives in chunks.
 *  buf starts as NULL, is malloc'ed on the first chunk (index 0) and holds the
 *  zero-terminated body once BODY_COMPLETE is returned. The caller frees it.
 *  Pure logic, no web server calls, so it can be tested on the host.
 */
inline BodyResult bodyChunk( char*& buf, const uint8_t* data, size_t len, size_t index, size_t total, size_t maxTotal )
{
  if( index == 0 )
  {
    free( buf );
    buf = NULL;
    if( total > maxTotal )
      return BODY_REJECTED;
    buf = (char*)malloc( total + 1 );
    if( !buf )
      return BODY_REJECTED;
    buf[total] = 0;
  }
  if( !buf || index + len > total )
    return BODY_REJECTED;

  memcpy( buf + index, data, len );
  return ( index + len == total ) ? BODY_COMPLETE : BODY_MORE;
}

#endif
