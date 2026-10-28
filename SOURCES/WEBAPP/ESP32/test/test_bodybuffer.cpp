// Host tests for POST body reassembly.
// Run: SOURCES/WEBAPP/ESP32/test/run.sh (runs all host tests)
#include <cassert>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include "../aurora/bodybuffer.h"

static BodyResult feed(char*& buf, const char* s, size_t len, size_t index, size_t total)
{
  return bodyChunk(buf, (const uint8_t*)s, len, index, total, 1024);
}

static void singleChunkIsComplete()
{
  char* buf = NULL;
  const char* body = "{\"a\":1}";
  assert(feed(buf, body, 7, 0, 7) == BODY_COMPLETE);
  assert(strcmp(buf, body) == 0);
  free(buf);
}

static void chunksAreJoinedInOrder()
{
  char* buf = NULL;
  assert(feed(buf, "{\"idx\":", 7, 0, 18) == BODY_MORE);
  assert(feed(buf, "\"3\",\"fc", 7, 7, 18) == BODY_MORE);
  assert(feed(buf, "\":1}", 4, 14, 18) == BODY_COMPLETE);
  assert(strcmp(buf, "{\"idx\":\"3\",\"fc\":1}") == 0);
  free(buf);
}

static void tooLargeBodyIsRejected()
{
  char* buf = NULL;
  assert(bodyChunk(buf, (const uint8_t*)"x", 1, 0, 2000, 1024) == BODY_REJECTED);
  assert(buf == NULL);
  // later chunks of the rejected body are ignored
  assert(bodyChunk(buf, (const uint8_t*)"x", 1, 1, 2000, 1024) == BODY_REJECTED);
}

static void chunkBeyondAnnouncedTotalDoesNotOverflow()
{
  char* buf = NULL;
  assert(feed(buf, "ab", 2, 0, 4) == BODY_MORE);
  assert(feed(buf, "cdefgh", 6, 2, 4) == BODY_REJECTED);
  free(buf);
}

int main()
{
  singleChunkIsComplete();
  chunksAreJoinedInOrder();
  tooLargeBodyIsRejected();
  chunkBeyondAnnouncedTotalDoesNotOverflow();
  printf("all tests passed\n");
  return 0;
}
