; FujiNet driver for the Nextor 3 kernel
;
;   Each FujiNet disk slot is one Nextor device: device 1 is slot 1, and so
;   on up to slot 6. The kernel maps drives onto them itself.
;
;   The driver has no transport of its own. It reaches the FujiNet through
;   the UNAPI ROM: it finds that ROM by the FUJINET tag at 4010h and calls
;   the fixed entry at 4018h, so it works whichever of the two initialises
;   first.

	INCLUDE	"nextor3.inc"

;--- FujiNet devices and commands

FUJI_DEVICEID_DISK:	equ	31h	; first of the disk slots
FUJI_DEVICEID_FUJINET:	equ	70h	; the control device
FUJICMD_READ:		equ	'R'
FUJICMD_WRITE:		equ	'W'
FUJICMD_READ_DEVICE_SLOTS: equ	0F2h
FUJI_FIELD_C1234:	equ	7	; one 32 bit value

DEV_COUNT:		equ	6
SLOT_ENTRY_LEN:		equ	38	; host slot, mode, file name
SLOTS_REPLY:		equ	SLOT_ENTRY_LEN*8	; the FujiNet sends all eight

SECTOR_SIZE:		equ	512

;--- The slot table is fetched at most this often: the kernel asks after a
;    device before most accesses, and the table is the same for all of them.

CACHE_JIFFIES:		equ	120	; about 2 seconds
CACHE_ROM:		equ	2	; cache entry for a ROM; hashes are odd

;--- The UNAPI ROM

UNAPI_ID_ADDR:		equ	4010h
UNAPI_ENTRY_ADDR:	equ	4018h
UNAPI_WRITE:		equ	1
UNAPI_READ:		equ	2

;--- Work area, in page 3: the UNAPI call pages over page 1 and page 2, so
;    the parameter block and any buffer it points at must be out of both.

W_PARAMS:	equ	0	; FujiNetParams, 11 bytes
P_DEVICE:	equ	W_PARAMS+0
P_COMMAND:	equ	W_PARAMS+1
P_DESCR:	equ	W_PARAMS+2
P_AUX:		equ	W_PARAMS+3
P_BUFFER:	equ	W_PARAMS+7
P_LENGTH:	equ	W_PARAMS+9
W_SLOT:		equ	11	; slot of the UNAPI ROM
W_ADDR:		equ	12	; word: caller's transfer address
W_COUNT:	equ	14
W_DONE:		equ	15
W_WRITE:	equ	16
W_HASH:		equ	17	; DEV_COUNT: slot entries last reported
W_CACHE:	equ	W_HASH+DEV_COUNT	; DEV_COUNT: entries, as SE_PARSE gives them
W_CACHE_TIME:	equ	W_CACHE+DEV_COUNT	; word: JIFFY when the cache was filled
W_CACHED:	equ	W_CACHE_TIME+2	; nonzero while the cache holds a table
W_BUFFER:	equ	W_CACHED+1
WRK_SIZE:	equ	W_BUFFER+SECTOR_SIZE

VER_MAIN:	equ	1
VER_SEC:	equ	0
VER_REV:	equ	0

	org	4100h

	defb	"NEXTORv3_DRIVER",0

	jp	TIMER_INT
	jp	OEMSTAT
	jp	BASDEV
	jp	EXTBIO
	jp	DRIVER_QUERY
	jp	DEVICE_QUERY
	jp	CUSTOM_DRIVER_QUERY
	jp	CUSTOM_DEVICE_QUERY
	jp	READ_WRITE
	defs	3*3,0C9h	; RESERVED_0-2
	defs	5*3,0C9h	; DIRECT_0-4

TIMER_INT:
EXTBIO:
	ret

OEMSTAT:
BASDEV:
	scf
	ret

CUSTOM_DRIVER_QUERY:
CUSTOM_DEVICE_QUERY:
NOT_IMPL:
	ld	a,RESULT_NOT_IMPLEMENTED
	ret


