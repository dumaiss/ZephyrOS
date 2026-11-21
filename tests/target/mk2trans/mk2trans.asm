; -----------------------------------------------------------------------------
; Zephyr-80 Mk2 transitions image (test-only; tests/test_mk2_transitions.py).
;
; The kernel's transition sources assembled for Mk2 (the syscall gate,
; page zero, the IRQ executive and its vectors, the event core, the Mk2
; mapping state), around the bootstrap of the Mk2 boot proof, a stub
; dispatcher, a stub IRQ dispatcher, an RT handler and a "process" that
; runs in the working bank.  The tests drive the process from the
; simulator: they set registers, point PC at a fragment and read the
; machine back.
;
; Physical pages (DAT entries after boot):
;
;   system  0 40h  page-0 image (the low stubs)      working  8 50h  page 0
;           1 41h  kernel code (_CODE, 2000h)                9 51h
;           2 42h  kernel data (_DATA 4000h), ISR stack     10 52h  stack (6000h down)
;           3 43h                                           11 53h  process code (6000h)
;           4 44h  the remap window (sys_remap)             12 54h
;           5 45h                                           13 55h  the far window (p_far)
;           6 46h                                           14 56h
;           7 7Fh  common                                   15 7Fh  common
;
; ROM pages 00h (boot, low stubs), 01h (kernel code), 03h (process code)
; and 07h (common) are the linked 64 KiB; the boot copies them into SRAM.
; -----------------------------------------------------------------------------

	.module	mk2trans
	.include "hardware.inc"
	.include "memory.inc"
	.include "irq.inc"
	.include "map_ops.inc"
	; The peripheral cards are the Mk1 board's.
	.include "platform/zephyr80/public/hardware.inc"

	.globl	kos_syscall_entry
	.globl	_irq_arch_init, _irq_arch_lock, _irq_arch_unlock
	.globl	_irq_active_source, irq_saved_latch
	.globl	_kos_event_table, _kos_event_maybe, _kos_event_in_handler
	.globl	_kos_event_frame, _kos_event_frame_latch, _kos_event_latch
	.globl	_kos_event_return_frame, _kos_event_pick, _kos_event_enter
	.globl	_kos_event_unwind
	.globl	_kos_dat_running
	.globl	s__COMMON_DATA, l__COMMON_DATA, s__DATA, l__DATA

SYS_SUM		= 1	; HL = w0 + w1 of the block, also stored at +4
SYS_FAIL	= 2	; carry, A = 5, HL = 1234h
SYS_LOAD	= 3	; the body loads another image into the working bank
SYS_EVENT_RETURN = 4	; the handler's return (kos_event_return)
SYS_SPIN	= 5	; spins w0 times with the caller's interrupt state
SYS_REMAP	= 6	; maps page b0 at system entry 4, HL = its byte at 8000h
SYS_POST	= 7	; posts row 0's event; HL = 0777h
SYS_NOP		= 8	; HL = 0

P_STACK_TOP	= 0x6000	; the process stack, working entry 10
P_TALLY		= 0x0180	; the handler's event count (process page 0)
PROCESS_PID	= 1

	.area	_CODE
	.area	_DATA
	.area	_PROC
	.area	_COMMON
	.area	_COMMON_DATA

; ---- ROM page 0: the reset view and the low stubs --------------------------

	.area	_MK2TRANS_LOW (ABS)
	.org	0x0000
	di
	jp	boot

	.org	KOS_NMI_ENTRY
nmi_stub::
	jp	KOS_RT_VECTOR

	.org	KOS_PAGE_ZERO_END
; Pass mode, no RAM: program both banks with B = A (wiring-agnostic), map,
; copy ROM pages into SRAM through entry 1 (source) and entry 2
; (destination), then the final images.  The last loop remaps entry 0
; from ROM page 00h to its SRAM copy under the running code.
boot:
	ld	hl,#boot_image
	ld	c,#0
