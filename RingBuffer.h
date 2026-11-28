#ifndef RINGBUFFER_H
#define RINGBUFFER_H

#include "types.h"

#include <cstddef>
#include <cstdint>

// Single-producer, single-consumer byte FIFO. One slot stays empty so a
// full ring can be told apart from an empty one.
template <size_t SIZE>
class RingBuffer
{
  static_assert(SIZE >= 2 && (SIZE & (SIZE - 1)) == 0, "SIZE must be a power of two");

private:
  uint8_t _data[SIZE];
  size_t _head = 0;  // next slot to write
  size_t _tail = 0;  // oldest unread byte

  static inline size_t wrap(size_t idx) { return idx & (SIZE - 1); }

public:
  inline bool isEmpty(void) const { return _head == _tail; }

  inline bool isFull(void) const { return wrap(_head + 1) == _tail; }

  // Bytes waiting to be read
  inline size_t available(void) const { return wrap(_head - _tail); }

  // Error (byte not stored) if full
  inline error_is_true put(uint8_t val)
  {
    if (isFull())
      RETURN_ERROR_AS_TRUE();

    _data[_head] = val;
    _head = wrap(_head + 1);
    RETURN_SUCCESS_AS_FALSE();
  }

  // Oldest byte; caller checks !isEmpty()
  inline uint8_t peek(void) const { return _data[_tail]; }

  inline const uint8_t *readPtr(void) const { return &_data[_tail]; }

  // Bytes readable at readPtr() before the end of the array or _head
  inline size_t contiguous(void) const
  {
    return (_head >= _tail) ? (_head - _tail) : (SIZE - _tail);
  }

  // Drops count bytes from the tail; count <= available()
  inline void consume(size_t count) { _tail = wrap(_tail + count); }
};

#endif /* RINGBUFFER_H */
