; ttcell.s - fast 16-colour page renderer for TELETEKS (SCC calling convention)
;
; void tt_render16a(unsigned char* top, unsigned char* bot, unsigned char* pal);
;   Draws the 40x25 cell grid (tt_ch / tt_at) into two 16-colour canvases:
;   rows 0-12 into 'top', rows 13-24 into 'bot' (canvas data, i.e. canvas+24).
;   Each glyph row is 3 pre-expanded nibble-mask bytes (ttfont3 / ttmos3);
;   output byte = fill ^ (xor & mask) with fill = bg|bg<<4, xor = (fg^bg)*0x11.
;   tt_step    = canvas bytes per line - 2   (set by C)
;   tt_rowskip = 7 * bytes per line - 120    (set by C)

.code
.export _tt_render16a
_tt_render16a:
	push bc
	push ix
	ld ix,0
	add ix,sp		; ix+6 top, ix+8 bot, ix+10 pal
	ld l,(ix+10)
	ld h,(ix+11)
	ld (ra_pal),hl
	ld l,(ix+6)
	ld h,(ix+7)
	ld (ra_dst),hl
	ld hl,_tt_ch
	ld (ra_pch),hl
	ld hl,_tt_at
	ld (ra_pat),hl
	ld a,0xFF
	ld (ra_last),a
	xor a
	ld (ra_row),a
ra_rowloop:
	ld a,(ra_row)
	cp 13
	jr nz,ra_nob
	ld l,(ix+8)
	ld h,(ix+9)
	ld (ra_dst),hl
ra_nob:
	ld a,40
	ld (ra_col),a
ra_cell:
	ld hl,(ra_pat)		; attribute
	ld a,(hl)
	inc hl
	ld (ra_pat),hl
	ld (ra_curat),a
	ld hl,ra_last
	cp (hl)
	jr z,ra_samecol
	ld (hl),a
	ld c,a
	and 7			; fg index
	ld e,a
	ld d,0
	ld hl,(ra_pal)
	add hl,de
	ld b,(hl)		; B = fg colour
	ld a,c
	rrca
	rrca
	rrca
	and 7			; bg index
	ld e,a
	ld hl,(ra_pal)
	add hl,de
	ld a,(hl)
	ld c,a			; C = bg colour
	rlca
	rlca
	rlca
	rlca
	or c
	ld (ra_fill),a
	ld a,b
	xor c
	ld c,a
	rlca
	rlca
	rlca
	rlca
	or c
	ld (ra_xor),a
ra_samecol:
	ld hl,(ra_pch)		; character
	ld a,(hl)
	inc hl
	ld (ra_pch),hl
	or a
	jr nz,ra_notblank
	ld hl,ra_zero
	jr ra_blit
ra_notblank:
	ld e,a
	ld d,0
	ld a,(ra_curat)
	bit 6,a
	jr z,ra_font
	bit 7,a
	jr z,ra_mos
	ld hl,64
	add hl,de
	ex de,hl
ra_mos:
	ld bc,_ttmos3
	jr ra_mul
ra_font:
	ld bc,_ttfont3
ra_mul:
	ld h,d			; HL = DE * 21 + BC
	ld l,e
	add hl,hl
	add hl,hl
	push hl
	add hl,hl
	add hl,hl
	add hl,de
	pop de
	add hl,de
	add hl,bc
ra_blit:
	ld de,(ra_dst)
	ld a,(ra_xor)
	ld b,a
	ld a,(ra_fill)
	ld c,a
	ld a,7
ra_r:
	push af
	ld a,(hl)
	and b
	xor c
	ld (de),a
	inc hl
	inc de
	ld a,(hl)
	and b
	xor c
	ld (de),a
	inc hl
	inc de
	ld a,(hl)
	and b
	xor c
	ld (de),a
	inc hl
	push hl
	ld hl,(_tt_step)
	add hl,de
	ex de,hl
	pop hl
	pop af
	dec a
	jr nz,ra_r
	ld hl,(ra_dst)
	inc hl
	inc hl
	inc hl
	ld (ra_dst),hl
	ld hl,ra_col
	dec (hl)
	jp nz,ra_cell
	ld hl,(ra_dst)		; next cell row
	ld de,(_tt_rowskip)
	add hl,de
	ld (ra_dst),hl
	ld hl,ra_row
	inc (hl)
	ld a,(hl)
	cp 25
	jp nz,ra_rowloop
	pop ix
	pop bc
	ret

.export _tt_step
_tt_step:
	.word 118
.export _tt_rowskip
_tt_rowskip:
	.word 720
ra_pal:
	.word 0
ra_dst:
	.word 0
ra_pch:
	.word 0
ra_pat:
	.word 0
ra_last:
	.byte 0
ra_curat:
	.byte 0
ra_row:
	.byte 0
ra_col:
	.byte 0
ra_fill:
	.byte 0
ra_xor:
	.byte 0
ra_zero:
	.ds 21