boot_entry:
	ld	a,(hl)
	ld	b,a
	out	(c),a
	inc	hl
	inc	c
	ld	a,c
	cp	#2 * KOS_DAT_ENTRIES
	jr	nz,boot_entry
	ld	bc,#KOS_DAT_CONTROL_PORT
	ld	a,#KOS_DAT_CTL_SYSTEM
	out	(c),a
	ld	hl,#copy_table
boot_copy:
	ld	a,(hl)
	cp	#0xFF
	jr	z,boot_final
	out	(KOS_DAT_SYSTEM_PORT + 1),a
	inc	hl
	ld	a,(hl)
	out	(KOS_DAT_SYSTEM_PORT + 2),a
	inc	hl
	exx
	ld	hl,#0x2000
	ld	de,#0x4000
	ld	bc,#KOS_DAT_PAGE_SIZE
	ldir
	exx
	jr	boot_copy
boot_final:
	ld	hl,#final_image
	ld	c,#0
boot_final_entry:
	ld	a,(hl)
	ld	b,a
	out	(c),a
	inc	hl
	inc	c
	ld	a,c
	cp	#2 * KOS_DAT_ENTRIES
	jr	nz,boot_final_entry
	jp	t_start

boot_image::
	.db	0x00, 0x01, 0x41, 0x43, 0x44, 0x45, 0x46, 0x7F
	.db	0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x7F
copy_table::
	.db	0x01, 0x41		; kernel code
	.db	0x03, 0x53		; process code
	.db	0x07, 0x7F		; common
	.db	0x00, 0x40		; the system page-0 image
	.db	0x00, 0x50		; the process page-0 image
	.db	0xFF
final_image::
	.db	0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x7F
	.db	0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x7F

; ---- the process (working bank, 6000h) ------------------------------------

	.area	_PROC
p_idle::
	jr	p_idle
p_call::				; the registers are the syscall's
	rst	0x30
p_return::
	jr	p_return
p_far::					; A = page: open the far window
	di
	KOS_DAT_REMAP 13
	ei
p_far_done::
	jr	p_far_done
; The event trampoline: RETI lands here with BC = handler, HL = count,
; DE = missed; SP is the frame.
p_trampoline::
	ld	de,#p_trampoline_back
	push	de
	push	bc
	ret
p_trampoline_back:
	ld	a,#SYS_EVENT_RETURN
	rst	0x30
p_handler::				; HL = count
	ld	de,(P_TALLY)
	add	hl,de
	ld	(P_TALLY),hl
	ret

; ---- kernel code (system bank, 2000h) -------------------------------------

	.area	_CODE

; The dispatcher boundary's C side (kernel/syscall/syscall.c's role).
_kernel_syscall_dispatch::
	ld	a,(KOS_DAT_CONTROL)	; where the body runs
	ld	(t_body_control),a
	ld	a,(KOS_DAT_SYSTEM)
	ld	(t_body_page0),a
	ld	a,(_kos_syscall_number)
	cp	#SYS_SUM
	jp	z,sys_sum
	cp	#SYS_FAIL
	jp	z,sys_fail
	cp	#SYS_LOAD
	jp	z,sys_load
	cp	#SYS_EVENT_RETURN
	jp	z,sys_event_return
	cp	#SYS_SPIN
	jp	z,sys_spin
	cp	#SYS_REMAP
	jp	z,sys_remap
	cp	#SYS_POST
	jp	z,sys_post
	cp	#SYS_NOP
	jp	z,sys_nop
	ld	hl,#0
	ld	a,#1
	jr	sys_error

sys_nop:
	ld	hl,#0
sys_ok:
	ld	(_kos_syscall_result_hl),hl
	xor	a
	ld	(_kos_syscall_result_a),a
	ld	(_kos_syscall_result_c),a
	ret

sys_fail:
	ld	hl,#0x1234
	ld	a,#5
sys_error:
	ld	(_kos_syscall_result_hl),hl
	ld	(_kos_syscall_result_a),a
	ld	a,#1
	ld	(_kos_syscall_result_c),a
	ret

