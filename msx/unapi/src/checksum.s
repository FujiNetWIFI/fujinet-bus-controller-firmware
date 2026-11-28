        PUBLIC  _fuji_calc_checksum

;-----------------------------------------------------------------------------
; uint8_t fuji_calc_checksum(const void *buf, uint16_t len, uint8_t seed)
; FujiBus checksum  (__CALLEE__ calling convention)
;
; Each byte is added with the carry folded back in, so seed may be the
; result of an earlier call to checksum a packet held in two buffers.
;
; Stack on entry:
;   (SP+0) = return address
;   (SP+2) = seed
;   (SP+4) = len
;   (SP+6) = buf
;
; Returns:
;   HL = checksum (H = 0)
;
; Register usage:
;   A  = running checksum
;   B  = bytes left in the current block of 256 (0 = 256)
;   C  = blocks left
;   HL = buffer pointer
;-----------------------------------------------------------------------------

_fuji_calc_checksum:
        pop     hl                      ; HL = return address
        pop     de                      ; E = seed
        pop     bc                      ; BC = len
        ex      (sp),hl                 ; HL = buf, return address back on top

        ; Split len so djnz can count: the low byte (a partial block, if any)
        ; runs first, then whole blocks of 256.
        ld      a,b                     ; A = high byte of len
        ld      b,c                     ; B = low byte
        inc     b
        dec     b
        jr      z,cksum_whole
        inc     a                       ; count the partial block
cksum_whole:
        ld      c,a                     ; C = blocks to run
        ld      a,e                     ; A = seed
        inc     c
        dec     c
        jr      z,cksum_done            ; len is 0

cksum_loop:
        add     a,(hl)
        adc     a,0                     ; fold the carry back in
        inc     hl
        djnz    cksum_loop
        dec     c
        jr      nz,cksum_loop

cksum_done:
        ld      l,a
        ld      h,0
        ret
