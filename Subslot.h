#ifndef SUBSLOT_H
#define SUBSLOT_H

// Include after board_defs.h, which supplies BusSignals.

#include <cstdint>

// The cartridge's slot is expanded. Subslot 0 holds CONFIG or the user ROM and
// the other three are empty; the subslot register is at FFFFh.
class Subslot
{
private:
  uint8_t _reg = 0;

public:
  static constexpr uint16_t REGISTER_ADDR = 0xFFFF;

  inline bool isRegister(BusSignals bus) const { return bus.addr == REGISTER_ADDR; }

  // Reads back inverted, which is how the BIOS tells the slot is expanded.
  inline uint8_t readRegister(void) const { return (uint8_t) ~_reg; }
  inline void writeRegister(uint8_t value) { _reg = value; return; }

  // Whether the page holding bus.addr has subslot 0 selected.
  inline bool isSelected(BusSignals bus) const {
    return ((_reg >> ((bus.addr >> 14) * 2)) & 3) == 0;
  }
};

#endif // SUBSLOT_H
