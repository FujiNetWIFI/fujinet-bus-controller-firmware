#ifndef SUBSLOT_H
#define SUBSLOT_H

// Include after board_defs.h, which supplies BusSignals and BUS_ROM_BASE.

#include <cstdint>

#ifdef RD_PIN
#define UNAPI_ROM_PATH STR(build/BOARD_NAME/unapi_rom.h)
#include UNAPI_ROM_PATH

// The cartridge's slot layout, chosen by the mode byte of a ROM mode command.
// Expanded (power-on default): the subslot register is at FFFFh, subslot 0 holds
// CONFIG or the user ROM, subslot 1 holds the UNAPI ROM (page 1) and the IO
// window (BFFCh-BFFFh), subslot 2 holds the Nextor kernel when the mode enables
// it, subslot 3 is empty. With Nextor and no user ROM, subslot 0 is left empty
// so CONFIG doesn't run again.
// Unexpanded with UNAPI: no register; every page is the UNAPI ROM and window.
// Unexpanded without UNAPI: everything is CONFIG or the user ROM, which has
// the IO window while the user ROM is inactive.
class Subslot
{
public:
  static constexpr uint16_t REGISTER_ADDR = 0xFFFF;
  // Mode bits of the control register, beside main.cpp's IO_FLAG_*.
  // On /RD boards the autostart bit enables Nextor instead.
  static constexpr uint8_t IO_FLAG_NEXTOR_ENABLE = 0b00000010;
  static constexpr uint8_t IO_FLAG_USERROM_ON   = 0b00000001;
  static constexpr uint8_t IO_FLAG_UNAPI_ENABLE = 0b00010000;
  static constexpr uint8_t IO_FLAG_EXPAND_SLOT  = 0b00100000;

  // What serves a cycle: nothing, CONFIG or the user ROM, UNAPI and the IO window,
  // or the Nextor kernel.
  enum Holder : uint8_t { HOLDER_NONE, HOLDER_MAIN, HOLDER_UNAPI, HOLDER_NEXTOR };

private:
  uint8_t _reg = 0;
  bool _expanded = true;
  bool _unapiEnabled = true;
  bool _nextorEnabled = false;
  bool _mainEmpty = false;   // Nextor without a user ROM: subslot 0 holds nothing
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
        return _mainEmpty ? HOLDER_NONE : HOLDER_MAIN;
      if (sub == 1)
        return HOLDER_UNAPI;
      if (sub == 2)
        return _nextorEnabled ? HOLDER_NEXTOR : HOLDER_NONE;
      return HOLDER_NONE;
    }
    return _unapiEnabled ? HOLDER_UNAPI : HOLDER_MAIN;
  }

  // Whether the IO window answers in the page holding the cycle. UNAPI's
  // window stays live under a running user ROM; the legacy one doesn't.
  inline bool ioWindowOpen(Holder holder, bool userRomActive) const {
    return holder == HOLDER_UNAPI || (!_expanded && !userRomActive);
  }

  // The UNAPI ROM byte at addr, or -1 outside the ROM.
  static inline int unapiByte(uint16_t addr) {
    if (BUS_ROM_BASE <= addr && addr < BUS_ROM_BASE + sizeof(unapi_rom))
      return unapi_rom[addr - BUS_ROM_BASE];
    return -1;
  }

  inline bool nextorEnabled(void) const { return _nextorEnabled; }

  // Applies the UNAPI, EXPAND and Nextor bits of a ROM mode command.
  inline void setMode(uint8_t mode) {
    _unapiEnabled = mode & IO_FLAG_UNAPI_ENABLE;
    bool expanded = mode & IO_FLAG_EXPAND_SLOT;
    if (expanded != _expanded)
      _reg = 0;
    _expanded = expanded;
    _nextorEnabled = expanded && (mode & IO_FLAG_NEXTOR_ENABLE);
    _mainEmpty = _nextorEnabled && !(mode & IO_FLAG_USERROM_ON);
    return;
  }

  // Core 0: return to the power-on layout. Core 1 applies it between cycles.
  inline void requestPowerOn(void) { _powerOnPending = true; return; }
  inline void applyPowerOn(void) {
    if (_powerOnPending) {
      _powerOnPending = false;
      _expanded = true;
      _unapiEnabled = true;
      _nextorEnabled = false;
      _mainEmpty = false;
      _reg = 0;
    }
    return;
  }
};

#else // ! RD_PIN

// Without /RD there is no FFFFh decode: one unexpanded slot, no UNAPI.
class Subslot
{
public:
  enum Holder : uint8_t { HOLDER_NONE, HOLDER_MAIN, HOLDER_UNAPI, HOLDER_NEXTOR };

  template <typename T> inline bool isRegister(T) const { return false; }
  inline uint8_t readRegister(void) const { return 0xFF; }
  inline void writeRegister(uint8_t) {}
  template <typename T> inline Holder holder(T) const { return HOLDER_MAIN; }
  inline bool ioWindowOpen(Holder, bool userRomActive) const { return !userRomActive; }
  static inline int unapiByte(uint16_t) { return -1; }
  inline bool nextorEnabled(void) const { return false; }
  inline void setMode(uint8_t) {}
  inline void requestPowerOn(void) {}
  inline void applyPowerOn(void) {}
};

#endif // RD_PIN

#endif // SUBSLOT_H
