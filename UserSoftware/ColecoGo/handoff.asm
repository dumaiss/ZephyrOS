; ColecoGo KonaOS no-return handoff.
;
; _handoff runs at its linked address in the KonaOS process bank.  The C
; loader has copied these same bytes to the same address in target bank 5, so
; execution remains valid across the memory-latch OUT.  The following JP lands
; in the position-independent Stage B body installed at 5F00h.

	.module colecogo_handoff

	.globl _handoff
	.globl _handoff_begin
	.globl _handoff_end
	.globl _stage_b_begin
	.globl _stage_b_end

MEMCTL_PORT             = 0x00
MODE_FLAT               = 0x08
MODE_APPLICATION        = 0x10
TARGET_BANK             = 0x05
TARGET_STAGE_B          = 0x5f00
TARGET_STUB_BACKUP      = 0x7a00
TARGET_BIOS_PAGE        = 0x7d00
TARGET_CART_TAIL        = 0x7e00
TARGET_UPPER_STAGE      = 0x2000
TARGET_UPPER_PREFIX     = 0x3f00

SIO0A_CTRL              = 0x21
SIO0B_CTRL              = 0x23
SIO1A_CTRL              = 0x31
SIO1B_CTRL              = 0x33
CTC0_CTRL               = 0x40
CTC1_CTRL               = 0x42
CTC2_CTRL               = 0x41
CTC3_CTRL               = 0x43
CTC_RESET_DISABLE       = 0x03

V9958_COMMAND_PORT      = 0xa1
V9958_PALETTE_PORT      = 0xa2
V9958_CONFIG_PORT       = 0xa4
V9958_CONFIG_NMI        = 0x01
V9958_CONFIG_PORCH_OFF  = 0x02
V9958_R8_64K_DRAM       = 0x08
V9958_R25_WAIT_OFF      = 0x00
V9958_R25_WAIT_ON       = 0x04
V9958_DELAY_COUNT       = 16

	.area _CODE

_handoff_begin::
_handoff::
	di

	; Stop every interrupt source owned by KonaOS before its mappings vanish.
	ld      a,#CTC_RESET_DISABLE
	out     (CTC0_CTRL),a
	out     (CTC1_CTRL),a
	out     (CTC2_CTRL),a
	out     (CTC3_CTRL),a

	ld      a,#0x01
	out     (SIO0B_CTRL),a
	xor     a
	out     (SIO0B_CTRL),a
	ld      a,#0x01
	out     (SIO0A_CTRL),a
	xor     a
	out     (SIO0A_CTRL),a
	ld      a,#0x09
	out     (SIO0A_CTRL),a
	xor     a
	out     (SIO0A_CTRL),a
	ld      a,#0x01
	out     (SIO1A_CTRL),a
	xor     a
	out     (SIO1A_CTRL),a
	ld      a,#0x01
	out     (SIO1B_CTRL),a
	xor     a
	out     (SIO1B_CTRL),a
	ld      a,#0x09
	out     (SIO1B_CTRL),a
	xor     a
	out     (SIO1B_CTRL),a

	; Establish the V9958 state that a TMS9918-era BIOS cannot select.
	ld      a,#V9958_CONFIG_PORCH_OFF
	out     (V9958_CONFIG_PORT),a
	ld      hl,#handoff_tms_baseline
	ld      c,#((handoff_tms_baseline_end-handoff_tms_baseline)/2)
handoff_tms_loop:
	ld      a,(hl)
	inc     hl
	ld      b,(hl)
	inc     hl
	call    handoff_vdp_write
	dec     c
	jr      nz,handoff_tms_loop

	ld      a,#V9958_R25_WAIT_ON
	ld      b,#25
	call    handoff_vdp_write
	xor     a
	out     (V9958_CONFIG_PORT),a
	in      a,(V9958_COMMAND_PORT)

	ld      hl,#handoff_palette
	ld      c,#0
	ld      d,#16
handoff_palette_loop:
	ld      a,c
	ld      b,#16
	call    handoff_vdp_write
	ld      a,(hl)
	inc     hl
	out     (V9958_PALETTE_PORT),a
	call    handoff_vdp_delay
	ld      a,(hl)
	inc     hl
	out     (V9958_PALETTE_PORT),a
	call    handoff_vdp_delay
	inc     c
	dec     d
	jr      nz,handoff_palette_loop

	ld      a,#V9958_CONFIG_NMI
	out     (V9958_CONFIG_PORT),a

	; The instruction after this OUT is fetched from bank 5.  It is valid
	; because the C loader installed this complete same-address stub there.
	ld      a,#(MODE_APPLICATION|TARGET_BANK)
	out     (MEMCTL_PORT),a
	jp      TARGET_STAGE_B

handoff_vdp_write:
	out     (V9958_COMMAND_PORT),a
	call    handoff_vdp_delay
	ld      a,b
	or      #0x80
	out     (V9958_COMMAND_PORT),a
	jp      handoff_vdp_delay

handoff_vdp_delay:
	push    bc
	ld      b,#V9958_DELAY_COUNT
handoff_delay_loop:
	djnz    handoff_delay_loop
	pop     bc
	ret

handoff_tms_baseline:
	.db V9958_R25_WAIT_OFF,25
	.db 0x00,46, 0x00,1, 0x00,0, 0x00,9
	.db 0x00,10, 0x00,11, 0x00,12, 0x00,13
	.db 0x00,14, 0x00,15, 0x00,16, 0x00,17
	.db 0x00,18, 0x00,19, 0x00,23, 0x00,26
	.db 0x00,27, 0x00,45, V9958_R8_64K_DRAM,8
handoff_tms_baseline_end:

handoff_palette:
	.db 0x00,0x00, 0x00,0x00, 0x12,0x05, 0x33,0x06
	.db 0x27,0x02, 0x37,0x03, 0x62,0x02, 0x27,0x06
	.db 0x72,0x02, 0x73,0x03, 0x62,0x05, 0x64,0x06
	.db 0x12,0x05, 0x65,0x02, 0x66,0x06, 0x77,0x07
_handoff_end::

; Copied to 5F00h.  There are no internal address references, so this byte
; sequence is position-independent.  It uses no stack after the bank switch.
_stage_b_begin::
	ld      a,#(MODE_FLAT|TARGET_BANK)
	out     (MEMCTL_PORT),a

	; Restore the final image hidden by the same-address handoff stub.
	ld      hl,#TARGET_STUB_BACKUP
	ld      de,#_handoff_begin
	ld      bc,#(_handoff_end-_handoff_begin)
	ldir

	; Install protected BIOS page zero, then finish the upper cartridge.
	ld      hl,#TARGET_BIOS_PAGE
	ld      de,#0x0000
	ld      bc,#0x0100
	ldir
	ld      hl,#TARGET_UPPER_STAGE
	ld      de,#0xc000
	ld      bc,#TARGET_UPPER_PREFIX
	ldir
	ld      hl,#TARGET_CART_TAIL
	ld      de,#0xff00
	ld      bc,#0x0100
	ldir

	; Coleco RAM.  Stage B remains below 6000h while this executes.
	xor     a
	ld      hl,#0x6000
	ld      (hl),a
	ld      de,#0x6001
	ld      bc,#0x1fff
	ldir
	jp      0x0000
_stage_b_end::
