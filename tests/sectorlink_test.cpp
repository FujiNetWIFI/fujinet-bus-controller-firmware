// g++ -std=c++20 -o sectorlink_test tests/sectorlink_test.cpp
#include "../SectorLink.h"
#include <cstdio>
#include <cstdlib>
#include <vector>

static int failures = 0;

#define CHECK(cond) do {                                        \
    if (!(cond)) {                                              \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
      failures++;                                               \
    }                                                           \
  } while (0)

// A host holding one image of `sectors` sectors in every drive.
struct Bench {
  SectorPort port;
  SectorLink link { port };
  uint8_t out[SectorLink::MAX_SEND];
  uint32_t sectors;
  uint32_t now = 0;
  bool os9 = false, os9TwoSides = false, readOnly = false;
  std::vector<uint32_t> reads, writes;
  uint8_t written[SectorPort::SECTOR_SIZE] {};

  explicit Bench(uint32_t sectors = 630) : sectors(sectors) {}

  // Sector contents are a function of the address, so any mix-up shows.
  static uint8_t pattern(uint32_t lsn, unsigned idx) {
    return (uint8_t) (lsn * 13 + idx * 3 + 1);
  }

  uint8_t content(uint32_t lsn, unsigned idx) const {
    if (os9 && lsn == 0) {
      if (idx < 3)
        return sectors >> (8 * (2 - idx));
      if (idx == 3 || idx == 0x12)
        return 18;
      if (idx == 0x10)
        return os9TwoSides ? 3 : 2;
      if (idx == 0x11)
        return 0;
    }
    return pattern(lsn, idx);
  }

  void request(uint8_t drive, uint8_t track, uint8_t side, uint8_t sector, bool write = false) {
    port.write = write;
    port.drive = drive;
    port.track = track;
    port.side = side;
    port.sector = sector;
    port.state = SectorPort::REQUESTED;
  }

  // Answers one message the link has just sent.
  void answer(size_t sent) {
    uint32_t lsn = out[2] << 16 | out[3] << 8 | out[4];

    if (out[0] == SectorLink::OP_READEX) {
      CHECK(sent == SectorLink::HEADER_SIZE);
      reads.push_back(lsn);
      bool valid = lsn < sectors;
      uint16_t sum = 0;
      size_t reply = 0;
      for (unsigned idx = 0; idx < SectorPort::SECTOR_SIZE; idx++) {
        uint8_t value = valid ? content(lsn, idx) : 0;
        sum += value;
        reply = link.receive(value, out);
      }
      CHECK(reply == 2);
      CHECK(out[0] == (sum >> 8) && out[1] == (sum & 0xFF));
      link.receive(valid ? 0 : 211, out);
    }
    else {
      CHECK(out[0] == SectorLink::OP_WRITE && sent == SectorLink::MAX_SEND);
      writes.push_back(lsn);
      uint16_t sum = 0;
      for (unsigned idx = 0; idx < SectorPort::SECTOR_SIZE; idx++) {
        written[idx] = out[SectorLink::HEADER_SIZE + idx];
        sum += written[idx];
      }
      CHECK(out[261] == (sum >> 8) && out[262] == (sum & 0xFF));
      link.receive(readOnly ? 0xF2 : lsn < sectors ? 0 : 0xF5, out);
    }
  }

  // Runs until the pending request has an answer.
  SectorPort::State finish() {
    for (unsigned idx = 0; idx < 8 && port.state == SectorPort::REQUESTED; idx++) {
      size_t sent = link.poll(now, out);
      if (sent)
        answer(sent);
    }
    CHECK(!link.active());
    return port.state;
  }

  bool holds(uint32_t lsn) const {
    for (unsigned idx = 0; idx < SectorPort::SECTOR_SIZE; idx++)
      if (port.data[idx] != content(lsn, idx))
        return false;
    return true;
  }
};

