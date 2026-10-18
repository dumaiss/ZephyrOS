; DonkeyKongKos native engine.
;
; OS-7's software engines are retained in their original 015Dh-1FFFh
; addresses.  Every device-facing entry is replaced here.  The cartridge is
; mapped at 8000h-BFFFh by KonaOS, while its original 7000h workspace remains
; ordinary process RAM.

	.module dkkos_native

	.globl _dk_install_hooks
	.globl _dk_video_init
	.globl _dk_video_shutdown
	.globl _dk_run_game
	.globl _dk_frame
	.globl _dk_start_clock
	.globl _dk_input_poll
	.globl _dk_in_event

VDP_DATA        = 0xa0
VDP_CTRL        = 0xa1
VDP_PALETTE     = 0xa2
VDP_INDIRECT    = 0xa3
PSG_PORT        = 0xe0

BIOS_R0         = 0x63c3
BIOS_R1         = 0x63c4
BIOS_RANDOM     = 0x63c8
BIOS_TABLES     = 0x63f2
LOCAL_SPR_PTR   = 0x8002

NAME_BASE       = 0x1800
NAME_END        = 0x1b00
SPR_PATTERN     = 0x3800

NATIVE_COLOR    = 0xf000       ; page 1 => physical 1f000h
NATIVE_SAT      = 0xf200       ; page 1 => physical 1f200h
NATIVE_PATTERN  = 0xf800       ; page 1 => physical 1f800h

	.area _CODE

; -------------------------------------------------------------------------
; Install JP hooks in the copied BIOS and initialize the state OS-7's boot
; path would normally establish before entering a cartridge.
; -------------------------------------------------------------------------
_dk_install_hooks::
	; BIOS global constants below the SDK's fixed low-data window.
	ld      a,#60
	ld      (0x0069),a
	ld      hl,#0x1623
	ld      (0x006c),hl

	; Clear private OS-7 variables without touching DK's 7000h workspace.
	xor     a
	ld      hl,#0x6300
	ld      (hl),a
	ld      de,#0x6301
	ld      bc,#0x0100
	ldir
	ld      hl,#0x0033
	ld      (BIOS_RANDOM),hl
	ld      hl,#native_table_bases
	ld      de,#BIOS_TABLES
	ld      bc,#10
	ldir

	; Direct entries used by OS-7 internals.
	ld      hl,#native_poller
	ld      de,#0x11c1
	call    install_jp
	ld      hl,#native_fill_vram
	ld      de,#0x18d4
	call    install_jp
	ld      hl,#native_game_opt
	ld      de,#0x1979
	call    install_jp
	ld      hl,#native_vram_read
	ld      de,#0x1ba3
	call    install_jp
	ld      hl,#native_write_sprites
	ld      de,#0x1c82
	call    install_jp
	ld      hl,#native_reg_write
	ld      de,#0x1cca
	call    install_jp
	ld      hl,#native_vram_write
	ld      de,#0x1d01
	call    install_jp
	ld      hl,#native_vram_read
	ld      de,#0x1d3e
	call    install_jp
	ld      hl,#native_reg_read
	ld      de,#0x1d57
	call    install_jp

	; Public jump-table entries.  Hook them directly as a guard against a
	; different-but-similar BIOS layout accidentally reaching old hardware.
	ld      hl,#native_game_opt
	ld      de,#0x1f7c
	call    install_jp
	ld      hl,#native_fill_vram
	ld      de,#0x1f82
	call    install_jp
	ld      hl,#native_write_sprites
	ld      de,#0x1fc4
	call    install_jp
	ld      hl,#native_reg_write
	ld      de,#0x1fd9
	call    install_jp
	ld      hl,#native_reg_read
	ld      de,#0x1fdc
	call    install_jp
	ld      hl,#native_vram_write
	ld      de,#0x1fdf
	call    install_jp
	ld      hl,#native_vram_read
	ld      de,#0x1fe2
	call    install_jp
	ld      hl,#native_poller
	ld      de,#0x1feb
	call    install_jp
	ld      hl,#native_rand
	ld      de,#0x1ffd
	call    install_jp

	; Standard OS-7 SN76489 port operands.  Zephyr-80 exposes chip zero at
	; E0h.  The software sequencer itself remains the original assembly.
	ld      a,#PSG_PORT
	ld      (0x0173),a
	ld      (0x017c),a
	ld      (0x018e),a
	ld      (0x023e),a
	ld      (0x0242),a
	ld      (0x0246),a
	ld      (0x024a),a
	ld      (0x0336),a
	ld      (0x0355),a

	; The two sweep helpers occupy OS-7 addresses 00FCh-0163h, which KonaOS
	; reserves for its startup block and SDK gates.  Retarget the only sound
	; manager calls to clean native copies below.
	ld      hl,#native_atn_sweep
	ld      (0x02e7),hl
	ld      hl,#native_freq_sweep
	ld      (0x02ea),hl
	ret