;-----------------------------------------------------------------------------
;
; Driver queries

DRIVER_QUERY:
	dec	a
	jr	z,DQ_VERSION
	dec	a
	jr	z,DQ_STRING
	dec	a
	jr	z,DQ_PARAMS
	dec	a
	jr	z,DQ_INIT
	dec	a
	jr	nz,NOT_IMPL

	ld	b,DEV_COUNT	; 5: maximum device number
	xor	a
	ret

DQ_VERSION:
	ld	bc,256*VER_MAIN+VER_SEC
	ld	d,VER_REV
	xor	a
	ret

;--- 2: driver name only

DQ_STRING:
	djnz	NOT_IMPL
	ld	b,d
	ld	de,STR_DRIVER
	jp	OUT_STR

;--- 3: the work area, and no hooks

DQ_PARAMS:
	ld	hl,WRK_SIZE
	ld	b,0
	xor	a
	ret

;--- 4: clear the work area and find the UNAPI ROM

DQ_INIT:
	push	de
	call	GET_WORK
	push	iy
	pop	hl
	ld	d,h
	ld	e,l
	inc	de
	ld	(hl),0
	ld	bc,WRK_SIZE-1
	ldir

	call	FIND_UNAPI
	pop	de
	jr	c,DQ_INIT_FAIL
	ld	(iy+W_SLOT),a
	xor	a
	ret

DQ_INIT_FAIL:
	ld	hl,STR_NO_UNAPI
	call	PRINT_CB
	ld	a,RESULT_INIT_ERROR
	ret

;--- Print a string with the kernel's character callback
;    In: HL = zero terminated string, DE = callback

PRINT_CB:
	ld	a,(hl)
	or	a
	ret	z
	push	hl
	push	de
	ld	hl,PRINT_CB_RET
	push	hl		; return address for the callback
	push	de
	ret			; "call (de)"
PRINT_CB_RET:
	pop	de
	pop	hl
	inc	hl
	jr	PRINT_CB

;--- Find a slot carrying the UNAPI ROM's "FUJINET" tag at 4010h
;    Out: Cy = 0 and A = slot, or Cy = 1 if there is none

FIND_UNAPI:
	ld	a,i
	push	af		; P/V = interrupt state: RDSLT leaves them off
	ld	c,0		; primary slot
FU_PRIMARY:
	ld	hl,EXPTBL
	ld	b,0
	add	hl,bc
	ld	a,(hl)
	and	80h
	or	c
	ld	b,a		; B = slot, subslot 0 if expanded
FU_SUB:
	push	bc
	ld	a,b
	call	CHECK_TAG
	pop	bc
	jr	z,FU_FOUND
	bit	7,b
	jr	z,FU_NEXT
	ld	a,b
	add	a,4
	ld	b,a
	and	0Ch
	jr	nz,FU_SUB
FU_NEXT:
	inc	c
	ld	a,c
	cp	4
	jr	c,FU_PRIMARY
	pop	af
	call	RESTORE_EI
	scf
	ret

FU_FOUND:
	pop	af
	call	RESTORE_EI
	ld	a,b
	or	a
	ret

RESTORE_EI:
	ret	po
	ei
	ret

;--- Z if the slot in A has the tag. Corrupts everything but IY.

CHECK_TAG:
	ld	e,a
	ld	hl,UNAPI_ID_ADDR
	ld	ix,STR_TAG
CT_LOOP:
	push	de
	push	hl
	ld	a,e
	call	RDSLT
	pop	hl
	pop	de
	cp	(ix)
	ret	nz
	or	a
	ret	z		; the terminator matched too
	inc	hl
	inc	ix
	jr	CT_LOOP


;-----------------------------------------------------------------------------
;
; Device queries

