; ColecoGo: the V9958 state a TMS9918-era ColecoVision BIOS cannot select.
;
; _coleco_video masks interrupts and programs the V9958 as the handoff
; leaves it: the TMS-era register baseline (R#8 64K DRAM, R#25 with the
; WAIT state), the 16-entry ColecoVision palette and, last, the board's
; VDP interrupt routed to the NMI.  It returns with interrupts masked; the
; console must not draw afterwards.  Both handoffs call it before the
; memory switch.

	.module colecogo_video

	.globl _coleco_video

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

_coleco_video::
	di
	ld      a,#V9958_CONFIG_PORCH_OFF
	out     (V9958_CONFIG_PORT),a
	ld      hl,#video_tms_baseline
	ld      c,#((video_tms_baseline_end-video_tms_baseline)/2)
video_tms_loop:
	ld      a,(hl)
	inc     hl
	ld      b,(hl)
	inc     hl
	call    video_vdp_write
	dec     c
	jr      nz,video_tms_loop

	ld      a,#V9958_R25_WAIT_ON
	ld      b,#25
	call    video_vdp_write
	xor     a
	out     (V9958_CONFIG_PORT),a
	in      a,(V9958_COMMAND_PORT)

	ld      hl,#video_palette
	ld      c,#0
	ld      d,#16
video_palette_loop:
	ld      a,c
	ld      b,#16
	call    video_vdp_write
	ld      a,(hl)
	inc     hl
	out     (V9958_PALETTE_PORT),a
	call    video_vdp_delay
	ld      a,(hl)
	inc     hl
	out     (V9958_PALETTE_PORT),a
	call    video_vdp_delay
	inc     c
	dec     d
	jr      nz,video_palette_loop

	ld      a,#V9958_CONFIG_NMI
	out     (V9958_CONFIG_PORT),a
	ret

video_vdp_write:
	out     (V9958_COMMAND_PORT),a
	call    video_vdp_delay
	ld      a,b
	or      #0x80
	out     (V9958_COMMAND_PORT),a
	jp      video_vdp_delay

video_vdp_delay:
	push    bc
	ld      b,#V9958_DELAY_COUNT
video_delay_loop:
	djnz    video_delay_loop
	pop     bc
	ret

video_tms_baseline:
	.db V9958_R25_WAIT_OFF,25
	.db 0x00,46, 0x00,1, 0x00,0, 0x00,9
	.db 0x00,10, 0x00,11, 0x00,12, 0x00,13
	.db 0x00,14, 0x00,15, 0x00,16, 0x00,17
	.db 0x00,18, 0x00,19, 0x00,23, 0x00,26
	.db 0x00,27, 0x00,45, V9958_R8_64K_DRAM,8
video_tms_baseline_end:

video_palette:
	.db 0x00,0x00, 0x00,0x00, 0x12,0x05, 0x33,0x06
	.db 0x27,0x02, 0x37,0x03, 0x62,0x02, 0x27,0x06
	.db 0x72,0x02, 0x73,0x03, 0x62,0x05, 0x64,0x06
	.db 0x12,0x05, 0x65,0x02, 0x66,0x06, 0x77,0x07
