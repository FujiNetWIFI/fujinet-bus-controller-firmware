#include "WD1773.h"

// Runs on the bus core, which cannot wait on a flash cache miss.
#if PICO_ON_DEVICE
#define FDC_FUNC __attribute__((noinline, section(".time_critical.wd1773")))
#else
#define FDC_FUNC
#endif

enum : uint8_t {
  CMD_NOT_TYPE_I   = 0x80,
  CMD_GROUP        = 0xF0,
  CMD_INTERRUPT    = 0xD0,
  CMD_READ_ADDRESS = 0xC0,
  CMD_WRITE_TRACK  = 0xF0,
  CMD_SECTOR_GROUP = 0xC0,
  CMD_SECTOR       = 0x80,   // read or write sector
  CMD_STEP_GROUP   = 0xE0,
  CMD_RESTORE_SEEK = 0x00,
  CMD_STEP_IN      = 0x40,
  CMD_STEP_OUT     = 0x60,
  CMD_SEEK         = 0x10,   // within CMD_RESTORE_SEEK
  CMD_STEP_RATE    = 0x03,
  CMD_VERIFY       = 0x04,   // type I
  CMD_SETTLE       = 0x04,   // types II, III
  CMD_UPDATE_TRACK = 0x10,   // step commands
  CMD_MULTIPLE     = 0x10,   // type II
  CMD_WRITE        = 0x20,   // type II
  CMD_INT_IMMEDIATE = 0x08,  // force interrupt
};

FDC_FUNC void WD1773::reset()
{
  _state = State::IDLE;
  _command = _track = _sector = _data = _flags = 0;
  _busy = _drqFlag = _intrqFlag = _intrqImmediate = false;
  _typeI = true;
  _direction = 1;
}

void WD1773::setDiskPresent(unsigned unit, bool isPresent, bool isProtected)
{
  _present[unit] = isPresent;
  _writeProtected[unit] = isProtected;
  refreshDrive();
}

FDC_FUNC uint8_t WD1773::peek(uint8_t reg, uint32_t now) const
{
  uint8_t value;

  switch (reg) {
  case REG_STATUS:
    value = _flags;
    if (_busy)
      value |= ST_BUSY;
    if (!_readyNow)
      value |= ST_NOT_READY;
    if (_typeI) {
      value |= _typeIBits;
      if (_readyNow && now % REVOLUTION_US < INDEX_PULSE_US)
        value |= ST_INDEX;
    }
    else if (_drqFlag)
      value |= ST_DRQ;
    return value;
  case REG_TRACK:
    return _track;
  case REG_SECTOR:
    return _sector;
  default:
    return _data;
  }
}

FDC_FUNC void WD1773::write(uint8_t reg, uint8_t value, uint32_t now)
{
  switch (reg) {
  case REG_COMMAND:
    startCommand(value, now);
    break;
  case REG_TRACK:
    _track = value;
    break;
  case REG_SECTOR:
    _sector = value;
    break;
  default:
    _drqFlag = false;
    _data = value;
    break;
  }
}

