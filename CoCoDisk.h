#ifndef COCODISK_H
#define COCODISK_H

// Include once, from main.cpp, after board_defs.h. A board gets the disk
// controller by defining HALT_PIN, NMI_PIN, DISK_LATCH and FDC_BASE in its
// .pio file and building WD1773.cpp (see CMakeLists.txt).

#include <cstdint>

#ifdef HALT_PIN

#include "SectorLink.h"
#include "WD1773.h"
#include <pico/stdlib.h>

#define DISK_FUNC __attribute__((noinline, section(".time_critical.cocodisk")))

class CoCoDisk {
public:
  enum : uint8_t {
    LATCH_DRIVE0      = 0x01,
    LATCH_DRIVE1      = 0x02,
    LATCH_DRIVE2      = 0x04,
    LATCH_DRIVES_0_2  = 0x07,
    LATCH_MOTOR       = 0x08,
    LATCH_NMI_ENABLE  = 0x20,
    LATCH_DRIVE3_SIDE = 0x40,
    LATCH_HALT_ENABLE = 0x80,
  };

  void init() {
    // nextCycleWrite() times itself with the cycle counter.
    m33_hw->demcr |= M33_DEMCR_TRCENA_BITS;
    m33_hw->dwt_ctrl |= M33_DWT_CTRL_CYCCNTENA_BITS;
    initPin(HALT_PIN);
    initPin(NMI_PIN);
    // The host does not report what is mounted: an empty or read-only slot
    // shows up as a failed transfer.
    for (unsigned drive = 0; drive < WD1773::DRIVES; drive++)
      _fdc.setDiskPresent(drive, true, false);
    refresh();
  }

  static inline bool isSelected(BusSignals bus) {
    return (bus.addr & (uint16_t) ~WD1773::REG_MASK) == FDC_BASE
      || (bus.addr == DISK_LATCH && !bus.rw);
  }

  // The select detector can fire on the cycle after an answered read, with
  // no select line asserted. A ROM byte served then is fatal to software
  // running from RAM in the ROM window (OS-9).
  static inline bool romSelected() { return !gpio_get(CTS_PIN); }

  // Bus core. A read must be answered within the bus cycle, at up to
  // 1.78MHz, so the answer is kept ready and the work is done afterwards.
  inline uint8_t peek(uint16_t addr) const { return _ready[addr & WD1773::REG_MASK]; }
  void afterRead(uint16_t addr);
  void write(uint16_t addr, uint8_t value);

  // Bounds a wait on E, so that a stopped clock cannot hang the bus core.
  static constexpr unsigned SPIN_LIMIT = 400;
  // The most a command can advance in one go.
  static constexpr unsigned CATCH_UP_TICKS = 3;

  // The work that follows a reply starts once its bus cycle is over.
  static inline void waitCycleEnd() {
    unsigned spins = 0;

    while (gpio_get(CLOCK_PIN) && ++spins < SPIN_LIMIT)
      ;
  }

  // A 16-bit store writes two registers on back-to-back bus cycles (STD
  // $FF49 sets track and sector together), and the select detector reports
  // only the first of two cycles that select the cart alike. So after a
  // register write the next cycle is read straight off the pins.
  static constexpr unsigned BUS_SETTLE_CYCLES = 30;

  inline bool nextCycleWrite(uint16_t addr, uint8_t &value) {
    unsigned spins = 0;

    while (gpio_get(CLOCK_PIN) && ++spins < SPIN_LIMIT)
      ;
    while (!gpio_get(CLOCK_PIN) && ++spins < 2 * SPIN_LIMIT)
      ;
    uint32_t start = m33_hw->dwt_cyccnt;
    while (m33_hw->dwt_cyccnt - start < BUS_SETTLE_CYCLES)
      ;

    BusSignals next;
    next.combined = sio_hw->gpio_in;
    if (next.scs || next.rw || next.addr != (uint16_t) (addr + 1))
      return false;
    value = next.data;
    return true;
  }

  // Code running from the cart's ROM reads the cart on every cycle, so this
  // is the only place to advance the controller while it does; see run().
  inline void afterRomRead() {
    _fromRom = true;
    if (_pending || _fdc.due(time_us_32()))
      piece();
  }

