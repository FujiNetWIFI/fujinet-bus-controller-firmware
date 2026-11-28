;--- FujiNet MSX UNAPI implementation
;    Based on unapi-rom.asm by Konamiman, 5-2019 (MIT License)

        PUBLIC  UNAPI_ID, APIINFO, FN_TABLE, MAX_FN
        INCLUDE "const.inc"
        INCLUDE "fuji_call.inc"

        ;--- Specification identifier (up to 15 chars)

UNAPI_ID:
        db      "FUJINET",0

        ;--- Implementation identifier (up to 63 chars and zero terminated)

APIINFO:
        db      "ROM implementation of FUJINET UNAPI",0

;--- Standard routines addresses table; numbering matches fujinet-lib's
;    bus/msx/fujinet-unapi-msx.h.

FN_TABLE:
        dw      FN_INFO
        dw      _fujiF5_write
        dw      _fujiF5_read
MAX_FN: equ     ($ - FN_TABLE) / 2 - 1


;--- Mandatory routine 0: return API information
;    Input:  A  = 0
;    Output: HL = Descriptive string for this implementation, on this slot, zero terminated
;            DE = API version supported, D.E
;            BC = This implementation version, B.C.
;            A  = 0 and Cy = 0

FN_INFO:
        ld      bc,256*ROM_V_P+ROM_V_S
        ld      de,256*API_V_P+API_V_S
        ld      hl,APIINFO
        xor     a
        ret