DEVICE_QUERY:
	ld	e,a
	call	CHECK_DEV
	ld	a,RESULT_INVALID_DEVICE
	ret	c
	ld	a,e
	dec	a
	jr	z,DV_STRING
	dec	a
	jr	z,DV_PARAMS
	dec	a
	jr	z,DV_STATUS
	dec	a
	jr	z,DV_AVAIL
	jp	NOT_IMPL

;--- 1: information strings, manufacturer and device name

DV_STRING:
	ld	a,b
	ld	b,d		; B = buffer size
	dec	a
	jr	z,DV_MANUF
	cp	3
	jp	nz,NOT_IMPL

	;--- "FujiNet disk slot N", built in the work area since the slot
	;    number has to go on the end

	push	hl
	push	bc
	call	GET_WORK
	call	BUFFER_ADDR
	ex	de,hl
	ld	hl,STR_DEVICE
	ld	bc,STR_DEVICE_LEN
	ldir
	pop	bc
	dec	de
	dec	de
	ld	a,c
	add	a,'0'
	ld	(de),a
	call	BUFFER_ADDR
	ex	de,hl
	pop	hl
	jp	OUT_STR

DV_MANUF:
	ld	de,STR_MANUF
	jp	OUT_STR

;--- 2: device parameters. The image size isn't something FujiNet reports.

DV_PARAMS:
	ld	a,h
	or	l
	ret	z		; A = RESULT_OK: just checking the device

	;--- A slot that is empty or holds a ROM is kept out of the partition
	;    search, so it gets no drive at boot, the disks after it keep
	;    consecutive letters, and searches don't read it. The flags follow the
	;    slot, so a disk mounted there later is picked up.

	push	hl
	call	GET_WORK
	call	SLOT_ENTRY
	pop	hl
	ld	e,01h		; removable
	jr	nc,DV_PARAMS_FILL
	ld	e,09h		; and not for automapping
DV_PARAMS_FILL:
	ld	(hl),0		; block device
	inc	hl
	ld	(hl),SECTOR_SIZE & 0FFh
	inc	hl
	ld	(hl),SECTOR_SIZE / 256
	inc	hl
	ld	b,4
DV_PARAMS_SIZE:
	ld	(hl),0
	inc	hl
	djnz	DV_PARAMS_SIZE
	ld	(hl),e
	inc	hl
	ld	b,4
DV_PARAMS_GEOM:
	ld	(hl),0
	inc	hl
	djnz	DV_PARAMS_GEOM
	xor	a
	ret

;--- 4: availability: whether a disk image is mounted in the slot

DV_AVAIL:
	call	GET_WORK
	call	SLOT_ENTRY
	ld	b,0
	jr	c,DV_STATUS_OUT
	inc	b
	jr	DV_STATUS_OUT

;--- 3: status, relative to the last call of this query. Nothing tells one
;    mount of an image from the next, so a change is a change in the slot's
;    entry: host slot, mode or file name.

DV_STATUS:
	call	GET_WORK
	call	SLOT_ENTRY
	ld	b,0
	push	af
	push	iy
	pop	hl
	ld	de,W_HASH-1
	add	hl,de
	ld	e,c
	ld	d,0
	add	hl,de		; HL = this device's hash on record
	pop	af
	jr	c,DV_STATUS_EMPTY
	inc	b
	cp	(hl)
	jr	z,DV_STATUS_OUT	; 1: unchanged
	ld	(hl),a
	inc	b		; 2: changed
	jr	DV_STATUS_OUT

DV_STATUS_EMPTY:
	ld	(hl),b		; no hash is zero: whatever comes next is a change

DV_STATUS_OUT:
	xor	a
	ret

;--- A slot's entry in the FujiNet's device slot table
;    In:  C = device number, IY = work area
;    Out: Cy = 1 if the slot is empty, holds a ROM (A = 1), or the FujiNet
;         did not answer (A = 0); else A = a hash of the entry, never zero.
;         C is preserved.
;
;    Answered from the cache while it is under CACHE_JIFFIES old, so a
;    change of image shows up that much later at most.

