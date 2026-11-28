;--- FujiNet MSX UNAPI implementation
;    Based on unapi-rom.asm by Konamiman, 5-2019 (MIT License)

        EXTERN  UNAPI_ID
        EXTERN  TOUPPER
        EXTERN  GETSLT
        EXTERN  GETWRK
        EXTERN  DO_EXTBIO
        EXTERN  UNAPI_ENTRY
        EXTERN  FN_ADD
        EXTERN  FN_MULT
        EXTERN  FN_TABLE
        EXTERN  MAX_FN

;*******************
;***  CONSTANTS  ***
;*******************

; 0: copy the old EXTBIO hook directly into our slot's SLTWRK area.
; 1: allocate 5 bytes in page 3 for it instead.

ALLOC_P3:       equ     0

;--- System variables and routines

CHPUT:  equ     00A2h
HOKVLD: equ     0FB20h
EXPTBL: equ     0FCC1h
EXTBIO: equ     0FFCAh
SLTWRK: equ     0FD09h
ARG:    equ     0F847h

;--- Bit 0 of HOKVLD vouches for three hooks, not just EXTBIO: DISINT and
;    ENAINT follow it. Whoever sets that bit has to have initialised all of
;    them -- see init.s.

DISINT: equ     0FFCFh
ENAINT: equ     0FFD4h
HOOK_LEN:       equ     5
HOOKS_LEN:      equ     ENAINT+HOOK_LEN-EXTBIO

;--- API version and implementation version

API_V_P:        equ     1
API_V_S:        equ     0
ROM_V_P:        equ     1
ROM_V_S:        equ     0

;Must be either zero (if no implementation-specific functions available), or 128 to 254
MAX_IMPFN:      equ     0