static void testRead()
{
  Bench bench;

  CHECK(bench.link.poll(0, bench.out) == 0);
  CHECK(!bench.link.active());

  // Sector 0 and one size check come first, then the sector asked for.
  bench.request(1, 17, 0, 3);
  CHECK(bench.finish() == SectorPort::READY);
  CHECK(bench.reads == (std::vector<uint32_t> { 0, 2879, 17 * 18 + 2 }));
  CHECK(bench.holds(17 * 18 + 2));

  // The shape is remembered while the drive stays in use.
  bench.reads.clear();
  bench.now += SectorLink::GEOMETRY_MS - 1;
  bench.request(1, 34, 0, 18);
  CHECK(bench.finish() == SectorPort::READY);
  CHECK(bench.reads == (std::vector<uint32_t> { 629 }));

  // Each drive is worked out for itself.
  bench.reads.clear();
  bench.request(0, 0, 0, 1);
  CHECK(bench.finish() == SectorPort::READY);
  CHECK(bench.reads == (std::vector<uint32_t> { 0, 2879, 0 }));
  CHECK(bench.holds(0));

  // An image can have been changed once the drive has stood idle.
  bench.reads.clear();
  bench.now += SectorLink::GEOMETRY_MS;
  bench.request(0, 1, 0, 1);
  CHECK(bench.finish() == SectorPort::READY);
  CHECK(bench.reads == (std::vector<uint32_t> { 0, 2879, 18 }));
}

static void testSides()
{
  // One side: the other is not there, and nor is a track past the end.
  Bench one(720);
  one.request(0, 39, 0, 18);
  CHECK(one.finish() == SectorPort::READY);
  CHECK(one.holds(719));
  one.request(0, 0, 1, 1);
  CHECK(one.finish() == SectorPort::FAILED);
  one.request(0, 40, 0, 1);
  CHECK(one.finish() == SectorPort::FAILED);

  // 80 tracks, two sides: the sides alternate, track by track.
  Bench two(2880);
  two.request(0, 0, 1, 1);
  CHECK(two.finish() == SectorPort::READY);
  CHECK(two.reads == (std::vector<uint32_t> { 0, 2879, 2880, 18 }));
  CHECK(two.holds(18));
  two.request(0, 79, 1, 18);
  CHECK(two.finish() == SectorPort::READY);
  CHECK(two.holds(2879));
  two.request(0, 2, 0, 5);
  CHECK(two.finish() == SectorPort::READY);
  CHECK(two.holds(2 * 36 + 4));

  // Larger than any floppy, such as several disks in one file: one side.
  Bench many(630 * 8);
  many.request(0, 1, 0, 1);
  CHECK(many.finish() == SectorPort::READY);
  CHECK(many.reads == (std::vector<uint32_t> { 0, 2879, 2880, 18 }));
  many.request(0, 0, 1, 1);
  CHECK(many.finish() == SectorPort::FAILED);

  // The same size as 40 double-sided tracks, with nothing to say so.
  Bench plain(1440);
  plain.request(0, 79, 0, 18);
  CHECK(plain.finish() == SectorPort::READY);
  CHECK(plain.holds(1439));
}

static void testOS9()
{
  // An OS-9 disk says how many sides it has, and no size check is needed.
  Bench two(1440);
  two.os9 = two.os9TwoSides = true;
  two.request(0, 39, 1, 18);
  CHECK(two.finish() == SectorPort::READY);
  CHECK(two.reads == (std::vector<uint32_t> { 0, 1439 }));
  CHECK(two.holds(1439));
  two.request(0, 1, 0, 1);
  CHECK(two.finish() == SectorPort::READY);
  CHECK(two.holds(36));

  Bench one(720);
  one.os9 = true;
  one.request(0, 1, 0, 1);
  CHECK(one.finish() == SectorPort::READY);
  CHECK(one.reads == (std::vector<uint32_t> { 0, 18 }));
  one.request(0, 1, 1, 1);
  CHECK(one.finish() == SectorPort::FAILED);
}