; The block is in the caller's page 0, mapped as system entry 0.
sys_sum:
	ld	hl,(_kos_syscall_args)
	ld	e,(hl)
	inc	hl
	ld	d,(hl)
	inc	hl
	ld	a,(hl)
	inc	hl
	ld	h,(hl)
	ld	l,a
	add	hl,de
	ex	de,hl
	ld	hl,(_kos_syscall_args)
	ld	bc,#4
	add	hl,bc
	ld	(hl),e
	inc	hl
	ld	(hl),d
	ex	de,hl
	jr	sys_ok

sys_load:
	call	_irq_arch_lock
	push	af
	ld	hl,#t_other_image
	KOS_DAT_LOAD_WORKING
	pop	af
	call	_irq_arch_unlock
	jr	sys_nop

sys_spin:
	ld	hl,(_kos_syscall_args)
	ld	c,(hl)
	inc	hl
	ld	b,(hl)
sys_spin_loop::
	dec	bc
	ld	a,b
	or	c
	jr	nz,sys_spin_loop
	jr	sys_nop

; Remap one system entry and back, under the caller's interrupt state.
sys_remap::
	ld	a,(KOS_DAT_SYSTEM + 4)
	ld	(t_saved_entry4),a
	ld	hl,(_kos_syscall_args)
	ld	a,(hl)
	KOS_DAT_REMAP 4
sys_remap_mapped::
	ld	a,(0x8000)
	ld	(t_remap_byte),a
	ld	a,(t_saved_entry4)
	KOS_DAT_REMAP 4
sys_remap_done::
	ld	a,(t_remap_byte)
	ld	l,a
	ld	h,#0
	jr	sys_ok

; event_post (kernel/event/event.c's role) for the running process's row.
sys_post:
	call	t_post
	ld	hl,#0x0777
	jr	sys_ok

; kos_event_return (kernel/event/event_syscall.c) for the one process.
sys_event_return:
	call	_irq_arch_lock
	xor	a
	ld	(_kos_event_in_handler + PROCESS_PID),a
	call	_kos_event_return_frame
	ld	(_kos_event_frame + 2 * PROCESS_PID),de
	ld	a,(_kos_event_frame_latch + PROCESS_PID)
	ld	(_kos_event_latch),a
sys_event_again:
	call	_kos_event_pick
	ld	a,d
	or	e
	jr	z,sys_event_unwind
	call	_kos_event_enter
sys_event_unwind:
	ld	hl,(_kos_event_frame + 2 * PROCESS_PID)
	jp	_kos_event_unwind

; The IRQ executive's dispatcher (kernel/irq/irq.c's role): count the
; source, record the context, and let CTC3 post the event when armed.
_irq_dispatch::
	ld	a,(KOS_DAT_CONTROL)
	ld	(t_irq_control),a
	ld	a,(irq_saved_latch)
	ld	(t_irq_value),a
	ld	a,(_irq_active_source)
	ld	e,a
	ld	d,#0
	ld	hl,#t_irq_count
	add	hl,de
	inc	(hl)
	cp	#3
	ret	nz
	ld	a,(t_post_on_irq)
	or	a
	ret	z
	jp	t_post

	.area	_DATA
_kos_syscall_number::
	.ds	1
_kos_syscall_args::
	.ds	2
_kos_syscall_result_a::
	.ds	1
_kos_syscall_result_c::
	.ds	1
_kos_syscall_result_hl::
	.ds	2
t_saved_entry4:
	.ds	1
t_remap_byte:
	.ds	1

; ---- common: start, RT handler, images, test state -------------------------

	.area	_COMMON

