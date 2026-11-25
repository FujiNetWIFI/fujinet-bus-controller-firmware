#ifndef SUBSLOT_H
#define SUBSLOT_H

// Include after board_defs.h, which supplies BusSignals.

#include <cstdint>

// The cartridge's slot layout, chosen by the mode byte of a ROM mode command.
// Expanded (power-on default): the subslot register is at FFFFh, subslot 0 holds
// CONFIG or the user ROM, subslot 1 holds the UNAPI ROM (page 1) and the IO
// window (BFFCh-BFFFh), subslots 2 and 3 are empty.
// Unexpanded with UNAPI: no register; every page is the UNAPI ROM and window.
// Unexpanded without UNAPI: everything is CONFIG or the user ROM, which has
// the IO window while the user ROM is inactive.
class Subslot
{
public:
  static constexpr uint16_t REGISTER_ADDR = 0xFFFF;
  // Mode bits of the control register, beside main.cpp's IO_FLAG_*.
  static constexpr uint8_t IO_FLAG_UNAPI_ENABLE = 0b00010000;
  static constexpr uint8_t IO_FLAG_EXPAND_SLOT  = 0b00100000;

  // What serves a cycle: nothing, CONFIG or the user ROM, or UNAPI and the IO window.
  enum Holder : uint8_t { HOLDER_NONE, HOLDER_MAIN, HOLDER_UNAPI };

private:
  uint8_t _reg = 0;
  bool _expanded = true;
  bool _unapiEnabled = true;
  volatile bool _powerOnPending = false;   // set by core 0, applied by core 1

public:
  inline bool isRegister(BusSignals bus) const {
    return _expanded && bus.addr == REGISTER_ADDR;
  }

  // Reads back inverted, which is how the BIOS tells the slot is expanded.
  inline uint8_t readRegister(void) const { return (uint8_t) ~_reg; }
  inline void writeRegister(uint8_t value) { _reg = value; return; }

  // Who serves the page holding bus.addr.
  inline Holder holder(BusSignals bus) const {
    if (_expanded) {
      uint8_t sub = (_reg >> ((bus.addr >> 14) * 2)) & 3;
      if (sub == 0)
        return HOLDER_MAIN;
      if (sub == 1)
        return HOLDER_UNAPI;
      return HOLDER_NONE;
    }
    return _unapiEnabled ? HOLDER_UNAPI : HOLDER_MAIN;
  }

  // Whether CONFIG / the user ROM shares its subslot with the IO window.
  inline bool mainHasIOWindow(void) const { return !_expanded; }

  // Applies the UNAPI and EXPAND bits of a ROM mode command.
  inline void setMode(uint8_t mode) {
    _unapiEnabled = mode & IO_FLAG_UNAPI_ENABLE;
    bool expanded = mode & IO_FLAG_EXPAND_SLOT;
    if (expanded != _expanded)
      _reg = 0;
    _expanded = expanded;
    return;
  }

  // Core 0: return to the power-on layout. Core 1 applies it between cycles.
  inline void requestPowerOn(void) { _powerOnPending = true; return; }
  inline void applyPowerOn(void) {
    if (_powerOnPending) {
      _powerOnPending = false;
      _expanded = true;
      _unapiEnabled = true;
      _reg = 0;
    }
    return;
  }
};

#endif // SUBSLOT_H
