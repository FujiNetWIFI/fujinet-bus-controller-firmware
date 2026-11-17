#ifndef WD1773_H
#define WD1773_H

#include <cstdint>

// One sector in transit between the controller and whatever holds the disk
// image. The controller moves IDLE->REQUESTED and any result->IDLE; the
// servicing side moves REQUESTED->READY, FAILED or DENIED (write-protected).
// For a write, `data` is filled before the request.
struct SectorPort {
  enum State : uint8_t { IDLE, REQUESTED, READY, FAILED, DENIED };
  static constexpr unsigned SECTOR_SIZE = 256;
  static constexpr unsigned SECTORS_PER_TRACK = 18;

  volatile State state = IDLE;
  bool write = false;
  uint8_t drive = 0, track = 0, side = 0, sector = 0;
  uint8_t data[SECTOR_SIZE];
};

// Sector-level WD1773. No hardware access: time comes in as microseconds.
class WD1773 {
public:
  enum Register : uint8_t { REG_STATUS = 0, REG_COMMAND = 0, REG_TRACK, REG_SECTOR, REG_DATA };

  enum Status : uint8_t {
    ST_BUSY          = 0x01,
    ST_DRQ           = 0x02,   // types II, III
    ST_INDEX         = 0x02,   // type I
    ST_LOST_DATA     = 0x04,   // types II, III
    ST_TRACK0        = 0x04,   // type I
    ST_CRC_ERROR     = 0x08,
    ST_RNF           = 0x10,   // types II, III
    ST_SEEK_ERROR    = 0x10,   // type I
    ST_WRITE_FAULT   = 0x20,   // types II, III
    ST_WRITE_PROTECT = 0x40,
    ST_NOT_READY     = 0x80,
  };

  static constexpr unsigned DRIVES = 4;
  static constexpr uint8_t MAX_TRACK = 79;
  static constexpr int NO_DRIVE = -1;
  static constexpr uint8_t REG_MASK = 3;
  // Restore seeks to track 0 from here.
  static constexpr uint8_t RESTORE_TRACK = 0xFF;

  static constexpr uint32_t BYTE_US = 32;
  static constexpr uint32_t CRC_US = 2 * BYTE_US;
  static constexpr uint32_t ID_TO_DATA_US = 2000;
  static constexpr uint32_t PORT_POLL_US = 50;
  static constexpr uint32_t FIRST_WRITE_BYTE_US = 22 * BYTE_US;
  static constexpr uint32_t SETTLE_US = 30000;
  static constexpr uint32_t REVOLUTION_US = 200000;
  static constexpr uint32_t INDEX_PULSE_US = 4000;
  static constexpr uint32_t SEARCH_TIMEOUT_US = 5 * REVOLUTION_US;
  static constexpr uint32_t HOST_TIMEOUT_US = 4000000;
  static constexpr unsigned ID_SIZE = 6;

  explicit WD1773(SectorPort &port) : _port(port) {}

  void reset();
  // peek() is what a read returns; acknowledge() is what a read then does.
  uint8_t peek(uint8_t reg, uint32_t now) const;
  inline void acknowledge(uint8_t reg) {
    if (reg == REG_STATUS) {
      if (!_intrqImmediate)
        _intrqFlag = false;
    }
    else if (reg == REG_DATA)
      _drqFlag = false;
  }
  inline uint8_t read(uint8_t reg, uint32_t now) {
    uint8_t value = peek(reg, now);
    acknowledge(reg);
    return value;
  }
  void write(uint8_t reg, uint8_t value, uint32_t now);

  // Does at most one short piece of work, so that a caller with a deadline
  // can interleave it. Returns true if anything changed.
  inline bool tick(uint32_t now) {
    if (_driveDirty) {
      _driveDirty = false;
      refreshDrive();
      return true;
    }
    if (_state == State::IDLE || (int32_t) (now - _deadline) < 0)
      return false;
    advance(now);
    return true;
  }

  inline bool active() const { return _state != State::IDLE || _driveDirty; }

  // True while a command can go no further until the host has answered.
  inline bool waitingForHost() const {
    return _state == State::FETCH || _state == State::FETCH_WAIT
      || _state == State::WRITE_WAIT;
  }

  // True when tick() has something to do.
  inline bool due(uint32_t now) const {
    return _driveDirty
      || (_state != State::IDLE && (int32_t) (now - _deadline) >= 0);
  }
  inline bool drq() const { return _drqFlag; }
  inline bool intrq() const { return _intrqFlag; }
  inline uint8_t trackRegister() const { return _track; }
  inline uint8_t sectorRegister() const { return _sector; }
  inline uint8_t dataRegister() const { return _data; }

  inline void select(int newDrive, bool newSide, bool newMotor) {
    // What is read comes from the drive and side selected when the sector
    // passes the head, and OS-9's Boot selects the side after the command.
    if (_state == State::READ_BYTE && _index == 0
        && (newDrive != _drive || newSide != _side))
      _state = State::FETCH;
    _drive = newDrive;
    _side = newSide;
    _motor = newMotor;
    refreshDrive();
  }
  void setDiskPresent(unsigned unit, bool isPresent, bool isProtected);

private:
  enum class State : uint8_t {
    IDLE, SEEK, VERIFY, FINISH, FETCH, FETCH_WAIT, READ_BYTE, READ_END,
    WRITE_START, WRITE_BYTE, WRITE_WAIT,
  };

  SectorPort &_port;

  State _state = State::IDLE;
  uint32_t _deadline = 0, _searchStart = 0;

  uint8_t _command = 0, _track = 0, _sector = 0, _data = 0;
  uint8_t _flags = 0;          // latched status bits
  uint8_t _finishFlags = 0;    // applied when FINISH expires
  bool _busy = false, _typeI = true;
  bool _drqFlag = false, _intrqFlag = false, _intrqImmediate = false;

  int _direction = 1;
  uint8_t _stepMs = 6;
  unsigned _index = 0, _length = 0;
  bool _firstSector = true, _idOnly = false;

  int _drive = NO_DRIVE;
  bool _side = false, _motor = false;
  uint8_t _head[DRIVES] {};
  bool _present[DRIVES] {}, _writeProtected[DRIVES] {};

  // Derived from the selected drive; kept so that status reads stay cheap.
  bool _readyNow = false, _driveDirty = false;
  uint8_t _typeIBits = 0;

  inline void refreshDrive() {
    _readyNow = false;
    _typeIBits = 0;
    if (_drive == NO_DRIVE)
      return;
    _readyNow = _motor && _present[_drive];
    if (_head[_drive] == 0)
      _typeIBits |= ST_TRACK0;
    if (_writeProtected[_drive])
      _typeIBits |= ST_WRITE_PROTECT;
  }

  inline bool ready() const { return _readyNow; }
  inline bool track0() const { return _typeIBits & ST_TRACK0; }

  void startCommand(uint8_t value, uint32_t now);
  void advance(uint32_t now);
  void step();
  void loadId(uint32_t now);
  void finishAfter(uint8_t statusFlags, uint32_t now, uint32_t delay);
  void finish(uint8_t statusFlags);
};

#endif // WD1773_H