SLOT_ENTRY:
	ld	a,(iy+W_CACHED)
	or	a
	jr	z,SE_REFRESH
	push	bc
	ld	hl,(JIFFY)
	ld	e,(iy+W_CACHE_TIME)
	ld	d,(iy+W_CACHE_TIME+1)
	or	a
	sbc	hl,de		; HL = age of the cache
	ld	de,CACHE_JIFFIES
	or	a
	sbc	hl,de
	pop	bc
	jr	c,SE_CACHED

SE_REFRESH:
	push	bc
	ld	(iy+P_DEVICE),FUJI_DEVICEID_FUJINET
	ld	(iy+P_COMMAND),FUJICMD_READ_DEVICE_SLOTS
	ld	(iy+P_DESCR),0
	call	BUFFER_ADDR
	ld	(iy+P_BUFFER),l
	ld	(iy+P_BUFFER+1),h
	ld	(iy+P_LENGTH),SLOTS_REPLY & 0FFh
	ld	(iy+P_LENGTH+1),SLOTS_REPLY / 256
	ld	a,UNAPI_READ
	call	FUJI_CALL
	jr	nc,SE_FILL
	ld	(iy+W_CACHED),0
	pop	bc
	xor	a
	scf
	ret

SE_FILL:
	ld	c,DEV_COUNT
SE_FILL_LOOP:
	call	SE_PARSE
	call	CACHE_ADDR
	ld	(hl),a
	dec	c
	jr	nz,SE_FILL_LOOP
	ld	hl,(JIFFY)
	ld	(iy+W_CACHE_TIME),l
	ld	(iy+W_CACHE_TIME+1),h
	ld	(iy+W_CACHED),1
	pop	bc

SE_CACHED:
	call	CACHE_ADDR
	ld	a,(hl)
	or	a
	scf
	ret	z		; empty
	cp	CACHE_ROM
	jr	z,SE_IS_ROM
	or	a		; a disk: Cy = 0, A = the hash
	ret
SE_IS_ROM:
	ld	a,1
	scf
	ret

;--- HL = device C's cache entry

CACHE_ADDR:
	push	de
	push	iy
	pop	hl
	ld	de,W_CACHE-1
	add	hl,de
	ld	e,c
	ld	d,0
	add	hl,de
	pop	de
	ret

;--- Device C's entry in the table in the work buffer
;    Out: A = 0 if the slot is empty, CACHE_ROM if it holds a ROM, else a
;         hash of the entry (odd). C is preserved.
;
;    An entry is the host slot, the mode and the file name. An empty slot
;    has host slot FFh, or keeps a host slot with no file name. A ROM is in
;    a slot too, but has no sectors to read.

SE_PARSE:
	call	BUFFER_ADDR
	ld	de,SLOT_ENTRY_LEN
	ld	b,c
	jr	SE_FIND
SE_NEXT:
	add	hl,de
SE_FIND:
	djnz	SE_NEXT

	ld	a,(hl)
	inc	a
	ret	z		; host slot FFh: empty, A = 0
	inc	hl
	inc	hl
	ld	a,(hl)
	dec	hl
	dec	hl
	or	a
	ret	z		; no file name: empty too, A = 0

	push	hl
	ld	b,SLOT_ENTRY_LEN
	xor	a
SE_HASH:
	rlca
	add	a,(hl)
	inc	hl
	djnz	SE_HASH
	or	1
	pop	hl
	ld	e,a

	inc	hl
	inc	hl		; the file name
	ld	b,SLOT_ENTRY_LEN-2
SE_DOT:
	ld	a,(hl)
	inc	hl
	or	a
	jr	z,SE_DISK
	cp	'.'
	call	z,IS_ROM_EXT
	jr	z,SE_ROM
	djnz	SE_DOT
SE_DISK:
	ld	a,e
	ret

SE_ROM:
	ld	a,CACHE_ROM
	ret

;--- Z if HL points at "rom" (any case) and the end of the name