static void testWrite()
{
  Bench bench(2880);

  // The sector to be written must survive the reads that come first.
  for (unsigned idx = 0; idx < 256; idx++)
    bench.port.data[idx] = (uint8_t) (idx ^ 0x5A);
  bench.request(2, 1, 1, 1, true);
  CHECK(bench.finish() == SectorPort::READY);
  CHECK(bench.reads == (std::vector<uint32_t> { 0, 2879, 2880 }));
  CHECK(bench.writes == (std::vector<uint32_t> { 54 }));
  CHECK(bench.written[0] == 0x5A && bench.written[255] == (uint8_t) (255 ^ 0x5A));

  bench.readOnly = true;
  bench.request(2, 1, 0, 1, true);
  CHECK(bench.finish() == SectorPort::DENIED);

  bench.readOnly = false;
  bench.request(2, 80, 0, 1, true);
  CHECK(bench.finish() == SectorPort::FAILED);

  // A write is allowed as long as a read before the link gives up.
  bench.request(2, 1, 0, 1, true);
  CHECK(bench.link.poll(bench.now, bench.out) == SectorLink::MAX_SEND);
  bench.link.poll(bench.now + 2999, bench.out);
  CHECK(bench.port.state == SectorPort::REQUESTED);
  bench.link.poll(bench.now + 3000, bench.out);
  CHECK(bench.port.state == SectorPort::FAILED);
}

static void testNoDisk()
{
  Bench bench(0);

  bench.request(0, 0, 0, 1);
  CHECK(bench.finish() == SectorPort::FAILED);
  CHECK(bench.reads == (std::vector<uint32_t> { 0 }));

  // Nothing was learned, so a disk put in later is looked at afresh.
  bench.sectors = 630;
  bench.reads.clear();
  bench.request(0, 0, 0, 2);
  CHECK(bench.finish() == SectorPort::READY);
  CHECK(bench.reads == (std::vector<uint32_t> { 0, 2879, 1 }));
  CHECK(bench.holds(1));
}

static void testBadAddress()
{
  Bench bench;

  bench.request(0, 0, 0, 19);
  CHECK(bench.finish() == SectorPort::FAILED);
  bench.request(0, 0, 0, 0);
  CHECK(bench.finish() == SectorPort::FAILED);
  bench.request(4, 0, 0, 1);
  CHECK(bench.finish() == SectorPort::FAILED);
  CHECK(bench.reads == (std::vector<uint32_t> { 0, 2879 }));
}

static void testTimeout()
{
  Bench bench;

  bench.request(0, 0, 0, 1);
  CHECK(bench.finish() == SectorPort::READY);

  bench.now = 1000;
  bench.request(0, 0, 0, 1);
  CHECK(bench.link.poll(1000, bench.out) == 5);
  bench.link.receive(1, bench.out);
  CHECK(bench.link.poll(3999, bench.out) == 0);
  CHECK(bench.port.state == SectorPort::REQUESTED);
  CHECK(bench.link.poll(4000, bench.out) == 0);
  CHECK(bench.port.state == SectorPort::FAILED);

  // Late bytes are swallowed, and each one keeps the link closed longer.
  CHECK(bench.link.active());
  CHECK(bench.link.receive(2, bench.out) == 0);
  CHECK(bench.port.state == SectorPort::FAILED);
  bench.link.poll(4050, bench.out);
  CHECK(bench.link.active());
  bench.link.poll(4149, bench.out);
  CHECK(bench.link.active());
  bench.link.poll(4150, bench.out);
  CHECK(!bench.link.active());

  // The same while the shape of the image is being worked out.
  bench.now = 10000;
  bench.request(3, 0, 0, 1);
  CHECK(bench.link.poll(10000, bench.out) == 5);
  CHECK(bench.link.poll(13000, bench.out) == 0);
  CHECK(bench.port.state == SectorPort::FAILED);
  bench.link.poll(13100, bench.out);
  CHECK(!bench.link.active());
  bench.now = 13100;
  bench.reads.clear();
  bench.request(3, 0, 0, 1);
  CHECK(bench.finish() == SectorPort::READY);
  CHECK(bench.reads == (std::vector<uint32_t> { 0, 2879, 0 }));
  CHECK(bench.holds(0));
}

int main()
{
  testRead();
  testSides();
  testOS9();
  testWrite();
  testNoDisk();
  testBadAddress();
  testTimeout();

  if (failures) {
    printf("%d check(s) failed\n", failures);
    return EXIT_FAILURE;
  }
  printf("all checks passed\n");
  return EXIT_SUCCESS;
}
