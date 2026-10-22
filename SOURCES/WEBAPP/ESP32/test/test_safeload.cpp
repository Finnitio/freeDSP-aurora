// Host tests for the ADAU1452 safeload register sequence.
// Build & run: g++ -std=c++11 -Wall -o test_safeload SOURCES/WEBAPP/ESP32/test/test_safeload.cpp && ./test_safeload
#include <cassert>
#include <cstdio>
#include <vector>
#include "../aurora/safeload.h"

struct Tx
{
  uint16_t reg;
  std::vector<uint8_t> data;
};

struct FakePort : SafeloadPort
{
  std::vector<Tx> txs;
  void write(uint16_t reg, const uint8_t* data, size_t len) override
  {
    Tx t;
    t.reg = reg;
    t.data.assign(data, data + len);
    txs.push_back(t);
  }
};

static const uint32_t kWords[5] = { 0x01000000, 0xFF000001, 0x00800000, 0x12345678, 0xFEDCBA98 };

static void safeloadWritesDataThenTargetAddress()
{
  FakePort port;
  SafeloadCfg cfg = { 15396, 15401 };
  safeloadBiquad(port, cfg, 15548, kWords);

  assert(port.txs.size() == 2);
  // 5 data words in one burst, big endian, into the data registers
  assert(port.txs[0].reg == 15396);
  assert(port.txs[0].data.size() == 20);
  assert(port.txs[0].data[0] == 0x01 && port.txs[0].data[3] == 0x00);
  assert(port.txs[0].data[4] == 0xFF && port.txs[0].data[7] == 0x01);
  assert(port.txs[0].data[12] == 0x12 && port.txs[0].data[15] == 0x78);
  assert(port.txs[0].data[16] == 0xFE && port.txs[0].data[19] == 0x98);
  // target address, then num_SafeLoad (= 5, at address+1) which triggers the transfer
  assert(port.txs[1].reg == 15401);
  assert(port.txs[1].data.size() == 8);
  uint32_t target = (port.txs[1].data[0] << 24) | (port.txs[1].data[1] << 16)
                  | (port.txs[1].data[2] << 8) | port.txs[1].data[3];
  assert(target == 15548);
  uint32_t num = (port.txs[1].data[4] << 24) | (port.txs[1].data[5] << 16)
               | (port.txs[1].data[6] << 8) | port.txs[1].data[7];
  assert(num == 5);
}

static void withoutSafeloadAddressesFallsBackToDirectWrites()
{
  FakePort port;
  SafeloadCfg cfg = { 0, 0 };
  safeloadBiquad(port, cfg, 100, kWords);

  assert(port.txs.size() == 5);
  for (int ii = 0; ii < 5; ii++)
  {
    assert(port.txs[ii].reg == 100 + ii);
    assert(port.txs[ii].data.size() == 4);
    assert(port.txs[ii].data[0] == (kWords[ii] >> 24));
    assert(port.txs[ii].data[3] == (kWords[ii] & 0xFF));
  }
}

int main()
{
  safeloadWritesDataThenTargetAddress();
  withoutSafeloadAddressesFallsBackToDirectWrites();
  printf("all tests passed\n");
  return 0;
}
