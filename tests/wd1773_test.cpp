// g++ -std=c++20 -o wd1773_test tests/wd1773_test.cpp WD1773.cpp
#include "../WD1773.h"
#include <cstdio>
#include <cstdlib>

static int failures = 0;

#define CHECK(cond) do {                                        \
    if (!(cond)) {                                              \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
      failures++;                                               \
    }                                                           \
  } while (0)

struct Bench {
  SectorPort port;
  WD1773 fdc { port };
  uint32_t now = 1000;
  bool serve = true, denyWrites = false;
  unsigned requests = 0, writes = 0;
  uint8_t written[SectorPort::SECTOR_SIZE] {};
  uint8_t writtenTrack = 0xFF, writtenSector = 0xFF, readSide = 0xFF;

  explicit Bench(bool writeProtected = true) {
    fdc.setDiskPresent(0, true, writeProtected);
    fdc.select(0, false, true);
  }

  // Sector contents are a function of the address, so any mix-up shows.
  static uint8_t pattern(uint8_t track, uint8_t sector, unsigned idx) {
    return (uint8_t) (track * 31 + sector * 7 + idx);
  }

  void service() {
    if (port.state != SectorPort::REQUESTED || !serve)
      return;
    requests++;
    if (port.drive != 0 || port.track > 34 || port.sector < 1 || port.sector > 18) {
      port.state = SectorPort::FAILED;
      return;
    }
    if (port.write) {
      if (denyWrites) {
        port.state = SectorPort::DENIED;
        return;
      }
      writes++;
      writtenTrack = port.track;
      writtenSector = port.sector;
      for (unsigned idx = 0; idx < SectorPort::SECTOR_SIZE; idx++)
        written[idx] = port.data[idx];
      port.state = SectorPort::READY;
      return;
    }
    readSide = port.side;
    for (unsigned idx = 0; idx < SectorPort::SECTOR_SIZE; idx++)
      port.data[idx] = pattern(port.track, port.sector, idx);
    port.state = SectorPort::READY;
  }

  // Writes one sector the way a halted CPU would: one byte per DRQ.
  unsigned writeSector(uint8_t sector, uint8_t seed, uint8_t cmd = 0xA0) {
    unsigned sent = 0;
    fdc.write(WD1773::REG_SECTOR, sector, now);
    command(cmd);
    for (unsigned idx = 0; idx < SectorPort::SECTOR_SIZE; idx++) {
      if (!waitDrq())
        break;
      fdc.write(WD1773::REG_DATA, (uint8_t) (seed + idx * 5), now);
      sent++;
    }
    return sent;
  }

  void run(uint32_t micros) {
    for (uint32_t end = now + micros; now != end; now++) {
      fdc.tick(now);
      service();
    }
  }

  // Returns the time taken, or 0 on timeout.
  uint32_t waitIdle(uint32_t limit = 3000000) {
    uint32_t start = now;
    while (fdc.active() && now - start < limit)
      run(1);
    return fdc.active() ? 0 : now - start;
  }

  bool waitDrq(uint32_t limit = 2000000) {
    uint32_t start = now;
    while (!fdc.drq() && fdc.active() && now - start < limit)
      run(1);
    return fdc.drq();
  }

  void command(uint8_t value) { fdc.write(WD1773::REG_COMMAND, value, now); }
  uint8_t status() { return fdc.read(WD1773::REG_STATUS, now); }

  void seek(uint8_t track) {
    fdc.write(WD1773::REG_DATA, track, now);
    command(0x14);
    CHECK(waitIdle());
  }

  // Reads one sector the way a halted CPU would: one byte per DRQ.
  unsigned readSector(uint8_t track, uint8_t sector, uint8_t cmd = 0x80) {
    unsigned good = 0;
    fdc.write(WD1773::REG_SECTOR, sector, now);
    command(cmd);
    for (unsigned idx = 0; idx < SectorPort::SECTOR_SIZE; idx++) {
      if (!waitDrq())
        break;
      if (fdc.read(WD1773::REG_DATA, now) == pattern(track, sector, idx))
        good++;
    }
    return good;
  }
};