install_jp:
	ld      a,#0xc3
	ld      (de),a
	inc     de
	ld      a,l
	ld      (de),a
	inc     de
	ld      a,h
	ld      (de),a
	ret

native_table_bases:
	.dw     0x1b00,0x3800,0x1800,0x0000,0x2000

; -------------------------------------------------------------------------
; V9958 GRAPHIC 4 initialization.  Page zero is the 256x212 16-colour
; bitmap.  Page one holds the 16 KiB TMS semantic shadow and the top of
; page one contains sprite-mode-2 tables.
; -------------------------------------------------------------------------
_dk_video_init::
	; Display off, G4, page zero, mode-2 sprite layout.
	ld      a,#0x04
	ld      b,#0
	call    vdp_reg
	ld      a,#0x02
	ld      b,#1
	call    vdp_reg
	ld      a,#0x1f
	ld      b,#2
	call    vdp_reg
	xor     a
	ld      b,#3
	call    vdp_reg
	xor     a
	ld      b,#4
	call    vdp_reg
	ld      a,#0xe4
	ld      b,#5
	call    vdp_reg
	ld      a,#0x3f
	ld      b,#6
	call    vdp_reg
	xor     a
	ld      b,#7
	call    vdp_reg
	ld      a,#0x08
	ld      b,#8
	call    vdp_reg
	ld      a,#0x88
	ld      b,#9
	call    vdp_reg
	xor     a
	ld      b,#10
	call    vdp_reg
	ld      a,#0x03
	ld      b,#11
	call    vdp_reg
	ld      a,#0x04
	ld      b,#25
	call    vdp_reg

	; Coleco-compatible colours in the V9958 programmable palette.
	xor     a
	ld      b,#16
	call    vdp_reg
	ld      hl,#native_palette
	ld      b,#32
001$:
	ld      a,(hl)
	out     (VDP_PALETTE),a
	inc     hl
	djnz    001$

	; Clear the 16 KiB semantic shadow at physical 10000h.
	ld      de,#0x0000
	ld      a,#1
	call    vdp_set_write
	ld      bc,#0x4000
	xor     a
002$:
	out     (VDP_DATA),a
	dec     bc
	push    af
	ld      a,b
	or      c
	jr      z,003$
	pop     af
	jr      002$
003$:
	pop     af

	call    clear_bitmap
	ld      a,#1
	ld      (full_dirty),a
	ret

_dk_video_shutdown::
	ld      a,#0x9f
	out     (PSG_PORT),a
	ld      a,#0xbf
	out     (PSG_PORT),a
	ld      a,#0xdf
	out     (PSG_PORT),a
	ld      a,#0xff
	out     (PSG_PORT),a
	ret

native_palette:
	.db 0x00,0x00, 0x00,0x00, 0x12,0x05, 0x33,0x06
	.db 0x27,0x02, 0x37,0x03, 0x62,0x02, 0x27,0x06
	.db 0x72,0x02, 0x73,0x03, 0x62,0x05, 0x64,0x06
	.db 0x12,0x05, 0x65,0x02, 0x66,0x06, 0x77,0x07

