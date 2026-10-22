// Host tests for AP config validation and captive-portal answers.
// Build & run: g++ -std=c++11 -Wall -o test_apconfig SOURCES/WEBAPP/ESP32/test/test_apconfig.cpp && ./test_apconfig
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

int main()
{
  validConfigIsAccepted();
  shortPasswordIsRejected();
  badNameIsRejected();
  channelOutsideOneToThirteenIsRejected();
  connectivityChecksKeepPhonesOnTheAp();
  printf("all tests passed\n");
  return 0;
}