IS_ROM_EXT:
	push	hl
	push	de
	ld	de,STR_ROM_EXT
IR_LOOP:
	ld	a,(de)
	or	a
	jr	z,IR_END
	ld	a,(hl)
	or	20h
	ex	de,hl
	cp	(hl)
	ex	de,hl
	jr	nz,IR_OUT
	inc	hl
	inc	de
	jr	IR_LOOP
IR_END:
	cp	(hl)
IR_OUT:
	pop	de
	pop	hl
	ret


;-----------------------------------------------------------------------------
;
; READ_WRITE
;    In:  Cy = 0 read / 1 write, A = device, B = sectors,
;         HL = transfer address, DE = address of the 4 byte sector number
;    Out: A = error code, B = sectors transferred
;
;    Neither address is in page 1, but a transfer may run into page 2, which
;    the UNAPI call pages over: such sectors bounce through the work area.

READ_WRITE:
	ld	c,a
	call	GET_WORK
	ld	(iy+W_WRITE),0
	jr	nc,RW_DIR
	inc	(iy+W_WRITE)
RW_DIR:
	ld	(iy+W_COUNT),b
	ld	(iy+W_DONE),0
	ld	(iy+W_ADDR),l
	ld	(iy+W_ADDR+1),h

	call	CHECK_DEV
	ld	a,E_IDEVN
	jr	c,RW_ERR
	ld	a,c
	add	a,FUJI_DEVICEID_DISK-1
	ld	(iy+P_DEVICE),a
	ld	(iy+P_DESCR),FUJI_FIELD_C1234
	ld	(iy+P_LENGTH),SECTOR_SIZE & 0FFh
	ld	(iy+P_LENGTH+1),SECTOR_SIZE / 256
	ld	a,FUJICMD_READ
	bit	0,(iy+W_WRITE)
	jr	z,RW_CMD
	ld	a,FUJICMD_WRITE
RW_CMD:
	ld	(iy+P_COMMAND),a

	push	iy
	pop	hl
	ld	bc,P_AUX
	add	hl,bc
	ex	de,hl
	ld	bc,4
	ldir

RW_LOOP:
	ld	a,(iy+W_COUNT)
	or	a
	jr	z,RW_OK

	call	RW_SECTOR
	jr	c,RW_FAILED

	inc	(iy+W_DONE)
	dec	(iy+W_COUNT)
	ld	l,(iy+W_ADDR)
	ld	h,(iy+W_ADDR+1)
	ld	bc,SECTOR_SIZE
	add	hl,bc
	ld	(iy+W_ADDR),l
	ld	(iy+W_ADDR+1),h

	inc	(iy+P_AUX)
	jr	nz,RW_LOOP
	inc	(iy+P_AUX+1)
	jr	nz,RW_LOOP
	inc	(iy+P_AUX+2)
	jr	nz,RW_LOOP
	inc	(iy+P_AUX+3)
	jr	RW_LOOP

RW_OK:
	xor	a
	jr	RW_ERR

;--- An empty slot NAKs, as does one mounted read only refusing a write;
;    nothing in the reply tells either from a FujiNet that did not answer.

RW_FAILED:
	ld	(iy+W_CACHED),0	; the slot may have changed: look again next time
	ld	a,E_NRDY
	bit	0,(iy+W_WRITE)
	jr	z,RW_ERR
	ld	a,E_WPROT

RW_ERR:
	ld	b,(iy+W_DONE)
	ret

;--- One sector, at W_ADDR
;    Out: Cy = 1 on failure

RW_SECTOR:
	ld	a,(iy+W_ADDR+1)
	cp	3Eh
	jr	c,RW_DIRECT	; ends below page 1
	cp	0C0h
	jr	nc,RW_DIRECT	; page 3

	call	BUFFER_ADDR
	ld	(iy+P_BUFFER),l
	ld	(iy+P_BUFFER+1),h
	bit	0,(iy+W_WRITE)
	jr	z,RW_BOUNCE_READ

	ex	de,hl
	ld	l,(iy+W_ADDR)
	ld	h,(iy+W_ADDR+1)
	ld	bc,SECTOR_SIZE
	ldir
	ld	a,UNAPI_WRITE
	jr	FUJI_CALL