  // Called once every bus event has been dealt with, until the next one.
  //
  // A late reply is lost or lands on a later cycle, so nothing may be in
  // progress when a read of the cart arrives. The controller is only
  // advanced where the next one is known to be some way off:
  //  - after a controller access from code in RAM (afterRead/write);
  //  - after a ROM read, one piece, when the code runs from the cart's ROM;
  //  - once HALT has been low for HALT_SETTLE_US. By the clock, not by a
  //    quiet bus: a halted CPU floats the address lines, which can keep the
  //    select detector firing;
  //  - once the bus has been quiet for QUIET_CYCLES, one piece per cycle,
  //    for software that starts a command and waits for the NMI. The quiet
  //    must far exceed the gap between polls in a wait loop (Disk BASIC:
  //    about 85 cycles).
  // A piece starts GUARD loop passes after E rises with no select announced.
  static constexpr unsigned GUARD = 3;
  static constexpr uint32_t HALT_SETTLE_US = 40;
  static constexpr unsigned QUIET_CYCLES = 2048;

  template <typename BusQuiet> inline void run(BusQuiet busQuiet) {
    watchReset();
    if (_haltLow)
      whileHalted();

    bool wasHigh = true;
    unsigned highFor = 0, quiet = 0;

    // HALT keeps the loop alive: a halted CPU makes no bus events of its
    // own, so a reset that should release it would otherwise go unnoticed.
    while ((_fdc.active() || _pending || _haltLow) && busQuiet()) {
      if (!gpio_get(CLOCK_PIN)) {
        wasHigh = false;
        highFor = 0;
      }
      else if (!wasHigh) {
        wasHigh = true;
        highFor = 1;
      }
      else if (highFor && ++highFor > GUARD) {
        highFor = 0;
        watchReset();
        if (_haltLow)
          whileHalted();
        else if (quiet < QUIET_CYCLES)
          quiet++;
        else if (_pending || _fdc.due(time_us_32()))
          piece();
      }
    }
  }

private:
  SectorPort _port;
  WD1773 _fdc { _port };

public:
  // Other core: the host link that fetches sectors.
  SectorLink link { _port };

private:
  uint8_t _latch = 0;
  uint8_t _ready[4] {};
  enum : uint8_t { PEND_REFRESH = 1, PEND_PINS = 2 };
  uint8_t _pending = 0;
  bool _fromRom = false;
  bool _haltEnable = false, _haltLow = false, _nmiLow = false;
  bool _inReset = false;
  uint32_t _haltSince = 0;

  void piece();
  void settle();
  void hardReset();

  // Acts once per press, on the sampled level.
  inline void watchReset() {
    if (gpio_get(RESET_PIN))
      _inReset = false;
    else if (!_inReset) {
      _inReset = true;
      hardReset();
    }
  }

  inline void whileHalted() {
    uint32_t now = time_us_32();

    if (now - _haltSince >= HALT_SETTLE_US && (_pending || _fdc.due(now)))
      settle();
  }

  // Open-drain: output latch 0, direction selects asserted or released.
  static void initPin(unsigned pin) {
    gpio_init(pin);
    gpio_put(pin, 0);
    gpio_set_dir(pin, GPIO_IN);
    gpio_disable_pulls(pin);
  }

  inline void refresh() {
    _ready[WD1773::REG_TRACK] = _fdc.trackRegister();
    _ready[WD1773::REG_SECTOR] = _fdc.sectorRegister();
    _ready[WD1773::REG_DATA] = _fdc.dataRegister();
    _ready[WD1773::REG_STATUS] = _fdc.peek(WD1773::REG_STATUS, time_us_32());
  }

  inline void updatePins() {
    if (_fdc.intrq())
      _haltEnable = false;

    // The CPU is also stopped while a command waits on the host, which can
    // take far longer than a drive: software gives up after half a second.
    bool halt = (_haltEnable && !_fdc.drq()) || _fdc.waitingForHost();
    bool nmi = _fdc.intrq() && (_latch & LATCH_NMI_ENABLE);
    if (halt == _haltLow && nmi == _nmiLow)
      return;

    gpio_set_dir(HALT_PIN, halt);
    gpio_set_dir(NMI_PIN, nmi);
    if (halt && !_haltLow)
      _haltSince = time_us_32();
    _haltLow = halt;
    _nmiLow = nmi;
  }
};

