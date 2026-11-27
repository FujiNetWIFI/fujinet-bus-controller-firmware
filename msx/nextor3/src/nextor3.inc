;--- Nextor 3 kernel constants, from the Nextor 3.0 Driver Development Guide
;    and the SDK's constants files.

;--- Page 0 code, present at the head of every bank including the driver's

GSLOT1:		equ	402Dh
CALBNK:		equ	4042h
GWORK:		equ	4045h

;--- Result codes for DRIVER_QUERY and DEVICE_QUERY

RESULT_OK:		equ	0
RESULT_TRUNCATED_STRING: equ	1
RESULT_INVALID_DEVICE:	equ	2
RESULT_INIT_ERROR:	equ	3
RESULT_NOT_IMPLEMENTED:	equ	0FFh

;--- READ_WRITE error codes

E_NRDY:		equ	0FCh
E_WPROT:	equ	0F8h
E_IDEVN:	equ	0B5h

;--- MSX BIOS and system variables

RDSLT:		equ	000Ch
CALSLT:		equ	001Ch
EXPTBL:		equ	0FCC1h
