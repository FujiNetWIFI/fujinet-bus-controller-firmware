# msx/unapi

A 16K MSX cartridge ROM that implements the FUJINET MSX-UNAPI spec over the
adapter's IO window, so programs anywhere in the machine can reach the FujiNet.

## Layout

| File | Defines |
|------|---------|
| `src/header/msx_rom_header.asm` | ROM header: `AB`, INIT, the `FUJINET` tag at 4010h and a direct entry at 4018h |
| `src/init.s` | INIT: sets up the EXTBIO hook block and installs the hook unless it is already installed |
| `src/do_extbio.s` | EXTBIO handler: answers UNAPI discovery and dispatches calls |
| `src/unapi_entry.s` | UNAPI routine dispatcher |
| `src/fuji_call.c`, `fuji_call.h`, `fuji_call.inc` | Builds FujiBus packets and exchanges them with the FujiNet |
| `src/subslot.s` | `io_window_enter`/`io_window_exit`: maps our own subslot into page 2 so the IO window is decoded |
| `src/port_*.s`, `portio.h`, `portio.inc` | Byte and SLIP-framed IO through the window |
| `src/timeout.s` | Loop-counted timeouts for the port routines |
| `src/getslt.s`, `getwrk.s`, `toupper.s` | Slot, work-area and string helpers |
| `src/const.s`, `const.inc` | UNAPI identifier, implementation name, routine table, and BIOS and work-area addresses |
| `src/main.c` | Empty `main()` that the C runtime requires |
| `pragma.inc` | z88dk pragmas: code at 4000h, no heap or stdio |

## How it fits

The msx_proto_260402 firmware serves it from subslot 1 of the cartridge, with
the IO window in page 2 of the same subslot; CONFIG or the user ROM sits in
subslot 0. With mode $1C the slot is left unexpanded and holds only this ROM
and the window. Programs reach the FujiNet through
it by EXTBIO; a client that initialises first finds it by the 4010h tag and
calls it at 4018h. Routine numbers match fujinet-lib.

## Build

`make unapi-rom` at the top level builds `build/unapi/r2r/msxrom/unapi.rom`.
It needs z88dk; MekkoGX is fetched and pinned by `Makefile`.