; A=value, B=register.
vdp_reg:
	out     (VDP_CTRL),a
	ld      a,b
	or      #0x80
	out     (VDP_CTRL),a
	ret

; A=A16, DE=low 16 bits.  Select a 17-bit VRAM write address.
vdp_set_write:
	add     a,a
	add     a,a
	ld      c,a
	ld      a,d
	rlca
	rlca
	and     #0x03
	or      c
	ld      b,#14
	call    vdp_reg
	ld      a,e
	out     (VDP_CTRL),a
	ld      a,d
	and     #0x3f
	or      #0x40
	out     (VDP_CTRL),a
	ret

; A=A16, DE=low 16 bits.  Select a 17-bit VRAM read address.
vdp_set_read:
	add     a,a
	add     a,a
	ld      c,a
	ld      a,d
	rlca
	rlca
	and     #0x03
	or      c
	ld      b,#14
	call    vdp_reg
	ld      a,e
	out     (VDP_CTRL),a
	ld      a,d
	and     #0x3f
	out     (VDP_CTRL),a
	ret

clear_bitmap:
	; Status register 2 for command completion.
	ld      a,#2
	ld      b,#15
	call    vdp_reg
004$:
	in      a,(VDP_CTRL)
	and     #1
	jr      nz,004$
	ld      a,#32
	ld      b,#17
	call    vdp_reg
	ld      hl,#clear_command
	ld      b,#15
005$:
	ld      a,(hl)
	out     (VDP_INDIRECT),a
	inc     hl
	djnz    005$
	ret

clear_command:
	.db 0,0, 0,0, 0,0, 0,0, 0,1, 212,0, 0,0, 0xc0

; -------------------------------------------------------------------------
; Native BIOS replacements.
; -------------------------------------------------------------------------
native_game_opt:
	; The original options screen is deliberately bypassed.  The first POLLER
	; call injects keypad option 4 (one player, difficulty four).
	ld      a,#1
	ld      (0x702d),a
	ld      a,#4
	ld      (0x70a6),a
	ret

native_poller:
	push    bc
	push    de
	push    hl
	push    ix
	push    iy
	call    _dk_input_poll            ; result in DE
	ld      a,e
	ld      (input_bits),a
	bit     7,a
	jr      z,006$
	ld      a,(_dk_in_event)
	or      a
	jp      z,native_abort
006$:
	ld      a,(input_bits)
	and     #0x0f
	ld      (0x7043),a
	xor     a
	ld      (0x7042),a
	ld      a,(input_bits)
	bit     4,a
	jr      z,007$
	ld      a,#0x40
	ld      (0x7042),a
007$:
	ld      a,(input_bits)
	bit     6,a
	jr      z,008$
	ld      a,#4
	ld      (0x7046),a
008$:
	pop     iy
	pop     ix
	pop     hl
	pop     de
	pop     bc
	ret

native_abort:
	ld      sp,(game_saved_sp)
	jp      dk_game_return

native_rand:
	ld      hl,(BIOS_RANDOM)
	ld      a,h
	xor     l
	rrca
	rr      h
	rr      l
	jr      nc,009$
	ld      a,h
	xor     #0xb4
	ld      h,a
009$:
	inc     hl
	ld      (BIOS_RANDOM),hl
	ld      a,l
	ret

; Relocated OS-7 sound sweep helpers.  The called utility entries at 018Eh,
; 01A7h and 01B1h are present in the safe BIOS tail copied by main.c.
native_freq_sweep:
	ld      a,7 (ix)
	add     a,a
	jr      nz,0091$
	ld      a,5 (ix)
	dec     a
	ret     z
	ld      5 (ix),a
	ret
