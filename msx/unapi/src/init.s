;--- FujiNet MSX UNAPI implementation
;    Based on unapi-rom.asm by Konamiman, 5-2019 (MIT License)

        INCLUDE "const.inc"
        PUBLIC  INIT

INIT:
        ;--- Initialize the hooks if necessary. Bit 0 of HOKVLD covers the
        ;    whole EXTBIO/DISINT/ENAINT block, so all three are filled with
        ;    RET; DISINT or ENAINT left as stale RAM crashes the machine.

        ld      a,(HOKVLD)
        bit     0,a
        jr      nz,CHK_INSTALLED

        ld      hl,EXTBIO
        ld      de,EXTBIO+1
        ld      bc,HOOKS_LEN-1
        ld      (hl),0C9h  ;code for RET
        ldir

        or      1
        ld      (HOKVLD),a
        jr      OK_INIEXTB

        ;--- A client may already have run us (see fujinet-lib's discovery).
        ;    Hooking twice would chain the hook to itself.

CHK_INSTALLED:
        ld      hl,UNAPI_ID
        ld      de,ARG
        ld      bc,8
        ldir
        ld      de,2222h
        xor     a
        ld      b,a
        call    EXTBIO
        ld      a,b
        or      a
        ret     nz
OK_INIEXTB:

        ;--- Save previous EXTBIO hook

        if      ALLOC_P3

        ld      hl,HOOK_LEN
        call    ALLOC
        push    hl
        call    GETSLT
        call    GETWRK
        pop     de
        ld      (hl),e
        inc     hl
        ld      (hl),d

        else

        call    GETSLT
        call    GETWRK
        ex      de,hl

        endif

        ld      hl,EXTBIO
        ld      bc,HOOK_LEN
        ldir

        ;--- Patch EXTBIO hook

        di
        ld      a,0F7h  ;code for "RST 30h"
        ld      (EXTBIO),a
        call    GETSLT
        ld      (EXTBIO+1),a
        ld      hl,DO_EXTBIO
        ld      (EXTBIO+2),hl
        ld      a,0C9h
        ld      (EXTBIO+4),a
        ei

        ;--- No banner: it would flash up before whatever boots next.

        ret