static void testRestore()
{
  Bench bench;
  bench.seek(10);
  bench.command(0x00);
  CHECK(bench.status() & WD1773::ST_BUSY);
  CHECK(!bench.fdc.intrq());
  uint32_t taken = bench.waitIdle();
  CHECK(taken >= 10 * 6000 && taken < 12 * 6000);
  CHECK(bench.fdc.intrq());
  uint8_t result = bench.status();
  CHECK(!(result & WD1773::ST_BUSY));
  CHECK(result & WD1773::ST_TRACK0);
  CHECK(!(result & WD1773::ST_SEEK_ERROR));
  CHECK(!bench.fdc.intrq());
  CHECK(bench.fdc.read(WD1773::REG_TRACK, bench.now) == 0);
}

static void testSeekVerify()
{
  Bench bench;
  bench.seek(17);
  uint8_t result = bench.status();
  CHECK(!(result & (WD1773::ST_SEEK_ERROR | WD1773::ST_TRACK0 | WD1773::ST_BUSY)));
  CHECK(bench.fdc.read(WD1773::REG_TRACK, bench.now) == 17);

  // The head is on 17; lying to the track register must fail the verify.
  bench.fdc.write(WD1773::REG_TRACK, 20, bench.now);
  bench.fdc.write(WD1773::REG_DATA, 20, bench.now);
  bench.command(0x14);
  CHECK(bench.waitIdle());
  CHECK(bench.status() & WD1773::ST_SEEK_ERROR);
}

static void testReadSector()
{
  Bench bench;
  bench.seek(17);
  CHECK(bench.readSector(17, 2) == 256);
  CHECK(bench.status() & WD1773::ST_BUSY);
  CHECK(!bench.fdc.intrq());
  CHECK(bench.waitIdle(1000));
  CHECK(bench.fdc.intrq());
  uint8_t result = bench.status();
  CHECK(result == 0);
}

static void testDataHeldUntilNextDrq()
{
  Bench bench;
  bench.seek(3);
  bench.fdc.write(WD1773::REG_SECTOR, 5, bench.now);
  bench.command(0x80);
  CHECK(bench.waitDrq());
  uint8_t first = bench.fdc.read(WD1773::REG_DATA, bench.now);
  CHECK(first == Bench::pattern(3, 5, 0));
  CHECK(!bench.fdc.drq());
  CHECK(bench.fdc.read(WD1773::REG_DATA, bench.now) == first);
  uint32_t start = bench.now;
  CHECK(bench.waitDrq());
  CHECK(bench.now - start >= 30 && bench.now - start <= 34);
  CHECK(bench.fdc.read(WD1773::REG_DATA, bench.now) == Bench::pattern(3, 5, 1));
}

static void testLostData()
{
  Bench bench;
  bench.command(0x00);
  CHECK(bench.waitIdle());
  bench.fdc.write(WD1773::REG_SECTOR, 1, bench.now);
  bench.command(0x80);
  CHECK(bench.waitIdle());
  uint8_t result = bench.status();
  CHECK(result & WD1773::ST_LOST_DATA);
  CHECK(result & WD1773::ST_DRQ);
}

static void testRecordNotFound()
{
  Bench bench;
  bench.command(0x00);
  CHECK(bench.waitIdle());
  bench.fdc.write(WD1773::REG_SECTOR, 19, bench.now);
  bench.command(0x80);
  uint32_t taken = bench.waitIdle();
  CHECK(taken >= 900000 && taken <= 1100000);
  CHECK(bench.status() == WD1773::ST_RNF);
}

static void testHostNeverAnswers()
{
  Bench bench;
  bench.command(0x00);
  CHECK(bench.waitIdle());
  bench.serve = false;
  bench.fdc.write(WD1773::REG_SECTOR, 1, bench.now);
  bench.command(0x80);
  CHECK(bench.fdc.waitingForHost());
  CHECK(bench.waitIdle(WD1773::HOST_TIMEOUT_US + 1000));
  CHECK(!bench.fdc.waitingForHost());
  CHECK(bench.status() == WD1773::ST_RNF);

  // The stale request must be answered before a new one is issued.
  bench.fdc.write(WD1773::REG_SECTOR, 2, bench.now);
  bench.command(0x80);
  bench.run(1000);
  CHECK(bench.port.sector == 1);
  bench.serve = true;
  CHECK(bench.waitDrq());
  CHECK(bench.port.sector == 2);
  CHECK(bench.fdc.read(WD1773::REG_DATA, bench.now) == Bench::pattern(0, 2, 0));
}

