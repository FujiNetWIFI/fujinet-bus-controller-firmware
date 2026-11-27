#ifndef NEXTOR_H
#define NEXTOR_H

// Include after board_defs.h and Mapper.h.

#include <cstdint>

#ifdef RD_PIN
#define NEXTOR_ROM_PATH STR(build/BOARD_NAME/nextor_rom.h)
#include NEXTOR_ROM_PATH

// The built-in Nextor kernel (ASCII16), served from subslot 2. The image is
// startup-copied into RAM with the rest of the data, so reads never wait on
// flash.
class Nextor
{
private:
  Mapper _mapper;

public:
  Nextor() {
    _mapper.setROMType(ROM_TYPE_MSX_ASCII16);
    _mapper.setROMSize(sizeof(nextor_rom));
    _mapper.activate();
  }

  // Core 1: back to the power-on banks, when a mode command enables Nextor.
  inline void activate(void) { _mapper.activate(); return; }

  // Core 1: the byte a read gets, or -1. Bank register writes switch banks.
  inline int access(BusSignals bus) {
    if (_mapper.isRemap(bus)) {
      _mapper.remap(bus);
      return -1;
    }
    if (Mapper::isWrite(bus) || !_mapper.inMapped(bus))
      return -1;
    return nextor_rom[_mapper.offset(bus)];
  }
};

#else // ! RD_PIN

// Without /RD there is no subslot 2: nothing to serve.
class Nextor
{
public:
  inline void activate(void) {}
  template <typename T> inline int access(T) { return -1; }
};

#endif // RD_PIN

#endif // NEXTOR_H