0091$:
	push    ix
	pop     hl
	ld      de,#6
	add     hl,de
	call    0x018e
	jr      nz,0092$
	call    0x01a7
	dec     hl
	ld      a,(hl)
	dec     a
	ret     z
	ld      (hl),a
	dec     hl
	dec     hl
	ld      a,7 (ix)
	call    0x01b1
	inc     hl
	res     2,(hl)
	or      #0xff
0092$:
	ret

native_atn_sweep:
	ld      a,8 (ix)
	add     a,a
	ret     z
	push    ix
	pop     hl
	ld      de,#9
	add     hl,de
	call    0x018e
	jr      nz,0094$
	call    0x01a7
	dec     hl
	call    0x018e
	jr      z,0093$
	ld      a,(hl)
	and     #0xf0
	ld      e,a
	dec     hl
	dec     hl
	dec     hl
	dec     hl
	ld      a,(hl)
	and     #0xf0
	add     a,e
	ld      e,a
	ld      a,(hl)
	and     #0x0f
	or      e
	ld      (hl),a
	or      #0xff
	jr      0094$
0093$:
	ld      (hl),#0
0094$:
	ret

native_reg_read:
	xor     a
	ret

; B=register, C=value.  Keep OS-7's shadows, but never leave native G4.
native_reg_write:
	push    af
	push    bc
	push    de
	push    hl
	ld      a,#1
	ld      (vdp_busy),a
	ld      a,b
	or      a
	jr      nz,010$
	ld      a,c
	ld      (BIOS_R0),a
	jr      014$
010$:
	cp      #1
	jr      nz,012$
	ld      a,c
	ld      (BIOS_R1),a
	bit     6,a
	jr      z,011$
	call    render_if_dirty
	ld      a,#0x42               ; display on, 16x16, IRQ off
	ld      b,#1
	call    vdp_reg
	call    _dk_start_clock
	jr      014$
011$:
	ld      a,#0x02
	ld      b,#1
	call    vdp_reg
	jr      014$
012$:
	cp      #7
	jr      nz,014$
	ld      a,c
	and     #0x0f
	ld      b,#7
	call    vdp_reg
014$:
	xor     a
	ld      (vdp_busy),a
	pop     hl
	pop     de
	pop     bc
	pop     af
	ret

; HL=source, DE=TMS address, BC=count.
native_vram_write:
	push    af
	push    bc
	push    de
	push    hl
	ld      a,#1
	ld      (vdp_busy),a
	ld      (io_src),hl
	ld      a,d
	and     #0x3f                ; TMS9928 has a 14-bit VRAM address
	ld      d,a
	ld      (io_dst),de
	ld      (io_len),bc
	ld      a,#1
	call    vdp_set_write
015$:
	ld      a,b
	or      c
	jr      z,016$
	ld      a,(hl)
	out     (VDP_DATA),a
	inc     hl
	dec     bc
	jr      015$
016$:
	ld      de,(io_dst)
	ld      a,d
	cp      #0x38
	jr      c,017$
	cp      #0x40
	jr      nc,017$
	; Mirror TMS sprite patterns into the V9958 mode-2 pattern table.
	ld      hl,(io_dst)
	ld      de,#SPR_PATTERN
	or      a
	sbc     hl,de
	ld      de,#NATIVE_PATTERN
	add     hl,de
	ex      de,hl
	ld      a,#1
	call    vdp_set_write
	ld      hl,(io_src)
	ld      bc,(io_len)
0161$:
	ld      a,b
	or      c
	jr      z,019$
	ld      a,(hl)
	out     (VDP_DATA),a
	inc     hl
	dec     bc
	jr      0161$
017$:
	; Small name-table updates are rendered immediately.  Everything else
	; dirties the background for one coalesced redraw at frame/present time.
	ld      de,(io_dst)
	ld      a,d
	cp      #0x18
	jr      c,018$
	cp      #0x1b
	jr      nc,018$
	ld      bc,(io_len)
	ld      a,b
	or      a
	jr      nz,018$
	ld      a,c
	cp      #33
	jr      nc,018$
