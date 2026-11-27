#include <stdio.h> // debug

#include "fuji_call.h"
#include "portio.h"
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>

#undef DEBUG
#define HEXDUMP 1

#if defined(DEBUG) && defined(HEXDUMP)
#define COLUMNS 16

static void hexdump(uint8_t *buffer, int count)
{
  int outer, inner;
  uint8_t byte;


  for (outer = 0; outer < count; outer += COLUMNS) {
    for (inner = 0; inner < COLUMNS; inner++) {
      if (inner + outer < count) {
        byte = buffer[inner + outer];
        printf("%02x ", byte);
      }
      else
        printf("   ");
    }
    printf(" |");
    for (inner = 0; inner < COLUMNS && inner + outer < count; inner++) {
      byte = buffer[inner + outer];
      if (byte >= ' ' && byte <= 0x7f)
        printf("%c", byte);
      else
        printf(".");
    }
    printf("|\n");
  }

  return;
}
#endif /* HEXDUMP */

#define milliseconds_to_jiffy(millis) ((millis) / (VDP_IS_PAL ? 20 : 1000 / 60))

#define TIMEOUT         milliseconds_to_jiffy(100)
#define TIMEOUT_SLOW    milliseconds_to_jiffy(15 * 1000)
#define DRAIN_IDLE_POLLS 64

#define false 0
#define true 1

enum {
  PACKET_ACK = 6, // ASCII ACK
  PACKET_NAK = 21, // ASCII NAK
};

enum {
  SLIP_END     = 0xC0,
  SLIP_ESCAPE  = 0xDB,
  SLIP_ESC_END = 0xDC,
  SLIP_ESC_ESC = 0xDD,
};

typedef struct {
  uint8_t device;   /* Destination Device */
  uint8_t command;  /* Command */
  uint16_t length;  /* Total length of packet including header */
  uint8_t checksum; /* Checksum of entire packet */
  uint8_t fields;   /* Describes the fields that follow */
} fujibus_header;

typedef struct {
  fujibus_header header;
  uint8_t data[4]; // max 4 aux bytes
} fujibus_packet;

static const uint8_t fuji_field_numbytes_table[] = {0, 1, 2, 3, 4, 2, 4, 4};
#define fuji_field_numbytes(descr) fuji_field_numbytes_table[descr]
#define FUJI_FIELD_AUX_MASK 0x07

static uint8_t msx_get_page_slot(uint8_t page)
{
  uint8_t slots = inp(0xA8);

  return (slots >> (page * 2)) & 0x03;
}

static void msx_set_page_slot(uint8_t page, uint8_t slot)
{
  uint8_t slots = inp(0xA8);
  uint8_t shift = page * 2;
  uint8_t mask = 0x03 << shift;

  slots = (slots & ~mask) | ((slot & 0x03) << shift);
  outp(0xA8, slots);
}

/*--- SLIP-encode a fujibus_packet over the IO window and read back the
 *    reply. `func` is FUJI_CALL_WRITE or FUJI_CALL_READ. io_window_enter/
 *    exit handle the subslot register and are no-ops if not expanded.
 */