t_start::
	ld	sp,#KOS_STACK_INIT_SP
	; The mirrors, after the boot's OUTs (architecture 2.7.6: no NMI
	; before the bootstrap has a RAM stack).
	ld	hl,#final_image
	ld	de,#KOS_DAT_SYSTEM
	ld	bc,#2 * KOS_DAT_ENTRIES
	ldir
	ld	a,#KOS_DAT_CTL_SYSTEM
	ld	(KOS_DAT_CONTROL),a
	ld	hl,#t_process_image
	ld	(KOS_DAT_CACHED),hl
	ld	hl,#s__COMMON_DATA
	ld	bc,#l__COMMON_DATA
	call	t_zero
	ld	hl,#s__DATA
	ld	bc,#l__DATA
	call	t_zero
	; The running program's image: an event's home mapping
	; (KOS_MAP_HOME closes the far window onto it).
	ld	hl,#t_process_image
	ld	(_kos_dat_running),hl
	call	_irq_arch_init
	di
	xor	a			; CTC vector base 00h
	out	(KOS_CTC0_PORT),a
	ld	a,(t_ctc3_constant)
	or	a
	jr	z,t_start_launch
	ld	b,a
	ld	a,#KOS_CTC2_CONTROL_TIMER
	out	(KOS_CTC3_PORT),a
	ld	a,b
	out	(KOS_CTC3_PORT),a
t_start_launch:
	; Into the process: the working bank holds its image.
	ld	a,(KOS_DAT_WORKING)
	KOS_MAP_RESTORE
	ld	sp,#P_STACK_TOP
	ei
	jp	p_idle

; HL = start, BC = length (may be 0).
t_zero:
	ld	a,b
	or	c
	ret	z
	ld	(hl),#0
	inc	hl
	dec	bc
	jr	t_zero

; row 0: one more pending event (event_post).
t_post:
	ld	hl,(_kos_event_table + 2)
	inc	hl
	ld	(_kos_event_table + 2),hl
	ld	a,#1
	ld	(_kos_event_maybe),a
	ret

; The RT owner's handler: two bytes of stack, no mapping change.
t_rt_tick::
	push	hl
	ld	hl,(t_rt_count)
	inc	hl
	ld	(t_rt_count),hl
	pop	hl
	retn

; Called from the system bank: the gate refuses it.
t_system_call::
	rst	0x30
t_system_call_back::
	jr	t_system_call_back

; The copy-through-common alternative's core: an argument block of
; t_copy_len bytes (looked up per call) copied from the caller's page 0
; into common before the call and back after it, while the caller's
; mapping is in force.  Measured, not used.
t_copy_core::
	ld	a,(t_copy_number)
	ld	e,a
	ld	d,#0
	ld	hl,#t_copy_len
	add	hl,de
	ld	c,(hl)
	ld	b,#0
	ld	(t_copy_bc),bc
	ld	hl,(t_copy_src)
	ld	de,#t_copy_buffer
	ldir
t_copy_mid::
	ld	bc,(t_copy_bc)
	ld	hl,#t_copy_buffer
	ld	de,(t_copy_src)
	ldir
t_copy_done::
	jr	t_copy_done

_early_console_init::
	ret
_panic::
	ld	(t_panic),hl
	halt

; The configuration the tests patch into the ROM image.
t_ctc3_constant::
	.db	0
t_copy_len::
	.db	16, 16, 16, 16

t_process_image::
	.db	0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x7F
t_other_image::
	.db	0x60, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x7F

	.area	_COMMON_DATA
t_body_control::
	.ds	1
t_body_page0::
	.ds	1
t_irq_control::
	.ds	1
t_irq_value::
	.ds	1
t_irq_count::
	.ds	7
t_post_on_irq::
	.ds	1
t_rt_count::
	.ds	2
t_panic::
	.ds	2
t_copy_number::
	.ds	1
t_copy_src::
	.ds	2
t_copy_bc::
	.ds	2
t_copy_buffer::
	.ds	16

; The RT vector: the test owns the real-time interrupt.
	.area	_MK2TRANS_RT (ABS)
	.org	KOS_RT_VECTOR
	jp	t_rt_tick
	.org	KOS_RT_IDLE
	retn