0171$:
	ld      a,b
	or      c
	jr      z,019$
	push    bc
	push    de
	call    render_name_address
	pop     de
	pop     bc
	inc     de
	dec     bc
	jr      0171$
018$:
	ld      a,#1
	ld      (full_dirty),a
019$:
	xor     a
	ld      (vdp_busy),a
	pop     hl
	pop     de
	pop     bc
	pop     af
	ret

; HL=TMS address, DE=count, A=value.
native_fill_vram:
	push    af
	push    bc
	push    de
	push    hl
	ld      (fill_value),a
	ld      a,#1
	ld      (vdp_busy),a
	ld      a,h
	and     #0x3f                ; preserve the TMS 16 KiB wrap
	ld      h,a
	ex      de,hl                  ; DE=address, HL=count
	ld      b,h
	ld      c,l
	ld      a,#1
	call    vdp_set_write
	ld      a,(fill_value)
020$:
	out     (VDP_DATA),a
	dec     bc
	push    af
	ld      a,b
	or      c
	jr      z,021$
	pop     af
	jr      020$
021$:
	pop     af
	ld      a,#1
	ld      (full_dirty),a
	xor     a
	ld      (vdp_busy),a
	pop     hl
	pop     de
	pop     bc
	pop     af
	ret

; HL=destination, DE=TMS address, BC=count.
native_vram_read:
	push    af
	push    bc
	push    de
	push    hl
	ld      a,#1
	ld      (vdp_busy),a
	ld      a,d
	and     #0x3f
	ld      d,a
	ld      a,#1
	call    vdp_set_read
022$:
	ld      a,b
	or      c
	jr      z,023$
	in      a,(VDP_DATA)
	ld      (hl),a
	inc     hl
	dec     bc
	jr      022$
023$:
	xor     a
	ld      (vdp_busy),a
	pop     hl
	pop     de
	pop     bc
	pop     af
	ret

; -------------------------------------------------------------------------
; Background translator: TMS G2 name/pattern/colour semantics to the V9958
; G4 bitmap.  Only dirty small name writes redraw individual cells; pattern
; and bulk operations coalesce into one complete 32x24 redraw.
; -------------------------------------------------------------------------
render_if_dirty:
	ld      a,(full_dirty)
	or      a
	ret     z
	xor     a
	ld      (full_dirty),a
	jp      render_full

render_full:
	xor     a
	ld      (render_y),a
024$:
	xor     a
	ld      (render_x),a
025$:
	ld      a,(render_y)
	ld      h,#0
	ld      l,a
	add     hl,hl
	add     hl,hl
	add     hl,hl
	add     hl,hl
	add     hl,hl
	ld      de,#NAME_BASE
	add     hl,de
	ld      a,(render_x)
	ld      e,a
	ld      d,#0
	add     hl,de
	ex      de,hl
	call    render_name_address
	ld      a,(render_x)
	inc     a
	ld      (render_x),a
	cp      #32
	jr      nz,025$
	ld      a,(render_y)
	inc     a
	ld      (render_y),a
	cp      #24
	jr      nz,024$
	ret

; DE=name-table address.
render_name_address:
	push    af
	push    bc
	push    de
	push    hl
	ld      h,d
	ld      l,e
	ld      bc,#NAME_BASE
	or      a
	sbc     hl,bc
	ld      a,l
	and     #31
	ld      (render_x),a
	ld      a,h
	add     a,a
	add     a,a
	add     a,a
	ld      b,a
	ld      a,l
	rlca
	rlca
	rlca
	and     #7
	or      b
	ld      (render_y),a
	ld      a,#1
	call    vdp_set_read
	in      a,(VDP_DATA)
	ld      (render_tile),a
	call    draw_tile
	pop     hl
	pop     de
	pop     bc
	pop     af
	ret

draw_tile:
	xor     a
	ld      (render_row),a