static void testMultiSector()
{
  Bench bench;
  bench.command(0x00);
  CHECK(bench.waitIdle());
  bench.fdc.write(WD1773::REG_SECTOR, 17, bench.now);
  bench.command(0x90);
  unsigned good = 0;
  for (uint8_t sector = 17; sector <= 18; sector++)
    for (unsigned idx = 0; idx < 256; idx++)
      if (bench.waitDrq()
          && bench.fdc.read(WD1773::REG_DATA, bench.now) == Bench::pattern(0, sector, idx))
        good++;
  CHECK(good == 512);
  CHECK(bench.waitIdle());
  CHECK(bench.status() == WD1773::ST_RNF);
  CHECK(bench.fdc.read(WD1773::REG_SECTOR, bench.now) == 19);
}

static void testForceInterrupt()
{
  Bench bench;
  bench.command(0x00);
  CHECK(bench.waitIdle());
  bench.status();
  bench.fdc.write(WD1773::REG_SECTOR, 1, bench.now);
  bench.command(0x80);
  CHECK(bench.waitDrq());
  bench.command(0xD0);
  CHECK(!bench.fdc.active());
  CHECK(!bench.fdc.intrq());
  CHECK(!(bench.status() & WD1773::ST_BUSY));

  bench.command(0xD8);
  CHECK(bench.fdc.intrq());
  bench.status();
  CHECK(bench.fdc.intrq());
  bench.command(0xD0);
  bench.status();
  CHECK(!bench.fdc.intrq());
}

static void testWriteProtectAndNotReady()
{
  Bench bench;
  bench.command(0x00);
  CHECK(bench.waitIdle());
  bench.fdc.write(WD1773::REG_SECTOR, 1, bench.now);
  bench.command(0xA0);
  CHECK(bench.waitIdle());
  CHECK(bench.status() == WD1773::ST_WRITE_PROTECT);

  bench.fdc.select(1, false, true);
  bench.command(0x80);
  CHECK(!bench.fdc.active());
  CHECK(bench.fdc.intrq());
  CHECK(bench.status() == WD1773::ST_NOT_READY);
  CHECK(bench.requests == 0);
}

static void testCommandIgnoredWhileBusy()
{
  Bench bench;
  bench.seek(5);
  bench.fdc.write(WD1773::REG_SECTOR, 1, bench.now);
  bench.command(0x80);
  bench.command(0x00);
  CHECK(bench.readSector(5, 1) == 256);
}

static void testWriteSector()
{
  Bench bench(false);
  bench.seek(20);
  CHECK(!(bench.status() & WD1773::ST_WRITE_PROTECT));
  CHECK(bench.writeSector(9, 0x40) == 256);
  CHECK(bench.waitIdle());
  CHECK(bench.fdc.intrq());
  CHECK(bench.status() == 0);
  CHECK(bench.writes == 1);
  CHECK(bench.writtenTrack == 20 && bench.writtenSector == 9);
  unsigned good = 0;
  for (unsigned idx = 0; idx < 256; idx++)
    if (bench.written[idx] == (uint8_t) (0x40 + idx * 5))
      good++;
  CHECK(good == 256);

  // And it reads back through the normal path afterwards.
  CHECK(bench.readSector(20, 9) == 256);
}

static void testWriteDenied()
{
  Bench bench(false);
  bench.denyWrites = true;
  bench.command(0x00);
  CHECK(bench.waitIdle());
  CHECK(bench.writeSector(1, 0) == 256);
  CHECK(bench.waitIdle());
  CHECK(bench.status() == WD1773::ST_WRITE_PROTECT);
  CHECK(bench.writes == 0);
}

static void testWriteNoData()
{
  Bench bench(false);
  bench.command(0x00);
  CHECK(bench.waitIdle());
  bench.fdc.write(WD1773::REG_SECTOR, 1, bench.now);
  bench.command(0xA0);
  CHECK(bench.waitDrq());
  CHECK(bench.waitIdle());
  CHECK(bench.status() == WD1773::ST_LOST_DATA);
  CHECK(bench.writes == 0);
}

static void testWriteHostFails()
{
  Bench bench(false);
  bench.command(0x00);
  CHECK(bench.waitIdle());
  CHECK(bench.writeSector(19, 0) == 256);
  CHECK(bench.waitIdle());
  CHECK(bench.status() == WD1773::ST_WRITE_FAULT);
}