// HALT and NMI are always settled straight after an access, because the CPU
// acts on them within an instruction. The rest is too, unless the access
// came from code running in the cart's ROM: see run().
DISK_FUNC void CoCoDisk::afterRead(uint16_t addr)
{
  waitCycleEnd();
  _fdc.acknowledge(addr & WD1773::REG_MASK);
  updatePins();
  if (_fromRom) {
    _fromRom = false;
    _pending |= PEND_REFRESH;
  }
  else
    settle();
}

DISK_FUNC void CoCoDisk::write(uint16_t addr, uint8_t value)
{
  if (addr != DISK_LATCH) {
    uint8_t reg = addr & WD1773::REG_MASK;
    uint8_t second = 0;
    // Not for code running from the cart's ROM, whose next cycle is a
    // fetch that has to be answered.
    bool pair = !_fromRom && reg < WD1773::REG_DATA
      && nextCycleWrite(addr, second);
    uint32_t now = time_us_32();

    // The chip may have finished something since it was last looked at, and
    // would refuse a command it still believed itself busy for.
    for (unsigned idx = 0; idx < CATCH_UP_TICKS && _fdc.tick(now); idx++)
      ;
    _fdc.write(reg, value, now);
    if (pair)
      _fdc.write(reg + 1, second, now);
  }
  else {
    int drive = WD1773::NO_DRIVE;

    _latch = value;
    if (value & LATCH_DRIVE0)
      drive = 0;
    else if (value & LATCH_DRIVE1)
      drive = 1;
    else if (value & LATCH_DRIVE2)
      drive = 2;
    else if (value & LATCH_DRIVE3_SIDE)
      drive = 3;
    _fdc.select(drive,
               (value & LATCH_DRIVE3_SIDE) && (value & LATCH_DRIVES_0_2),
               value & LATCH_MOTOR);
    _haltEnable = value & LATCH_HALT_ENABLE;
  }
  updatePins();
  if (_fromRom) {
    _fromRom = false;
    _pending |= PEND_REFRESH;
  }
  else
    settle();
}

// RESET clears the real controller's latch and chip, releasing HALT and NMI.
DISK_FUNC void CoCoDisk::hardReset()
{
  _fdc.reset();
  _fdc.select(WD1773::NO_DRIVE, false, false);
  _latch = 0;
  _haltEnable = false;
  updatePins();
  refresh();
  _pending = 0;
  _fromRom = false;
}

// Brings the controller fully up to date in one go.
DISK_FUNC void CoCoDisk::settle()
{
  uint32_t now = time_us_32();

  for (unsigned idx = 0; idx < CATCH_UP_TICKS && _fdc.tick(now); idx++)
    ;
  refresh();
  updatePins();
  _pending = 0;
}

// The same, one step per call. Registers before pins, so that the data is in
// place before a release of HALT lets the CPU come for it.
DISK_FUNC void CoCoDisk::piece()
{
  if (_pending & PEND_REFRESH) {
    refresh();
    _pending &= ~PEND_REFRESH;
  }
  else if (_pending & PEND_PINS) {
    updatePins();
    _pending &= ~PEND_PINS;
  }
  else if (_fdc.tick(time_us_32()))
    _pending = PEND_REFRESH | PEND_PINS;
}

#else // ! HALT_PIN

class CoCoDisk {
public:
  void init() {}
  template <typename T> static inline bool isSelected(T) { return false; }
  inline uint8_t peek(uint16_t) const { return 0; }
  inline void afterRead(uint16_t) {}
  inline void write(uint16_t, uint8_t) {}
  inline void afterRomRead() {}
  static inline bool romSelected() { return true; }
  template <typename BusQuiet> inline void run(BusQuiet) {}
  struct NoLink {
    static constexpr unsigned MAX_SEND = 1;
    inline bool active() const { return false; }
    inline unsigned poll(uint32_t, uint8_t *) { return 0; }
    inline unsigned receive(uint8_t, uint8_t *) { return 0; }
  } link;
};

#endif // HALT_PIN

#endif // COCODISK_H