RW_BOUNCE_READ:
	ld	a,UNAPI_READ
	call	FUJI_CALL
	ret	c
	call	BUFFER_ADDR
	ld	e,(iy+W_ADDR)
	ld	d,(iy+W_ADDR+1)
	ld	bc,SECTOR_SIZE
	ldir
	or	a
	ret

RW_DIRECT:
	ld	a,(iy+W_ADDR)
	ld	(iy+P_BUFFER),a
	ld	a,(iy+W_ADDR+1)
	ld	(iy+P_BUFFER+1),a
	ld	a,UNAPI_READ
	bit	0,(iy+W_WRITE)
	jr	z,FUJI_CALL
	ld	a,UNAPI_WRITE
	; fall through


;-----------------------------------------------------------------------------
;
; Helpers

;--- Send the parameter block in the work area through the UNAPI ROM
;    In:  A = UNAPI_READ or UNAPI_WRITE, IY = work area
;    Out: Cy = 0 if the FujiNet answered. IY is preserved.
;
;    Interrupts stay off for the call: CALSLT has this bank out of page 1,
;    and an interrupt taken then runs the kernel's hooks without it.

FUJI_CALL:
	ld	e,a
	ld	a,i
	push	af
	push	iy
	push	iy
	pop	hl
	ld	bc,W_PARAMS
	add	hl,bc
	ld	a,(iy+W_SLOT)
	push	af
	pop	iy		; IYh = slot
	ld	ix,UNAPI_ENTRY_ADDR
	ld	a,e
	di
	call	CALSLT
	di
	pop	iy
	pop	af
	call	RESTORE_EI
	ld	a,l
	cp	1		; 1 if the device answered
	ret	z
	scf
	ret

;--- Is C a device number this driver has?
;    Out: Cy = 1 if not

CHECK_DEV:
	ld	a,c
	or	a
	scf
	ret	z
	cp	DEV_COUNT+1
	ccf
	ret

;--- IY = the work area. Corrupts IX and AF'.

GET_WORK:
	push	af
	push	bc
	push	de
	push	hl
	xor	a
	ex	af,af'
	xor	a
	ld	ix,GWORK
	call	CALBNK
	ld	l,(ix)
	ld	h,(ix+1)
	push	hl
	pop	iy
	pop	hl
	pop	de
	pop	bc
	pop	af
	ret

;--- HL = the work buffer

BUFFER_ADDR:
	push	bc
	push	iy
	pop	hl
	ld	bc,W_BUFFER
	add	hl,bc
	pop	bc
	ret

;--- Copy a zero terminated string to a caller's buffer of limited size
;    In:  DE = string, HL = buffer, B = buffer size
;    Out: A = RESULT_OK, or RESULT_TRUNCATED_STRING if it did not fit

OUT_STR:
	ld	a,b
	or	a
	ld	a,RESULT_TRUNCATED_STRING
	ret	z
OS_LOOP:
	ld	a,(de)
	ld	(hl),a
	or	a
	ret	z		; A = RESULT_OK
	inc	hl
	inc	de
	djnz	OS_LOOP
	dec	hl
	ld	(hl),0
	ld	a,RESULT_TRUNCATED_STRING
	ret

STR_TAG:
	defb	"FUJINET",0
STR_ROM_EXT:
	defb	"rom",0
STR_DRIVER:
	defb	"FujiNet",0
STR_MANUF:
	defb	"FujiNet",0
STR_NO_UNAPI:
	defb	"FujiNet UNAPI ROM not found",13,10,0
STR_DEVICE:
	defb	"FujiNet disk slot ",0,0
STR_DEVICE_LEN:	equ	$-STR_DEVICE