static void testReadAddress()
{
  Bench bench;
  bench.seek(9);
  // The ID comes from under the head, whatever the track register says.
  bench.fdc.write(WD1773::REG_TRACK, 30, bench.now);
  bool seen[19] {};

  for (unsigned tries = 0; tries < 40; tries++) {
    uint8_t ident[WD1773::ID_SIZE];
    unsigned got = 0;

    bench.command(0xC0);
    while (got < sizeof(ident) && bench.waitDrq())
      ident[got++] = bench.fdc.read(WD1773::REG_DATA, bench.now);
    CHECK(got == sizeof(ident));
    CHECK(bench.waitIdle(1000));
    CHECK(bench.fdc.intrq());
    CHECK(bench.status() == 0);
    CHECK(ident[0] == 9 && ident[1] == 0 && ident[3] == 1);
    CHECK(ident[2] >= 1 && ident[2] <= 18);
    seen[ident[2] <= 18 ? ident[2] : 0] = true;
    CHECK(bench.fdc.sectorRegister() == 9);

    // Over the sync bytes, the mark, the ID and its own CRC the sum is zero.
    static const uint8_t lead[] = { 0xA1, 0xA1, 0xA1, 0xFE };
    uint16_t crc = 0xFFFF;
    for (unsigned idx = 0; idx < sizeof(lead) + sizeof(ident); idx++) {
      crc ^= (idx < sizeof(lead) ? lead[idx] : ident[idx - sizeof(lead)]) << 8;
      for (unsigned bit = 0; bit < 8; bit++)
        crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1;
    }
    CHECK(crc == 0);
    bench.run(7919);
  }
  unsigned distinct = 0;
  for (unsigned idx = 1; idx <= 18; idx++)
    distinct += seen[idx];
  CHECK(distinct > 9);

  // A track the image does not have.
  bench.fdc.write(WD1773::REG_TRACK, 9, bench.now);
  bench.seek(40);
  bench.command(0xC0);
  CHECK(bench.waitIdle());
  CHECK(bench.status() & WD1773::ST_RNF);
}

// OS-9's Boot issues Read Sector and only then selects the side.
static void testSideSelectedAfterCommand()
{
  Bench bench;
  bench.fdc.write(WD1773::REG_SECTOR, 4, bench.now);
  bench.command(0x80);
  CHECK(bench.fdc.waitingForHost());
  bench.run(200);
  CHECK(bench.requests == 1 && bench.readSide == 0);
  CHECK(!bench.fdc.waitingForHost() && !bench.fdc.drq());

  // The sector fetched for the wrong side is thrown away and fetched again.
  bench.fdc.select(0, true, true);
  CHECK(bench.fdc.waitingForHost());
  CHECK(bench.waitDrq());
  CHECK(bench.requests == 2 && bench.readSide == 1);

  // Once data has started to flow, a later select changes nothing.
  CHECK(bench.fdc.read(WD1773::REG_DATA, bench.now) == Bench::pattern(0, 4, 0));
  CHECK(bench.waitDrq());
  bench.fdc.select(0, false, true);
  CHECK(!bench.fdc.waitingForHost());
  CHECK(bench.fdc.read(WD1773::REG_DATA, bench.now) == Bench::pattern(0, 4, 1));
  CHECK(bench.requests == 2);

  // Disk BASIC rewrites the latch with the same drive and side.
  Bench same;
  same.fdc.write(WD1773::REG_SECTOR, 4, same.now);
  same.command(0x80);
  same.run(200);
  same.fdc.select(0, false, true);
  CHECK(!same.fdc.waitingForHost());
  CHECK(same.waitDrq());
  CHECK(same.requests == 1);
}

int main()
{
  testRestore();
  testSeekVerify();
  testReadSector();
  testDataHeldUntilNextDrq();
  testLostData();
  testRecordNotFound();
  testHostNeverAnswers();
  testMultiSector();
  testForceInterrupt();
  testWriteProtectAndNotReady();
  testCommandIgnoredWhileBusy();
  testWriteSector();
  testWriteDenied();
  testWriteNoData();
  testWriteHostFails();
  testReadAddress();
  testSideSelectedAfterCommand();

  if (failures) {
    printf("%d check(s) failed\n", failures);
    return EXIT_FAILURE;
  }
  printf("all checks passed\n");
  return EXIT_SUCCESS;
}
