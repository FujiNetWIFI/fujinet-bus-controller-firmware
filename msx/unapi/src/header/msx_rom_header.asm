        EXTERN INIT
        EXTERN UNAPI_ENTRY

_header_start:
        defb "AB"       ; MSX ROM Signature
        defw INIT       ; Init routine

        ;;  Pad to 16 bytes from the start of the header
        defs 16 - ($ - _header_start), 0

        ;; At 4010h: lets a client that ran before our INIT find and call it.
        defb "FUJINET",0

        ;; At 4018h: the UNAPI entry point, for a client that cannot ask
        ;; EXTBIO for it because our INIT has not run yet.
        jp UNAPI_ENTRY
