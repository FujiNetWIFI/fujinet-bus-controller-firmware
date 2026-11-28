#ifndef FUJI_CALL_H
#define FUJI_CALL_H

#include <stdint.h>
#include <stdbool.h>

/* Aligns with fujinet-lib's bus/msx/fujinet-unapi-msx.h.  The ROM defines fuji_unapi_call() itself:
 * an inter-slot call can enter this ROM but cannot page its bank register,
 * so the library's CALSLT back end is useless here. */

#define FUJI_UNAPI_ID "FUJINET"

typedef struct {
  uint8_t device;
  uint8_t command;
  uint8_t aux_descr;
  uint8_t aux1, aux2, aux3, aux4;
  void *buffer;
  uint16_t length;
} FujiNetParams;

enum {
  FUJI_CALL_INFO  = 0,
  FUJI_CALL_WRITE = 1,
  FUJI_CALL_READ  = 2,
};

/* Talks to the IO window directly, not through EXTBIO. */
extern uint8_t fuji_unapi_call(uint8_t func, FujiNetParams *params);

/* Entry points published in the FN_TABLE (const.s). */
extern uint8_t __FASTCALL__ fujiF5_write(FujiNetParams *params);
extern uint8_t __FASTCALL__ fujiF5_read(FujiNetParams *params);

/* checksum.s: seed is 0, or the result for the packet's first buffer. */
extern uint8_t __CALLEE__ fuji_calc_checksum(const void *buf, uint16_t len, uint8_t seed);

/* subslot.s: io_window_enter's result goes back to io_window_exit. */
extern uint8_t io_window_enter(void);
extern void __FASTCALL__ io_window_exit(uint8_t saved);

#endif /* FUJI_CALL_H */
