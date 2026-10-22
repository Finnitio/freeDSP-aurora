// Host tests for the STA retry policy.
// Build & run: g++ -std=c++11 -Wall -o test_staretry SOURCES/WEBAPP/ESP32/test/test_staretry.cpp && ./test_staretry
#include <cassert>
#include <cstdio>
#include "../aurora/staretry.h"

static void firstUpdateStartsConnecting()
{
  StaRetry retry;
  assert(retry.update(false, 0) == StaRetry::BEGIN);
  assert(retry.update(false, 250) == StaRetry::NONE);
}

// Every kind of failure (wrong password, SSID not found, ...) looks the same
// from here: the attempt did not reach "connected" within 15 s.
static void unreachableNetworkStopsAfterThreeAttempts()
{
  StaRetry retry;
  assert(retry.update(false, 0) == StaRetry::BEGIN);
  assert(retry.update(false, 14999) == StaRetry::NONE);
  assert(retry.update(false, 15000) == StaRetry::BEGIN);   // 1st failure, retry
  assert(retry.update(false, 30000) == StaRetry::BEGIN);   // 2nd failure, retry
  assert(retry.update(false, 45000) == StaRetry::STOP);    // 3rd failure, give up
  assert(retry.update(false, 50000) == StaRetry::NONE);
}

static void afterStopRetriesOnceEvery60s()
{
  StaRetry retry;
  retry.update(false, 0);
  retry.update(false, 15000);
  retry.update(false, 30000);
  assert(retry.update(false, 45000) == StaRetry::STOP);
  assert(retry.update(false, 104999) == StaRetry::NONE);
  assert(retry.update(false, 105000) == StaRetry::BEGIN);  // 60 s later: one try
  assert(retry.update(false, 119999) == StaRetry::NONE);
  assert(retry.update(false, 120000) == StaRetry::STOP);   // failed again, stop
  assert(retry.update(false, 179999) == StaRetry::NONE);
  assert(retry.update(false, 180000) == StaRetry::BEGIN);
}

static void lostConnectionGetsFreshAttempts()
{
  StaRetry retry;
  retry.update(false, 0);
  retry.update(false, 15000);                              // 1 failure
  retry.update(false, 30000);                              // 2 failures
  assert(retry.update(true, 31000) == StaRetry::NONE);     // connected: reset
  assert(retry.update(true, 90000) == StaRetry::NONE);
  assert(retry.update(false, 100000) == StaRetry::BEGIN);  // link lost
  assert(retry.update(false, 115000) == StaRetry::BEGIN);  // 3 tries again
  assert(retry.update(false, 130000) == StaRetry::BEGIN);
  assert(retry.update(false, 145000) == StaRetry::STOP);
}

int main()
{
  lostConnectionGetsFreshAttempts();
  firstUpdateStartsConnecting();
  unreachableNetworkStopsAfterThreeAttempts();
  afterStopRetriesOnceEvery60s();
  printf("all tests passed\n");
  return 0;
}
