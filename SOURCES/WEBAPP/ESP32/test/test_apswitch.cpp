// Host tests for the AP on/off rule ("AP off while connected", ADR 0005).
// Run: SOURCES/WEBAPP/ESP32/test/run.sh (runs all host tests)
#include <cassert>
#include <cstdio>
#include "../aurora/apswitch.h"

static const bool OPTION_ON = true;
static const bool OPTION_OFF = false;

static void apStartsUp()
{
  ApSwitch ap;
  assert(ap.apUp());
}

static void optionOffKeepsTheApUpWhileConnected()
{
  ApSwitch ap;
  assert(ap.update(OPTION_OFF, true, 0) == ApSwitch::NONE);
  assert(ap.update(OPTION_OFF, true, 3600000) == ApSwitch::NONE);
  assert(ap.apUp());
}

static void connectedApStopsAfterTheGracePeriod()
{
  ApSwitch ap;
  assert(ap.update(OPTION_ON, true, 1000) == ApSwitch::NONE);   // just connected
  assert(ap.update(OPTION_ON, true, 30999) == ApSwitch::NONE);  // 30 s grace
  assert(ap.apUp());
  assert(ap.update(OPTION_ON, true, 31000) == ApSwitch::STOP);
  assert(!ap.apUp());
  assert(ap.update(OPTION_ON, true, 60000) == ApSwitch::NONE);  // stays off
}

static void neverConnectedKeepsTheApUp()
{
  ApSwitch ap;
  assert(ap.update(OPTION_ON, false, 0) == ApSwitch::NONE);
  assert(ap.update(OPTION_ON, false, 3600000) == ApSwitch::NONE);
  assert(ap.apUp());
}

static void lostConnectionStartsTheApAtOnce()
{
  ApSwitch ap;
  ap.update(OPTION_ON, true, 0);
  assert(ap.update(OPTION_ON, true, 30000) == ApSwitch::STOP);
  assert(ap.update(OPTION_ON, false, 40000) == ApSwitch::START);
  assert(ap.apUp());
  assert(ap.update(OPTION_ON, false, 40250) == ApSwitch::NONE);
}

// A short connection between losses must not drop the AP: every new
// connection waits the full grace period again.
static void reconnectWaitsTheFullGraceAgain()
{
  ApSwitch ap;
  ap.update(OPTION_ON, true, 0);
  ap.update(OPTION_ON, true, 30000);                            // AP off
  ap.update(OPTION_ON, false, 40000);                           // lost: AP on
  assert(ap.update(OPTION_ON, true, 50000) == ApSwitch::NONE);  // back
  assert(ap.update(OPTION_ON, false, 70000) == ApSwitch::NONE); // lost again within grace
  assert(ap.update(OPTION_ON, true, 75000) == ApSwitch::NONE);
  assert(ap.update(OPTION_ON, true, 104999) == ApSwitch::NONE);
  assert(ap.update(OPTION_ON, true, 105000) == ApSwitch::STOP);
}

// Enabling the option on a long-standing connection still leaves the user the
// grace period to read the WiFi IP before the AP goes.
static void enablingTheOptionWhileConnectedWaitsTheGrace()
{
  ApSwitch ap;
  ap.update(OPTION_OFF, true, 0);
  assert(ap.update(OPTION_ON, true, 600000) == ApSwitch::NONE);
  assert(ap.update(OPTION_ON, true, 629999) == ApSwitch::NONE);
  assert(ap.update(OPTION_ON, true, 630000) == ApSwitch::STOP);
}

static void disablingTheOptionStartsTheAp()
{
  ApSwitch ap;
  ap.update(OPTION_ON, true, 0);
  ap.update(OPTION_ON, true, 30000);
  assert(ap.update(OPTION_OFF, true, 40000) == ApSwitch::START);
  assert(ap.apUp());
}

// millis() wraps after ~49 days; the grace is measured across the wrap.
static void graceSurvivesTheMillisWrap()
{
  ApSwitch ap;
  ap.update(OPTION_ON, true, 0xFFFFF000u);
  assert(ap.update(OPTION_ON, true, 1000) == ApSwitch::NONE);
  assert(ap.update(OPTION_ON, true, 0xFFFFF000u + 30000u) == ApSwitch::STOP);
}

int main()
{
  apStartsUp();
  optionOffKeepsTheApUpWhileConnected();
  connectedApStopsAfterTheGracePeriod();
  neverConnectedKeepsTheApUp();
  lostConnectionStartsTheApAtOnce();
  reconnectWaitsTheFullGraceAgain();
  enablingTheOptionWhileConnectedWaitsTheGrace();
  disablingTheOptionStartsTheAp();
  graceSurvivesTheMillisWrap();
  printf("all tests passed\n");
  return 0;
}