FDC_FUNC void WD1773::startCommand(uint8_t value, uint32_t now)
{
  // 6, 12, 20, 30 ms, packed so that no table is read from flash.
  static constexpr uint32_t STEP_MS = 0x1E140C06;

  if ((value & CMD_GROUP) == CMD_INTERRUPT) {
    _command = value;
    if (!_busy)
      _typeI = true;
    _state = State::IDLE;
    _busy = false;
    _intrqImmediate = value & CMD_INT_IMMEDIATE;
    if (_intrqImmediate)
      _intrqFlag = true;
    return;
  }

  if (_busy)
    return;

  _command = value;
  if (!_intrqImmediate)
    _intrqFlag = false;
  _busy = true;
  _drqFlag = false;

  if (!(value & CMD_NOT_TYPE_I)) {
    _typeI = true;
    _flags = 0;
    _stepMs = (STEP_MS >> (8 * (value & CMD_STEP_RATE))) & 0xFF;

    switch (value & CMD_STEP_GROUP) {
    case CMD_RESTORE_SEEK:
      if (!(value & CMD_SEEK)) {
        _track = RESTORE_TRACK;
        _data = 0;
      }
      _state = State::SEEK;
      _deadline = now;
      return;
    case CMD_STEP_IN:
      _direction = 1;
      break;
    case CMD_STEP_OUT:
      _direction = -1;
      break;
    }
    if (value & CMD_UPDATE_TRACK)
      _track += _direction;
    step();
    _state = State::VERIFY;
    _deadline = now + _stepMs * 1000;
    return;
  }

  _typeI = false;
  _flags = 0;
  _idOnly = false;

  if (!ready()) {
    finish(0);
    return;
  }

  uint32_t settle = (value & CMD_SETTLE) ? SETTLE_US : 0;

  if ((value & CMD_SECTOR_GROUP) == CMD_SECTOR) {
    _firstSector = true;
    if (value & CMD_WRITE) {
      if (_writeProtected[_drive]) {
        finishAfter(ST_WRITE_PROTECT, now, settle);
        return;
      }
      _state = State::WRITE_START;
      _searchStart = now + settle;
      _deadline = _searchStart + ID_TO_DATA_US;
      return;
    }
    _state = State::FETCH;
    _deadline = now + settle;
    _searchStart = _deadline;
    return;
  }

  if ((value & CMD_GROUP) == CMD_READ_ADDRESS) {
    _idOnly = true;
    _state = State::FETCH;
    _deadline = now + settle;
    _searchStart = _deadline;
    return;
  }

  // Write Track is not implemented.
  if ((value & CMD_GROUP) == CMD_WRITE_TRACK) {
    finishAfter(ST_WRITE_PROTECT, now, settle);
    return;
  }

  // Read Track is not implemented.
  finishAfter(ST_RNF, now, settle + SEARCH_TIMEOUT_US);
}

FDC_FUNC void WD1773::step()
{
  if (_drive == NO_DRIVE)
    return;
  if (_direction > 0) {
    if (_head[_drive] < MAX_TRACK)
      _head[_drive]++;
  }
  else if (_head[_drive] > 0)
    _head[_drive]--;
  _driveDirty = true;
}

// Read Address: the ID field that would pass under the head next. Sector 1
// of the track has been fetched first, to prove that the track exists.
FDC_FUNC void WD1773::loadId(uint32_t now)
{
  static constexpr uint32_t SECTOR_US = REVOLUTION_US / SectorPort::SECTORS_PER_TRACK;
  // CRC of the three sync bytes and the ID address mark.
  static constexpr uint16_t ID_CRC_START = 0xB230;
  static constexpr uint16_t CRC_POLYNOMIAL = 0x1021, CRC_TOP_BIT = 0x8000;
  static constexpr uint8_t SIZE_CODE_256 = 1;
  static constexpr unsigned CRC_SIZE = 2;
  uint16_t crc = ID_CRC_START;
  uint8_t *field = _port.data;

  field[0] = _head[_drive];
  field[1] = _side;
  field[2] = (now % REVOLUTION_US) / SECTOR_US + 1;
  field[3] = SIZE_CODE_256;
  for (unsigned idx = 0; idx < ID_SIZE - CRC_SIZE; idx++) {
    crc ^= field[idx] << 8;
    for (unsigned bit = 0; bit < 8; bit++)
      crc = (crc & CRC_TOP_BIT) ? (crc << 1) ^ CRC_POLYNOMIAL : crc << 1;
  }
  field[4] = crc >> 8;
  field[5] = crc;
  _sector = field[0];
  _length = ID_SIZE;
}

FDC_FUNC void WD1773::finish(uint8_t statusFlags)
{
  _flags |= statusFlags;
  _busy = false;
  _intrqFlag = true;
  _state = State::IDLE;
}

FDC_FUNC void WD1773::finishAfter(uint8_t statusFlags, uint32_t now, uint32_t delay)
{
  _finishFlags = statusFlags;
  _state = State::FINISH;
  _deadline = now + delay;
}

