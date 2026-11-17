#ifndef SECTORLINK_H
#define SECTORLINK_H

#include "WD1773.h"
#include <cstddef>
#include <cstdint>

// Fills SectorPort requests as a DriveWire client (OP_READEX, OP_WRITE) on
// the link the CoCo's own DriveWire traffic uses. While active() the caller
// must hand every incoming byte to receive() and not to the CoCo.
// No hardware access: bytes to send are returned to the caller.
//
// The host says nothing of an image's shape, so the number of sides is
// worked out before a drive is first used: from the OS-9 description in
// sector 0 if there is one, otherwise two sides only for an image of exactly
// 80 double-sided tracks. Nor does it say when an image is changed, so the
// answer is dropped once a drive has gone unused for GEOMETRY_MS.
class SectorLink {
public:
  static constexpr uint8_t OP_READEX = 0xD2;
  static constexpr uint8_t OP_WRITE = 0x57;
  static constexpr uint8_t ERROR_WRITE_PROTECT = 0xF2;
  static constexpr unsigned SECTORS_PER_TRACK = SectorPort::SECTORS_PER_TRACK;
  static constexpr unsigned DRIVES = 4;
  static constexpr uint32_t DOUBLE_SIDED_SECTORS = 80 * 2 * SECTORS_PER_TRACK;
  static constexpr uint32_t GEOMETRY_MS = 2000;
  static constexpr uint32_t READ_TIMEOUT_MS = 3000;
  static constexpr uint32_t WRITE_TIMEOUT_MS = 3000;
  static constexpr uint32_t DRAIN_MS = 100;
  static constexpr size_t HEADER_SIZE = 5;
  static constexpr size_t MAX_SEND = HEADER_SIZE + SectorPort::SECTOR_SIZE + 2;

  explicit SectorLink(SectorPort &port) : _port(port) {}

  inline bool active() const { return _state != State::IDLE; }

  // Call regularly. Returns how many bytes of `out` to send.
  size_t poll(uint32_t nowMs, uint8_t out[MAX_SEND]) {
    switch (_state) {
    case State::IDLE:
      if (_port.state != SectorPort::REQUESTED)
        return 0;
      if (_port.drive >= DRIVES) {
        _port.state = SectorPort::FAILED;
        return 0;
      }
      {
        uint8_t &known = _sides[_port.drive];
        uint32_t lsn;

        if (known && nowMs - _used[_port.drive] >= GEOMETRY_MS)
          known = 0;
        if (!known) {
          if (_probe == Probe::NONE)
            _probe = Probe::FIRST;
          lsn = _probe == Probe::FIRST ? 0
            : DOUBLE_SIDED_SECTORS - (_probe == Probe::LAST);
        }
        else {
          _used[_port.drive] = nowMs;
          if (_port.side >= known || _port.sector < 1
              || _port.sector > SECTORS_PER_TRACK) {
            _port.state = SectorPort::FAILED;
            return 0;
          }
          lsn = (_port.track * known + _port.side) * SECTORS_PER_TRACK
            + _port.sector - 1;
        }
        out[0] = _port.write && known ? OP_WRITE : OP_READEX;
        out[1] = _port.drive;
        out[2] = lsn >> 16;
        out[3] = lsn >> 8;
        out[4] = lsn;
      }
      _count = 0;
      _sum = 0;
      _started = nowMs;
      if (out[0] == OP_READEX) {
        _timeout = READ_TIMEOUT_MS;
        _state = State::DATA;
        return HEADER_SIZE;
      }
      for (unsigned idx = 0; idx < SectorPort::SECTOR_SIZE; idx++) {
        out[HEADER_SIZE + idx] = _port.data[idx];
        _sum += _port.data[idx];
      }
      out[MAX_SEND - 2] = _sum >> 8;
      out[MAX_SEND - 1] = _sum;
      _timeout = WRITE_TIMEOUT_MS;
      _state = State::STATUS;
      return MAX_SEND;

    case State::DATA:
    case State::STATUS:
      if (nowMs - _started >= _timeout) {
        _probe = Probe::NONE;
        _port.state = SectorPort::FAILED;
        _started = nowMs;
        _state = State::DRAIN;
      }
      return 0;

    default:
      // Late bytes from an abandoned reply must not reach the CoCo, so the
      // link stays closed until the host has been silent for DRAIN_MS.
      if (_drained) {
        _drained = false;
        _started = nowMs;
      }
      else if (nowMs - _started >= DRAIN_MS)
        _state = State::IDLE;
      return 0;
    }
  }

  // Returns how many bytes of `out` to send.
  size_t receive(uint8_t byte, uint8_t out[MAX_SEND]) {
    switch (_state) {
    case State::DATA:
      // A probe must leave a sector waiting to be written alone.
      if (_probe == Probe::NONE)
        _port.data[_count] = byte;
      else if (_count < sizeof(_lsn0))
        _lsn0[_count] = byte;
      _count++;
      _sum += byte;
      if (_count < SectorPort::SECTOR_SIZE)
        return 0;
      out[0] = _sum >> 8;
      out[1] = _sum;
      _state = State::STATUS;
      return 2;

    case State::STATUS:
      if (_probe != Probe::NONE)
        probed(byte == 0);
      else
        _port.state = byte == 0 ? SectorPort::READY
          : byte == ERROR_WRITE_PROTECT ? SectorPort::DENIED : SectorPort::FAILED;
      _state = State::IDLE;
      return 0;

    default:
      _drained = true;
      return 0;
    }
  }

private:
  enum class State : uint8_t { IDLE, DATA, STATUS, DRAIN };
  // Sector 0, then the last sector of an 80-track double-sided image, then
  // the one past it.
  enum class Probe : uint8_t { NONE, FIRST, LAST, PAST };
  // Offsets in an OS-9 sector 0.
  enum : uint8_t { DD_TOT = 0, DD_TKS = 3, DD_FMT = 0x10, DD_SPT = 0x11, LSN0_USED = 0x13 };

  SectorPort &_port;
  State _state = State::IDLE;
  unsigned _count = 0;
  uint16_t _sum = 0;
  uint32_t _started = 0, _timeout = 0;
  bool _drained = false;
  Probe _probe = Probe::NONE;
  uint8_t _sides[DRIVES] {};   // 0: not known
  uint32_t _used[DRIVES] {};
  uint8_t _lsn0[LSN0_USED] {};

  void learned(uint8_t sideCount) {
    _sides[_port.drive] = sideCount;
    _used[_port.drive] = _started;
    _probe = Probe::NONE;
  }

  // The request that set the probe off stays pending and is sent next.
  void probed(bool readable) {
    switch (_probe) {
    case Probe::FIRST:
      if (!readable) {
        _probe = Probe::NONE;
        _port.state = SectorPort::FAILED;
      }
      else if (_lsn0[DD_TKS] == SECTORS_PER_TRACK && _lsn0[DD_SPT] == 0
               && _lsn0[DD_SPT + 1] == SECTORS_PER_TRACK
               && (_lsn0[DD_TOT] | _lsn0[DD_TOT + 1] | _lsn0[DD_TOT + 2]))
        learned((_lsn0[DD_FMT] & 1) + 1);
      else
        _probe = Probe::LAST;
      break;
    case Probe::LAST:
      if (readable)
        _probe = Probe::PAST;
      else
        learned(1);
      break;
    default:
      learned(readable ? 1 : 2);
      break;
    }
  }
};

#endif // SECTORLINK_H