static uint8_t fuji_packet_call(uint8_t func, fujibus_packet *packet_ptr,
                                void *pbuf, uint16_t plen)
{
  uint8_t saved_slot, my_slot;
  uint8_t saved_sub;
  uint8_t ck1, ck2;
  uint16_t rlen;
  uint8_t idle;
  bool success = false;
  uint8_t pdev = packet_ptr->header.device;
  uint8_t aux_len = fuji_field_numbytes(packet_ptr->header.fields);


  packet_ptr->header.length = sizeof(packet_ptr->header) + aux_len;
  if (func == FUJI_CALL_WRITE)
    packet_ptr->header.length += plen;

  // The checksum covers the whole header, so the field itself has to be zero
  // before it is worked out. It also holds whatever the last call left there.
  packet_ptr->header.checksum = 0;

  // Data is spread across two buffers: packet_ptr and pbuf
  ck1 = fuji_calc_checksum(packet_ptr, aux_len + sizeof(packet_ptr->header), 0);
  if (func == FUJI_CALL_WRITE)
    ck1 = fuji_calc_checksum(pbuf, plen, ck1);
  packet_ptr->header.checksum = ck1;

  /*--- Page in memory-mapped IO ---
   * Page 2's primary slot must match page 1's (our slot) -- a no-op if
   * already expanded, or what makes the window visible if not. io_window_
   * enter() then points page 2 at our own subslot, in case the client left
   * it on another one (the cartridge).
   */
  my_slot = msx_get_page_slot(1);
  saved_slot = msx_get_page_slot(2);
  msx_set_page_slot(2, my_slot);

  saved_sub = io_window_enter();    /* page 2 -> our subslot, if expanded */

  // Drop a late reply to an earlier, timed-out call. Several empty polls in a
  // row, since the adapter refills the port a byte at a time.
  for (idle = 0; idle < DRAIN_IDLE_POLLS; )
    idle = (port_getc() < 0) ? idle + 1 : 0;

  port_putc(SLIP_END);
  port_putbuf_slip(packet_ptr, aux_len + sizeof(packet_ptr->header));
  if (func == FUJI_CALL_WRITE)
    port_putbuf_slip(pbuf, plen);
  port_putc(SLIP_END);

  if (func != FUJI_CALL_READ) {
    pbuf = NULL;
    plen = 0;
  }
  rlen = port_getbuf_slip_dual(packet_ptr, sizeof(packet_ptr->header),
                               pbuf, plen, TIMEOUT_SLOW);
  if (rlen < sizeof(fujibus_header) || rlen != packet_ptr->header.length) {
#ifdef DEBUG
    printf("Reply length incorrect: %d %d\n", rlen, packet_ptr->header.length);
    hexdump((uint8_t *) packet_ptr, sizeof(fujibus_header));
#endif /* DEBUG */
    success = false;
    goto done;
  }
#ifdef DEBUG
  if (rlen - sizeof(fujibus_header) != plen) {
    printf("Expected length incorrect: %d %d\n", rlen - sizeof(fujibus_header), plen);
    hexdump((uint8_t *) packet_ptr, sizeof(*packet_ptr));
  }
#endif /* DEBUG */

  ck1 = packet_ptr->header.checksum;
  packet_ptr->header.checksum = 0;

  // Data is spread across two buffers: packet_ptr and pbuf
  ck2 = fuji_calc_checksum(packet_ptr, sizeof(packet_ptr->header), 0);
  if (func == FUJI_CALL_READ)
    ck2 = fuji_calc_checksum(pbuf, rlen - sizeof(packet_ptr->header), ck2);

  if (ck1 != ck2) {
#ifdef DEBUG
    printf("Checksum mismatch: 0x%02x 0x%02x\n", ck1, ck2);
#endif /* DEBUG */
    success = false;
    goto done;
  }

  if (packet_ptr->header.device != pdev) {
#ifdef DEBUG
    printf("Incorrect device: R:0x%02x E:0x%02x\n", packet_ptr->header.device, pdev);
    hexdump((uint8_t *) packet_ptr, sizeof(packet_ptr->header));
#endif /* DEBUG */
    success = false;
    goto done;
  }

  if (packet_ptr->header.command != PACKET_ACK) {
#ifdef DEBUG
    printf("Not ACK: 0x%02x\n", packet_ptr->header.command);
#endif /* DEBUG */
    success = false;
    goto done;
  }

  // FIXME - validate that packet_ptr->header.fields is zero?

  success = true;

 done:
  /*--- Restore: undo subslot change, then primary slot --- */
  io_window_exit(saved_sub);
  msx_set_page_slot(2, saved_slot);
  return success;
}

uint8_t fuji_unapi_call(uint8_t func, FujiNetParams *params)
{
  uint16_t idx, numbytes;
  fujibus_packet fb_packet;


  fb_packet.header.device = params->device;
  fb_packet.header.command = params->command;
  // fujinet-lib also sets its DATA and REPLY flags here; they aren't on the wire.
  fb_packet.header.fields = params->aux_descr & FUJI_FIELD_AUX_MASK;

  for (idx = 0, numbytes = fuji_field_numbytes(fb_packet.header.fields); numbytes; numbytes--, idx++)
    fb_packet.data[idx] = (uint8_t)((&params->aux1)[idx]);

  return fuji_packet_call(func, &fb_packet, params->buffer, params->length);
}

uint8_t __FASTCALL__ fujiF5_write(FujiNetParams *params)
{
  return fuji_unapi_call(FUJI_CALL_WRITE, params);
}

uint8_t __FASTCALL__ fujiF5_read(FujiNetParams *params)
{
  return fuji_unapi_call(FUJI_CALL_READ, params);
}

bool fuji_bus_call(uint8_t device, uint8_t fuji_cmd, uint8_t fields,
                   uint8_t aux1, uint8_t aux2, uint8_t aux3, uint8_t aux4,
                   const void *data, size_t data_length,
                   void *reply, size_t reply_length)
{
  FujiNetParams params;

  params.device = device;
  params.command = fuji_cmd;
  params.aux_descr = fields;
  params.aux1 = aux1;
  params.aux2 = aux2;
  params.aux3 = aux3;
  params.aux4 = aux4;

  if (reply) {
    params.buffer = reply;
    params.length = reply_length;
    return fuji_unapi_call(FUJI_CALL_READ, &params);
  }

  params.buffer = (void *)data;
  params.length = data_length;
  return fuji_unapi_call(FUJI_CALL_WRITE, &params);
}