FDC_FUNC void WD1773::advance(uint32_t now)
{
  // States that wait on the port look again later, not on every call.
  if (_state == State::FETCH || _state == State::FETCH_WAIT
      || _state == State::WRITE_START || _state == State::WRITE_WAIT)
    _deadline = now + PORT_POLL_US;

  switch (_state) {
  case State::SEEK:
    if (_data == _track) {
      _state = State::VERIFY;
      return;
    }
    _direction = _data > _track ? 1 : -1;
    _track += _direction;
    if (_direction < 0 && track0()) {
      _track = 0;
      _state = State::VERIFY;
    }
    else
      step();
    _deadline = now + _stepMs * 1000;
    return;

  case State::VERIFY:
    if (!(_command & CMD_VERIFY))
      finish(0);
    else if (ready() && _head[_drive] == _track)
      finishAfter(0, now, ID_TO_DATA_US);
    else
      finishAfter(ST_SEEK_ERROR, now, SEARCH_TIMEOUT_US);
    return;

  case State::FINISH:
    finish(_finishFlags);
    return;

  case State::FETCH:
    // A request abandoned by Force Interrupt may still be in service.
    if (_port.state == SectorPort::REQUESTED)
      return;
    _port.state = SectorPort::IDLE;
    if (!ready() || (!_idOnly && _head[_drive] != _track)) {
      finishAfter(ST_RNF, _searchStart, SEARCH_TIMEOUT_US);
      return;
    }
    _port.write = false;
    _port.drive = _drive;
    _port.track = _idOnly ? _head[_drive] : _track;
    _port.side = _side;
    _port.sector = _idOnly ? 1 : _sector;
    _port.state = SectorPort::REQUESTED;
    _state = State::FETCH_WAIT;
    return;

  case State::FETCH_WAIT:
    if (_port.state == SectorPort::READY) {
      _port.state = SectorPort::IDLE;
      _index = 0;
      _length = SectorPort::SECTOR_SIZE;
      if (_idOnly)
        loadId(now);
      _state = State::READ_BYTE;
      _deadline = now + ID_TO_DATA_US;
    }
    else if (_port.state != SectorPort::REQUESTED) {
      _port.state = SectorPort::IDLE;
      finishAfter(ST_RNF, _searchStart, SEARCH_TIMEOUT_US);
    }
    else if (now - _searchStart >= HOST_TIMEOUT_US)
      finish(ST_RNF);
    return;

  case State::READ_BYTE:
    if (_drqFlag)
      _flags |= ST_LOST_DATA;
    _data = _port.data[_index++];
    _drqFlag = true;
    if (_index < _length)
      _deadline = now + BYTE_US;
    else {
      _state = State::READ_END;
      _deadline = now + CRC_US;
    }
    return;

  case State::READ_END:
    if (!_idOnly && (_command & CMD_MULTIPLE)) {
      _sector++;
      _state = State::FETCH;
      _searchStart = now;
    }
    else
      finish(0);
    return;

  case State::WRITE_START:
    // The sector is collected in the port's buffer, so it must be free.
    if (_port.state == SectorPort::REQUESTED)
      return;
    _port.state = SectorPort::IDLE;
    if (!ready() || _head[_drive] != _track) {
      finishAfter(ST_RNF, _searchStart, SEARCH_TIMEOUT_US);
      return;
    }
    _index = 0;
    _drqFlag = true;
    _state = State::WRITE_BYTE;
    _deadline = now + FIRST_WRITE_BYTE_US;
    return;

  case State::WRITE_BYTE:
    if (_drqFlag) {
      if (_index == 0) {
        _drqFlag = false;
        finish(ST_LOST_DATA);
        return;
      }
      _flags |= ST_LOST_DATA;
      _data = 0;
    }
    _port.data[_index++] = _data;
    if (_index < SectorPort::SECTOR_SIZE) {
      _drqFlag = true;
      _deadline = now + BYTE_US;
      return;
    }
    _port.write = true;
    _port.drive = _drive;
    _port.track = _track;
    _port.side = _side;
    _port.sector = _sector;
    _port.state = SectorPort::REQUESTED;
    _state = State::WRITE_WAIT;
    _searchStart = now;
    return;

  case State::WRITE_WAIT:
    if (_port.state == SectorPort::READY) {
      _port.state = SectorPort::IDLE;
      if (_command & CMD_MULTIPLE) {
        _sector++;
        _firstSector = false;
        _state = State::WRITE_START;
        _searchStart = now;
        _deadline = now + ID_TO_DATA_US;
      }
      else
        finish(0);
    }
    else if (_port.state == SectorPort::DENIED) {
      _port.state = SectorPort::IDLE;
      finish(ST_WRITE_PROTECT);
    }
    else if (_port.state == SectorPort::FAILED) {
      _port.state = SectorPort::IDLE;
      // Running off the end of the track is how a multiple write ends.
      finish(_firstSector ? ST_WRITE_FAULT : ST_RNF);
    }
    else if (now - _searchStart >= HOST_TIMEOUT_US)
      finish(ST_WRITE_FAULT);
    return;

  default:
    return;
  }
}