026$:
	; DE = section*0800h + tile*8 + row.
	ld      a,(render_tile)
	ld      l,a
	ld      h,#0
	add     hl,hl
	add     hl,hl
	add     hl,hl
	ld      a,(render_y)
	and     #0x18
	add     a,h
	ld      h,a
	ld      a,(render_row)
	ld      e,a
	ld      d,#0
	add     hl,de
	ld      (tile_offset),hl
	ex      de,hl
	ld      a,#1
	call    vdp_set_read
	in      a,(VDP_DATA)
	ld      (tile_pattern),a
	ld      hl,(tile_offset)
	ld      de,#0x2000
	add     hl,de
	ex      de,hl
	ld      a,#1
	call    vdp_set_read
	in      a,(VDP_DATA)
	ld      (tile_color),a
	and     #0x0f
	ld      (tile_bg),a
	ld      a,(tile_color)
	rrca
	rrca
	rrca
	rrca
	and     #0x0f
	ld      (tile_fg),a
	ld      a,(tile_pattern)
	ld      (shift_pattern),a
	ld      hl,#pixel_bytes
	ld      b,#4
027$:
	call    next_pixel
	add     a,a
	add     a,a
	add     a,a
	add     a,a
	ld      c,a
	call    next_pixel
	or      c
	ld      (hl),a
	inc     hl
	djnz    027$

	; DE = (tile_y*8+row)*128 + tile_x*4.
	ld      a,(render_y)
	add     a,a
	add     a,a
	add     a,a
	ld      b,a
	ld      a,(render_row)
	add     a,b
	ld      b,a
	and     #1
	rrca                            ; 0 or 80h
	ld      e,a
	ld      a,b
	srl     a
	ld      d,a
	ld      a,(render_x)
	add     a,a
	add     a,a
	add     a,e
	ld      e,a
	jr      nc,028$
	inc     d
028$:
	xor     a
	call    vdp_set_write
	ld      hl,#pixel_bytes
	ld      b,#4
029$:
	ld      a,(hl)
	out     (VDP_DATA),a
	inc     hl
	djnz    029$
	ld      a,(render_row)
	inc     a
	ld      (render_row),a
	cp      #8
	jp      nz,026$
	ret

next_pixel:
	ld      a,(shift_pattern)
	add     a,a
	ld      (shift_pattern),a
	jr      nc,030$
	ld      a,(tile_fg)
	ret
030$:
	ld      a,(tile_bg)
	ret

; -------------------------------------------------------------------------
; V9958 sprite mode 2.  OS-7 has already built and ordered the local TMS
; four-byte records.  Each colour is expanded to the mode-2 per-line colour
; table, lifting the TMS four-sprites-per-line limit to eight.
; -------------------------------------------------------------------------
native_write_sprites:
	push    af
	push    bc
	push    de
	push    hl
	push    ix
	push    iy
	ld      (sprite_count),a
	ld      a,#1
	ld      (vdp_busy),a
	xor     a
	ld      (sprite_slot),a
031$:
	ld      a,(sprite_count)
	or      a
	jr      z,036$
	ld      a,0 (ix)
	inc     ix
	ld      l,a
	ld      h,#0
	add     hl,hl
	add     hl,hl
	ld      de,(LOCAL_SPR_PTR)
	add     hl,de
	ld      a,(hl)
	ld      (sprite_y),a
	inc     hl
	ld      a,(hl)
	ld      (sprite_x),a
	inc     hl
	ld      a,(hl)
	ld      (sprite_pattern),a
	inc     hl
	ld      a,(hl)
	and     #0x0f
	ld      (sprite_color),a
	ld      a,(sprite_pattern)
	call    sync_sprite_pattern

	; Per-line colour block.
	ld      a,(sprite_slot)
	ld      l,a
	ld      h,#0
	add     hl,hl
	add     hl,hl
	add     hl,hl
	add     hl,hl
	ld      de,#NATIVE_COLOR
	add     hl,de
	ex      de,hl
	ld      a,#1
	call    vdp_set_write
	ld      a,(sprite_color)
	ld      b,#16
