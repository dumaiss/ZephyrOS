; ZephyrOS IPC acceptance: the caller-buffer probe's entry stub (test
; artifact, work package 39 correction 01).
;
; The probe links with NO_CRT0, LOWBASE=0x0300 and CODEBASE=0x0344, so the
; SDK startup is absent and the program owns its entry.  Like the M9 fsraw
; fixture this is an assembly label without the C underscore prefix, which is what the
; zex_program rule's ENTRY= option names in the link map.

	.module ipccopy_entry
	.globl ipc_copy_entry
	.globl _ipc_copy_entry

	.area _CODE

ipc_copy_entry::
	jp	_ipc_copy_entry
