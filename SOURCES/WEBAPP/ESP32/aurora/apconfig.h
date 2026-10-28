#ifndef APCONFIG_H_
#define APCONFIG_H_

#include <stddef.h>
#include <stdio.h>
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
/*! Returns NULL if the credentials for joining a WiFi (STA) are valid, else a
 *  message for the UI. An empty SSID forgets the network (the password is
 *  then ignored), an empty password joins an open network.
 */
static inline const char* staConfigError( const char* ssid, const char* pwd )
{
  const size_t ssidLen = strlen( ssid );
  const size_t pwdLen = strlen( pwd );

  if( ssidLen > 32 )
    return "WiFi name can have at most 32 characters";
  if( ssidLen > 0 && ( ( pwdLen > 0 && pwdLen < 8 ) || pwdLen > 63 ) )
    return "WiFi password needs 8 to 63 characters, or none for an open network";
  return NULL;
}

//------------------------------------------------------------------------------
/*! How the AP is started from the stored settings, which may be invalid
 *  (older file, edited by hand). Name and channel fall back to their
 *  defaults. An invalid password falls back to an open AP so the web UI
 *  stays reachable (docs/adr: open AP fallback); the stored password is
 *  left untouched.
 */
struct ApPlan
{
  const char* name;
  int channel;
  const char* pwd;        //!< NULL = open AP
  bool openFallback;      //!< open only because the stored password is invalid
  const char* nameError;  //!< why name/channel were defaulted, or NULL
  const char* pwdError;   //!< why the password was dropped, or NULL
};

static inline ApPlan apPlan( const char* pwd, const char* name, int channel, const char* defaultName )
{
  ApPlan plan = { name, channel, NULL, false, NULL, NULL };
  plan.nameError = apConfigError( "", name, channel );
  if( plan.nameError )
  {
    plan.name = defaultName;
    plan.channel = AP_CHANNEL_DEFAULT;
  }
  plan.pwdError = apConfigError( pwd, plan.name, plan.channel );
  plan.openFallback = plan.pwdError != NULL;
  if( !plan.openFallback && pwd[0] != '\0' )
    plan.pwd = pwd;
  return plan;
}

//------------------------------------------------------------------------------
/*! Bottom line of the display: the IP, or a warning while the AP runs open
 *  because the stored password is invalid. Fits 25 characters (5x7 font).
 */
static const size_t AP_STATUS_LINE_SIZE = 26;

static inline void apStatusLine( char* line, size_t size, const char* ip, bool connected, bool openFallback )
{
  if( !openFallback )
    snprintf( line, size, "%s", ip );
  else if( connected )
    snprintf( line, size, "OPEN AP! %s", ip );
  else
    snprintf( line, size, "OPEN AP: invalid password" );
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
