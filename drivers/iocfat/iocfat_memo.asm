; -----------------------------------------------------------------------------
; The IOC-FAT listing memo (iocfat.c, the enumeration operation): the entries
; of the last enumeration batch - name, type and size from their READDIR
; reply - so a stat of one of them is answered without the card, once per
; entry.  iocfat.c owns when the memo is valid (the listed path in
; iocfat_list_path_storage, the media generation, forgetting it on every
; other operation); this file owns the table, filling it and the lookup.
;
; Memo entry, 18 bytes: name 0..12 (an unpacked 8.3 name and its NUL),
; type 13 (KFS_TYPE_*; 0 once a stat consumed it), size 14..17.
; -----------------------------------------------------------------------------

	.module iocfat_memo

	.globl _iocfat_memo_count
	.globl _iocfat_memo_add
	.globl _iocfat_memo_stat
	.globl _iocfat_list_path_storage

MEMO_ENTRIES   = 8
MEMO_NAME      = 13		; name bytes, NUL included
MEMO_TYPE      = 13
MEMO_SIZE      = 14
MEMO_BYTES     = 18
DIRENT_NAMELEN = 2		; struct kfs_dirent (abi/kos/fs.h)
DIRENT_SIZE    = 4
DIRENT_NAME    = 16
STAT_SIZE      = 4		; struct kfs_stat

	.area _DRIVER_IO
_iocfat_memo_count::
	.ds	1
memo_table:
	.ds	MEMO_ENTRIES * MEMO_BYTES

	.area _CODE

; HL = the address of memo entry A.  Destroys DE.
memo_entry:
	ld	l,a
	ld	h,#0
	add	hl,hl
	ld	d,h
	ld	e,l			; DE = 2a
	add	hl,hl
	add	hl,hl
	add	hl,hl			; HL = 16a
	add	hl,de
	ld	de,#memo_table
	add	hl,de
	ret

; ---------------------------------------------------------------------------
; void iocfat_memo_add(const struct kfs_dirent *file);  HL = file
;   Append one listed entry while the table has room, its name fits and a
;   listed path is recorded.
; ---------------------------------------------------------------------------
_iocfat_memo_add::
	ld	a,(_iocfat_list_path_storage)
	or	a
	ret	z
	ld	a,(_iocfat_memo_count)
	cp	#MEMO_ENTRIES
	ret	nc
	push	hl
	ld	de,#DIRENT_NAMELEN
	add	hl,de
	ld	c,(hl)
	inc	hl
	ld	a,(hl)
	or	a
	jr	nz,memo_add_skip	; name_length >= 256
	ld	a,c
	cp	#MEMO_NAME + 1
	jr	nc,memo_add_skip	; longer than an 8.3 name
	ld	a,(_iocfat_memo_count)
	call	memo_entry
	ex	de,hl			; DE = entry
	pop	hl			; HL = file
	push	hl
	push	de
	ld	b,#0			; BC = name_length
	push	bc
	ld	bc,#DIRENT_NAME
	add	hl,bc
	pop	bc
	ldir				; the name and its NUL
	pop	de			; DE = entry
	pop	hl			; HL = file
	ld	a,(hl)			; type
	push	hl
	ld	hl,#MEMO_TYPE
	add	hl,de
	ld	(hl),a
	inc	hl
	ex	de,hl			; DE = entry size field
	pop	hl
	ld	bc,#DIRENT_SIZE
	add	hl,bc
	ldi
	ldi
	ldi
	ldi
	ld	hl,#_iocfat_memo_count
	inc	(hl)
	ret
memo_add_skip:
	pop	hl
	ret

; ---------------------------------------------------------------------------
; uint8_t iocfat_memo_stat(const char *path, struct kfs_stat *out);
;   HL = path, DE = out; A = 1 with out->type and out->size set (the entry
;   consumed), else 0.  The path's parent (without its '/', except the
;   root's own) must be the listed path, and its leaf an entry's name.
;   The caller has checked the media generation.
; ---------------------------------------------------------------------------
_iocfat_memo_stat::
	ld	a,(_iocfat_memo_count)
	or	a
	ret	z
	push	de			; out
	; BC = the leaf: past the last '/'
	ld	b,h
	ld	c,l
	push	hl
memo_scan:
	ld	a,(hl)
	or	a
	jr	z,memo_scanned
	inc	hl
	cp	#'/'
	jr	nz,memo_scan
	ld	b,h
	ld	c,l
	jr	memo_scan
memo_scanned:
	pop	hl			; HL = path
	ld	a,(bc)
	or	a
	jr	z,memo_miss		; no leaf
	; E = the parent's length
	ld	a,c
	sub	l
	cp	#2
	jr	c,memo_parent
	dec	a
memo_parent:
	or	a
	jr	z,memo_miss
	push	bc			; leaf
	ld	b,a
	ld	de,#_iocfat_list_path_storage
memo_parent_loop:
	ld	a,(de)
	cp	(hl)
	jr	nz,memo_miss_leaf
	or	a
	jr	z,memo_miss_leaf	; the listed path is shorter
	inc	hl
	inc	de
	djnz	memo_parent_loop
	ld	a,(de)
	or	a
	jr	nz,memo_miss_leaf	; ...or longer
	pop	bc			; BC = leaf
	ld	a,(_iocfat_memo_count)
	ld	d,a			; D = entries left
	ld	hl,#memo_table
memo_entry_loop:
	push	hl
	ld	a,l
	add	a,#MEMO_TYPE
	ld	l,a
	jr	nc,memo_type_hi
	inc	h
memo_type_hi:
	ld	a,(hl)
	pop	hl
	or	a
	jr	z,memo_next		; consumed
	push	hl
	push	bc
memo_cmp:
	ld	a,(bc)
	cp	(hl)
	jr	nz,memo_cmp_no
	or	a
	jr	z,memo_hit
	inc	hl
	inc	bc
	jr	memo_cmp
memo_cmp_no:
	pop	bc
	pop	hl
memo_next:
	ld	a,l
	add	a,#MEMO_BYTES
	ld	l,a
	jr	nc,memo_next_hi
	inc	h
memo_next_hi:
	dec	d
	jr	nz,memo_entry_loop
	jr	memo_miss
memo_hit:
	pop	bc
	pop	hl			; HL = entry
	pop	de			; DE = out
	ld	bc,#MEMO_TYPE
	add	hl,bc
	ld	a,(hl)
	ld	(de),a			; out->type
	ld	(hl),#0			; consumed
	inc	hl
	inc	de
	inc	de
	inc	de
	inc	de			; out->size (STAT_SIZE)
	ldi
	ldi
	ldi
	ldi
	ld	a,#1
	ret
memo_miss_leaf:
	pop	bc
memo_miss:
	pop	de
	xor	a
	ret
