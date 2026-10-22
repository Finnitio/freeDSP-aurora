#ifndef APCONFIG_H_
#define APCONFIG_H_

#include <stddef.h>
#include <string.h>

//==============================================================================
/*! Pure logic for the access point, testable on the host.
 */

static const int AP_CHANNEL_DEFAULT = 6;

//------------------------------------------------------------------------------
/*! Returns NULL if the AP settings are valid, else a message for the UI.
 *  An empty password means an open AP.
 */
static inline const char* apConfigError( const char* pwd, const char* name, int channel )
{
  const size_t pwdLen = strlen( pwd );
  const size_t nameLen = strlen( name );

  if( pwdLen > 0 && pwdLen < 8 )
    return "AP password needs at least 8 characters";
  if( pwdLen > 63 )
    return "AP password can have at most 63 characters";
  if( nameLen == 0 || nameLen > 32 )
    return "AP name needs 1 to 32 characters";
  if( channel < 1 || channel > 13 )
    return "AP channel must be between 1 and 13";
  return NULL;
}

//------------------------------------------------------------------------------
/*! Answer to a connectivity-check URL that makes the phone think it has
 *  internet, so it stays on the WLAN.
 */
struct CaptiveAnswer
{
  const char* path;
  int status;
  const char* type;
  const char* body;
};

static inline const CaptiveAnswer* captiveAnswer( const char* path )
{
  static const CaptiveAnswer answers[] =
  {
    { "/generate_204",  204, "text/plain", "" },  // Android
    { "/gen_204",       204, "text/plain", "" },  // Android, Chrome
    { "/hotspot-detect.html", 200, "text/html",
      "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>" },  // iOS, macOS
    { "/library/test/success.html", 200, "text/html",
      "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>" },  // older iOS
    { "/connecttest.txt", 200, "text/plain", "Microsoft Connect Test" },  // Windows 10+
    { "/ncsi.txt",        200, "text/plain", "Microsoft NCSI" },          // Windows
    { "/success.txt",     200, "text/plain", "success\n" },               // Firefox
  };
  for( size_t i = 0; i < sizeof(answers) / sizeof(answers[0]); i++ )
    if( strcmp( path, answers[i].path ) == 0 )
      return &answers[i];
  return NULL;
}

#endif
