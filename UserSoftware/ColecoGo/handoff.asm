; ColecoGo KonaOS no-return handoff (Zephyr-80; the Mk2 board enters the
; image through SYS_PERSONALITY instead).
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
	.globl _coleco_video

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

	; The V9958 state a TMS9918-era BIOS cannot select (video.asm).
	call    _coleco_video

	; The instruction after this OUT is fetched from bank 5.  It is valid
	; because the C loader installed this complete same-address stub there.
	ld      a,#(MODE_APPLICATION|TARGET_BANK)
	out     (MEMCTL_PORT),a
	jp      TARGET_STAGE_B

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
