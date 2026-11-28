// g++ -std=c++20 -o ringbuffer_test tests/ringbuffer_test.cpp
#include "../RingBuffer.h"
#include <cstdio>
#include <cstdlib>

static int failures = 0;

#define CHECK(cond) do {                                        \
    if (!(cond)) {                                              \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
      failures++;                                               \
    }                                                           \
  } while (0)

static void testEmpty(void)
{
  RingBuffer<8> ring;
  CHECK(ring.isEmpty());
  CHECK(!ring.isFull());
  CHECK(ring.available() == 0);
  CHECK(ring.contiguous() == 0);
}

static void testFull(void)
{
  RingBuffer<8> ring;
  for (uint8_t val = 0; val < 7; val++) {
    CHECK(!ring.put(val + 1).is_error());
    CHECK(ring.available() == (size_t) val + 1);
  }
  CHECK(ring.isFull());
  CHECK(!ring.isEmpty());
  CHECK(ring.put(0xEE).is_error());
  CHECK(ring.available() == 7);
  CHECK(ring.peek() == 1);
  CHECK(ring.contiguous() == 7);
}

static void testOrder(void)
{
  RingBuffer<8> ring;
  for (uint8_t val = 10; val < 15; val++)
    ring.put(val);
  for (uint8_t val = 10; val < 15; val++) {
    CHECK(!ring.isEmpty());
    CHECK(ring.peek() == val);
    ring.consume(1);
  }
  CHECK(ring.isEmpty());
  CHECK(ring.available() == 0);
}

static void testWrap(void)
{
  RingBuffer<8> ring;
  for (uint8_t val = 0; val < 6; val++)
    ring.put(val);
  ring.consume(5);                  // tail at 5, one byte (5) left
  CHECK(ring.available() == 1);
  for (uint8_t val = 6; val < 12; val++)
    CHECK(!ring.put(val).is_error());  // head wraps to 4
  CHECK(ring.isFull());
  CHECK(ring.available() == 7);

  // Bytes 5..7 sit at the end of the array, 8..11 wrapped to the start
  CHECK(ring.contiguous() == 3);
  const uint8_t *ptr = ring.readPtr();
  CHECK(ptr[0] == 5 && ptr[1] == 6 && ptr[2] == 7);

  ring.consume(3);
  CHECK(ring.available() == 4);
  CHECK(ring.contiguous() == 4);
  ptr = ring.readPtr();
  CHECK(ptr[0] == 8 && ptr[1] == 9 && ptr[2] == 10 && ptr[3] == 11);

  ring.consume(4);
  CHECK(ring.isEmpty());
  CHECK(ring.contiguous() == 0);
}

int main(void)
{
  testEmpty();
  testFull();
  testOrder();
  testWrap();

  if (failures) {
    printf("%d check(s) failed\n", failures);
    return EXIT_FAILURE;
  }
  printf("all checks passed\n");
  return EXIT_SUCCESS;
}
