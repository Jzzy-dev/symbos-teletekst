; ttfast.s - fast scanner for the "content" string of a NOS Teletekst page
; (TELETEKS for SymbOS, SCC calling convention)
;
; unsigned short tt_fastrun(char* buf, unsigned short len, unsigned char mode);
;   mode 1 = HTML text, mode 2 = inside an HTML tag.
;   Consumes only "simple" input and returns the number of bytes consumed:
;     - bytes whose class (cls[c]) matches mode
;     - \" and \/ escapes
;     - \uF0xx block graphics (xx = 20-3F, 60-7F), text mode only
;   Everything else (and escapes split across buffers) is left to the C
;   state machine in ttcore.c, which handles exactly the same cases the same way.

.code
.export _tt_fastrun
_tt_fastrun:
	push bc
	ld hl,4
	add hl,sp
	ld e,(hl)
	inc hl
	ld d,(hl)		; DE = buf
	inc hl
	ld c,(hl)
	inc hl
	ld b,(hl)		; BC = len
	inc hl
	ld a,(hl)
	ld (fr_mode),a
	ld (fr_start),de
	ex de,hl		; HL = buf
fr_loop:
	ld a,b
	or c
	jp z,fr_done
	ld a,(hl)
	push hl
	ld e,a
	ld d,0
	ld hl,_cls
	add hl,de
	ld a,(fr_mode)
	and (hl)
	pop hl
	jr z,fr_special
	ld a,(fr_mode)
	cp 1
	ld a,(hl)
	jr nz,fr_tagbyte
	sub 0x20		; glyph index
	ld e,0
	call fr_put
	inc hl
	dec bc
	jr fr_loop
fr_tagbyte:
	call fr_buf
	inc hl
	dec bc
	jr fr_loop

fr_special:
	ld a,(hl)
	cp 0x5C			; backslash
	jp nz,fr_done
	ld a,b			; need 2 bytes
	or a
	jr nz,fr_have2
	ld a,c
	cp 2
	jp c,fr_done
fr_have2:
	inc hl
	ld a,(hl)
	dec hl
	cp 0x22			; \"
	jr z,fr_lit
	cp 0x2F			; \/
	jr z,fr_lit
	cp 0x75			; \u
	jp nz,fr_done
	ld a,(fr_mode)
	cp 1
	jp nz,fr_done
	ld a,b			; need 6 bytes
	or a
	jr nz,fr_have6
	ld a,c
	cp 6
	jp c,fr_done
fr_have6:
	push hl
	inc hl
	inc hl
	ld a,(hl)
	or 0x20
	cp 0x66			; 'f'
	jr nz,fr_popdone
	inc hl
	ld a,(hl)
	cp 0x30			; '0'
	jr nz,fr_popdone
	inc hl
	ld a,(hl)
	call fr_hex
	jr c,fr_popdone
	ld d,a			; high nibble
	and 0xFA
	cp 2			; must be 2,3,6,7
	jr nz,fr_popdone
	inc hl
	ld a,(hl)
	call fr_hex
	jr c,fr_popdone
	ld e,a			; low nibble
	; bits = E | (D&1)<<4 | (D&4 ? 0x20 : 0)
	ld a,d
	and 1
	jr z,fr_m1
	ld a,0x10
fr_m1:
	or e
	bit 2,d
	jr z,fr_m2
	or 0x20
fr_m2:
	pop hl
	ld e,0x40		; AT_MOSAIC
	call fr_put
	ld de,6
	add hl,de
	ld a,c			; BC -= 6
	sub 6
	ld c,a
	jr nc,fr_m3
	dec b
fr_m3:
	jp fr_loop

fr_lit:				; \" or \/ : literal character
	ld e,a
	ld a,(fr_mode)
	cp 1
	ld a,e
	jr nz,fr_littag
	sub 0x20
	ld e,0
	call fr_put
	jr fr_litnext
fr_littag:
	call fr_buf
fr_litnext:
	inc hl
	inc hl
	dec bc
	dec bc
	jp fr_loop

fr_popdone:
	pop hl
fr_done:
	ld de,(fr_start)
	or a
	sbc hl,de		; HL = bytes consumed
	pop bc
	ret

; store cell: A = char/bits, E = extra attribute bits; keeps HL, BC
fr_put:
	push hl
	push bc
	ld d,a
	ld a,(_h_room)
	or a
	jr z,fr_putskip
	dec a
	ld (_h_room),a
	ld bc,(_h_idx)
	ld hl,_tt_ch
	add hl,bc
	ld (hl),d
	ld hl,_tt_at
	add hl,bc
	ld a,(_h_attr)
	or e
	ld (hl),a
	ld hl,_tt_lk
	add hl,bc
	ld a,(_h_link)
	ld (hl),a
	inc bc
	ld (_h_idx),bc
fr_putskip:
	ld hl,_h_col
	inc (hl)
	pop bc
	pop hl
	ret

; append A to the tag buffer (max 99 chars); keeps HL, BC
fr_buf:
	push hl
	ld e,a
	ld a,(_h_len)
	cp 99
	jr nc,fr_bufskip
	ld hl,_h_buf
	add a,l
	ld l,a
	jr nc,fr_bufnc
	inc h
fr_bufnc:
	ld (hl),e
	ld hl,_h_len
	inc (hl)
fr_bufskip:
	pop hl
	ret

; A = ASCII hex digit -> A = value, carry set if invalid
fr_hex:
	cp 0x30
	jr c,fr_hbad
	cp 0x3A
	jr c,fr_hdig
	or 0x20
	cp 0x61
	jr c,fr_hbad
	cp 0x67
	jr nc,fr_hbad
	sub 0x57
	or a
	ret
fr_hdig:
	sub 0x30
	or a
	ret
fr_hbad:
	scf
	ret

fr_mode:
	.byte 0
fr_start:
	.word 0
