; ACSI2STM Atari hard drive emulator
; Copyright (C) 2019-2024 by Jean-Matthieu Coulon

; This program is free software: you can redistribute it and/or modify
; it under the terms of the GNU General Public License as published by
; the Free Software Foundation, either version 3 of the License, or
; (at your option) any later version.

; This program is distributed in the hope that it will be useful,
; but WITHOUT ANY WARRANTY; without even the implied warranty of
; MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
; GNU General Public License for more details.

; You should have received a copy of the GNU General Public License
; along with this program.  If not, see <https://www.gnu.org/licenses/>.

; TOS loader for the GemDrive system hook

	incdir	..\inc\
	include	tos.i

	opt	O+

	text

start	bra	main                    ; Initialization is in the freed zone

	include	syshook.s

	; These variables need to be in the text segment because the main
	; entry point as well as everything after will be freed when the driver
	; will be made resident.
	dc.b	0
acsiid	dc.b	$ff                     ; Patched by device detection
prmoff	dc.w	$ffff                   ; Detected during initialization

syshook.end

	; Freed zone: everything after this is freed when the driver is made
	; resident.

main
	lea	stack,sp                ; Initialize stack

	Super	                        ; This program needs super user

	; Check if the driver is already installed

.nxtvec	move.l	gemdos.vector.w,a0
	cmp.l	#'XBRA',-12(a0)
	bne.b	.notins

	cmp.l	#'A2ST',-8(a0)
	bne.b	.na2st

	print	alrdyin
	gemdos	Cconin,2                ; Wait for a key
	Pterm0                          ;  then exit

.na2st	move.l	-4(a0),a0
	bra.b	.nxtvec

.notins	moveq	#0,d7                   ; d7 = pre-shifted ACSI id
	st	flock.w                 ; Lock floppy controller

.test	; Test for ACSI device in d7

	moveq	#0,d1                   ; Disable DMA
	bsr.w	syshook.setdmaaddr      ;
	move.w	#$0088,(a1)             ; Switch to command.
	move.w	#$0010,d0               ; Send command $10 with drive ID to STM
	add.b	d7,d0                   ;
	move.w	d0,(a0)                 ;

	moveq	#20,d1                  ; 100ms timeout

	add.l	hz200.w,d1              ;
.await	cmp.l	hz200.w,d1              ; Test timeout
	bmi.b	.nxtid                  ;
	btst.b	#5,gpip.w               ; Test command acknowledge
	bne.b	.await                  ;

	move.w	#$008a,(a1)             ; Prepare to read command/status
	move.w	(a0),d0                 ; Read command/status byte

	tst.b	d0                      ; 0 = success
	beq.b	.found                  ;

.nxtid	add.b	#$20,d7                 ; Point at next ACSI id
	bne.b	.test                   ; Try next ACSI id

	sf	flock.w                 ; Unlock floppy controller
	print	devnfnd
	gemdos	Cconin,2                ; Wait for a key
	Pterm0	                        ;  then exit

.found	sf	flock.w                 ; Unlock floppy controller

	; All good, found device in d7

setvars	
	lea	acsiid(pc),a0           ; Save ACSI id to RAM
	move.b	d7,(a0)+                ;

	move.w	#6,(a0)                 ; Compute parameter offset (prmoff)
	tst.w	_longframe.w            ; Test _longframe
	beq.b	.shrtfr                 ;
	move.w	#8,(a0)                 ;
.shrtfr

	; Install system call hook

	lea	syshook.old(pc),a0      ;
	lea	gemdos.vector.w,a3      ;
	move.l	(a3),(a0)+              ; Save old vector and point at hook code
	move.l	a0,(a3)                 ; Install hook vector

	; Enter syshook mode to run onInit on the STM32

	bsr	syshook.init

	; Shrink memory usage, terminate and stay resident

	clr.w	-(sp)                   ; TSR return code
	move.l	#$100+(syshook.end-start),-(sp) ; TSR memory size
	gemdos	Ptermres                ; Terminate and stay resident

	; Strings
alrdyin	dc.b	7,'GemDrive already installed',13,10,0
devnfnd	dc.b	7,'No GemDrive PIO device detected',13,10,0
	even

	bss

	ds.b	256                     ; Temporary stack for initialization
stack		                        ;

end

; vim: ff=dos ts=8 sw=8 sts=8 noet colorcolumn=8,41,81 ft=asm68k tw=80