032$:
	out     (VDP_DATA),a
	djnz    032$

	; Four-byte mode-2 SAT record.
	ld      a,(sprite_slot)
	ld      l,a
	ld      h,#0
	add     hl,hl
	add     hl,hl
	ld      de,#NATIVE_SAT
	add     hl,de
	ex      de,hl
	ld      a,#1
	call    vdp_set_write
	ld      a,(sprite_y)
	out     (VDP_DATA),a
	ld      a,(sprite_x)
	out     (VDP_DATA),a
	ld      a,(sprite_pattern)
	out     (VDP_DATA),a
	xor     a
	out     (VDP_DATA),a

	ld      a,(sprite_slot)
	inc     a
	ld      (sprite_slot),a
	ld      a,(sprite_count)
	dec     a
	ld      (sprite_count),a
	jp      031$
036$:
	; End marker after the final active sprite.
	ld      a,(sprite_slot)
	ld      l,a
	ld      h,#0
	add     hl,hl
	add     hl,hl
	ld      de,#NATIVE_SAT
	add     hl,de
	ex      de,hl
	ld      a,#1
	call    vdp_set_write
	ld      a,#0xd8
	out     (VDP_DATA),a
	call    render_if_dirty
	xor     a
	ld      (vdp_busy),a
	pop     iy
	pop     ix
	pop     hl
	pop     de
	pop     bc
	pop     af
	ret

; A=pattern number.  A 16x16 sprite consumes four consecutive 8-byte
; patterns; copy those 32 bytes from the TMS shadow to the native mode-2
; pattern table.  Doing this on presentation also handles OS-7's deliberate
; 14-bit wrap when it updates pattern number 256.
sync_sprite_pattern:
	push    af
	push    bc
	push    de
	push    hl
	ld      l,a
	ld      h,#0
	add     hl,hl
	add     hl,hl
	add     hl,hl
	push    hl
	ld      de,#SPR_PATTERN
	add     hl,de
	ex      de,hl
	ld      a,#1
	call    vdp_set_read
	ld      hl,#sprite_pattern_bytes
	ld      b,#32
037$:
	in      a,(VDP_DATA)
	ld      (hl),a
	inc     hl
	djnz    037$
	pop     hl
	ld      de,#NATIVE_PATTERN
	add     hl,de
	ex      de,hl
	ld      a,#1
	call    vdp_set_write
	ld      hl,#sprite_pattern_bytes
	ld      b,#32
038$:
	ld      a,(hl)
	out     (VDP_DATA),a
	inc     hl
	djnz    038$
	pop     hl
	pop     de
	pop     bc
	pop     af
	ret

; -------------------------------------------------------------------------
; Cartridge entry and 60 Hz frame callback.
; -------------------------------------------------------------------------
_dk_run_game::
	push    ix
	push    iy
	ld      (game_saved_sp),sp
	call    0x8048

dk_game_return:
	pop     iy
	pop     ix
	ret

_dk_frame::
	ld      a,(vdp_busy)
	or      a
	ret     nz
	call    0x852e
	ret

	.area _DATA
game_saved_sp:  .ds 2
io_src:         .ds 2
io_dst:         .ds 2
io_len:         .ds 2
tile_offset:    .ds 2
full_dirty:     .ds 1
vdp_busy:       .ds 1
input_bits:     .ds 1
fill_value:     .ds 1
render_x:       .ds 1
render_y:       .ds 1
render_tile:    .ds 1
render_row:     .ds 1
tile_pattern:   .ds 1
shift_pattern:  .ds 1
tile_color:     .ds 1
tile_fg:        .ds 1
tile_bg:        .ds 1
pixel_bytes:    .ds 4
sprite_count:   .ds 1
sprite_slot:    .ds 1
sprite_y:       .ds 1
sprite_x:       .ds 1
sprite_pattern: .ds 1
sprite_color:   .ds 1
sprite_pattern_bytes: .ds 32
