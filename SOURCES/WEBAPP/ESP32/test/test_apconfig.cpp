// Host tests for AP config validation and captive-portal answers.
// Run: SOURCES/WEBAPP/ESP32/test/run.sh (runs all host tests)
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include "../aurora/apconfig.h"

static void validConfigIsAccepted()
{
  assert(apConfigError("secret123", "aurora", 6) == NULL);
  assert(apConfigError("", "aurora", 1) == NULL);            // open AP
  assert(apConfigError("12345678", std::string(32, 'a').c_str(), 13) == NULL);
}

static void shortPasswordIsRejected()
{
  const char* msg = apConfigError("1234567", "aurora", 6);
  assert(msg != NULL && strstr(msg, "8") != NULL);
}

static void badNameIsRejected()
{
  assert(apConfigError("", "", 6) != NULL);
  assert(apConfigError("", std::string(33, 'a').c_str(), 6) != NULL);
}

static void channelOutsideOneToThirteenIsRejected()
{
  assert(apConfigError("", "aurora", 0) != NULL);
  assert(apConfigError("", "aurora", 14) != NULL);
  assert(apConfigError("", "aurora", 13) == NULL);
}

static void connectivityChecksKeepPhonesOnTheAp()
{
  const CaptiveAnswer* a = captiveAnswer("/generate_204");   // Android
  assert(a && a->status == 204);
  a = captiveAnswer("/gen_204");
  assert(a && a->status == 204);
  a = captiveAnswer("/hotspot-detect.html");                 // iOS / macOS
  assert(a && a->status == 200 && strstr(a->body, "Success") != NULL);
  a = captiveAnswer("/connecttest.txt");                     // Windows
  assert(a && a->status == 200 && strcmp(a->body, "Microsoft Connect Test") == 0);
  a = captiveAnswer("/ncsi.txt");
  assert(a && a->status == 200 && strcmp(a->body, "Microsoft NCSI") == 0);
  assert(captiveAnswer("/preset") == NULL);                  // normal paths untouched
}

static void validSettingsStartTheApAsStored()
{
  ApPlan p = apPlan("secret123", "mydsp", 11, "aurora");
  assert(strcmp(p.name, "mydsp") == 0 && p.channel == 11);
  assert(p.pwd != NULL && strcmp(p.pwd, "secret123") == 0);
  assert(!p.openFallback && p.pwdError == NULL && p.nameError == NULL);

  p = apPlan("", "mydsp", 3, "aurora");                     // open by choice
  assert(p.pwd == NULL && !p.openFallback);
}

static void invalidPasswordFallsBackToAnOpenAp()
{
  ApPlan p = apPlan("short", "mydsp", 11, "aurora");
  assert(strcmp(p.name, "mydsp") == 0 && p.channel == 11);
  assert(p.pwd == NULL && p.openFallback);
  assert(p.pwdError != NULL && strstr(p.pwdError, "8") != NULL);

  p = apPlan(std::string(64, 'x').c_str(), "mydsp", 11, "aurora");
  assert(p.pwd == NULL && p.openFallback);
}

static void invalidNameAndChannelFallBackToDefaults()
{
  ApPlan p = apPlan("secret123", "", 42, "aurora");
  assert(strcmp(p.name, "aurora") == 0 && p.channel == AP_CHANNEL_DEFAULT);
  assert(p.nameError != NULL && !p.openFallback);
  assert(p.pwd != NULL && strcmp(p.pwd, "secret123") == 0);

  p = apPlan("bad", std::string(33, 'a').c_str(), 6, "aurora");  // both invalid
  assert(strcmp(p.name, "aurora") == 0 && p.pwd == NULL && p.openFallback);
}

static void displayWarnsWhileTheApRunsOpenByFallback()
{
  char line[AP_STATUS_LINE_SIZE];
  // 5x7 font on a 128 px display: at most 25 characters
  apStatusLine(line, sizeof(line), "Not Connected", false, true);
  assert(strcmp(line, "OPEN AP: invalid password") == 0);
  apStatusLine(line, sizeof(line), "192.168.178.123", true, true);
  assert(strcmp(line, "OPEN AP! 192.168.178.123") == 0);  // IP stays visible
  assert(strlen(line) <= 25);

  apStatusLine(line, sizeof(line), "192.168.178.123", true, false);
  assert(strcmp(line, "192.168.178.123") == 0);
  apStatusLine(line, sizeof(line), "Not Connected", false, false);
  assert(strcmp(line, "Not Connected") == 0);
}

int main()
{
  validConfigIsAccepted();
  shortPasswordIsRejected();
  badNameIsRejected();
  channelOutsideOneToThirteenIsRejected();
  connectivityChecksKeepPhonesOnTheAp();
  validSettingsStartTheApAsStored();
  invalidPasswordFallsBackToAnOpenAp();
  invalidNameAndChannelFallBackToDefaults();
  displayWarnsWhileTheApRunsOpenByFallback();
  printf("all tests passed\n");
  return 0;
}
