# msx/nextor3

A Nextor 3.0 kernel ROM whose driver makes the FujiNet disk slots Nextor
devices: device 1 is slot 1, up to slot 6, the slots CONFIG shows.

## Layout

| File | Defines |
|------|---------|
| `src/driver.asm` | The driver bank: driver and device queries, sector reads and writes |
| `src/nextor3.inc` | Kernel, BIOS and result-code constants |
| `Makefile` | Downloads the kernel base file and puts the ROM together |

## Build

`make nextor-rom` at the top level builds `build/nextor3/nextor3.rom`, a
128K ASCII16 image. It needs z88dk. The first build downloads Konamiman's
kernel base file into `build/nextor3/base/`. The ROM is put together as
section 3.1 of the Nextor 3.0 Driver Development Guide describes, so no
`mknexrom` is needed.

## How it fits

The msx_proto_260402 firmware serves it from subslot 2 of the cartridge
when a ROM mode command sets bit 1 ($3E alone, $3F beside the user ROM).
The image is part of the firmware's data, held in RAM beside the user ROM
pool, so it never shares the pool with a user ROM. Each mode command that
enables it puts its banks back to their power-on state. With Nextor and no
user ROM, subslot 0 is left empty so CONFIG does not run again.

The driver has no transport of its own. It finds the UNAPI ROM in subslot 1
by the `FUJINET` tag at 4010h and calls the fixed entry at 4018h through
`CALSLT`. Interrupts stay off for each call, since `CALSLT` has the kernel
out of page 1. The parameter block and a 512 byte bounce buffer, for
transfers that reach into page 2, are in the page 3 work area the kernel
allocates.

## Devices

- Availability comes from the device slot table: a slot with nothing in it,
  or holding a ROM, is offline.
- A slot that is empty or holds a ROM is flagged as not for automapping,
  so it gets no drive at boot, the disks around it keep consecutive
  letters, and the kernel's partition search doesn't read it.
- A media change is a change in a slot's table entry. Remounting the same
  image is not seen as one.
- One fetch of the table answers for all six devices for about 2 seconds
  (120 frames), so a change of image can take that long to be seen. A
  failed read or write fetches it again on the next query.
- An image whose PERCOM geometry fits a floppy (up to 84 tracks, 2 sides
  and 36 sectors per track of 512 bytes) is a floppy: it reports that
  geometry and its size, and Nextor maps a drive to sector 0 with no
  partition search. Anything else is a hard disk and reports no size, since
  the PERCOM block gives none past 255 tracks.
- FORMAT works on a 360K or 720K floppy image. It writes an MSX-DOS 1 boot
  sector, empty FATs and an empty root directory, and leaves the data area
  alone; in DOS 2 mode the kernel then writes its own boot sector. Other
  floppy sizes can't be formatted.
- A failed read is `.NRDY`, a failed write `.WPROT`: a NAK doesn't say which.
