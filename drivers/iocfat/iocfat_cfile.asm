; -----------------------------------------------------------------------------
; The IOC-FAT file records (iocfat.c, the block cache): a read-only open of
; a file that fits the cache finds or makes the record for its path, and
; the record's number (1..8) keys the file's cached blocks.  A record
; keeps the path's length, hash, the size the open reported and a use
; stamp; the path itself is in the bulk extent (IOCFAT_CACHE_PATHS_AT,
; 128 bytes per record), reached through the nucleus bulk copy.
;
; struct iocfat_cfile (9 bytes; pinned by the _Static_asserts in
; iocfat.c): length 0 (0 = free), hash 1..2, size 3..6, stamp 7..8.
; -----------------------------------------------------------------------------

	.module iocfat_cfile

	.globl _iocfat_cache_file
	.globl _iocfat_cache_drop_key
	.globl _iocfat_cfiles
	.globl _iocfat_pathbuf
	.globl _iocfat_pathbuf_address
	.globl _iocfat_ring_address
	.globl _iocfat_ring_key
	.globl _iocfat_api
	.globl _iocfat_clock
	.globl _kos_block_copy

CF_LENGTH      = 0
CF_HASH        = 1
CF_SIZE        = 3
CF_STAMP       = 7
CF_BYTES       = 9
CF_FILES       = 8		; IOCFAT_CACHE_FILES
CF_PATH        = 128		; IOCFAT_CACHE_PATH
CF_PATHS_AT_HI = 0x40		; IOCFAT_CACHE_PATHS_AT = 4000h
API_BULK_READ  = 8		; struct driver_api
API_BULK_WRITE = 10

	.area _DRIVER_IO
cf_length:
	.ds	1
cf_hash:
	.ds	2
cf_size:
	.ds	2			; -> the size the open reported
cf_index:
	.ds	1
cf_pick:
	.ds	1

	.area _CODE

; ---------------------------------------------------------------------------
; uint8_t iocfat_cache_file(const char *path, const uint32_t *size);
;   HL = path, DE = size.  The record for `path` as a read-only open saw it,
;   found or made: A = its key (1..8), or 0 when the path does not fit a
;   record (empty, or 128 bytes and longer) or the extent refuses it.  A
;   found record whose size changed forgets its blocks first; a made record
;   takes a free one, else the least recently used, whose blocks go.  The
;   candidates are compared byte for byte: the stored path is read into the
;   ring's first block, so the ring is given up first.  IX and IY are
;   preserved.
; ---------------------------------------------------------------------------
_iocfat_cache_file::
	push	ix
	push	iy
	ld	(cf_size),de
	; The length: 1..127 bytes.
	ld	b,#0
	push	hl
cf_len:
	ld	a,(hl)
	or	a
	jr	z,cf_len_done
	inc	hl
	inc	b
	ld	a,b
	cp	#CF_PATH
	jr	c,cf_len
	pop	hl
	jp	cf_none
cf_len_done:
	pop	hl
	ld	a,b
	or	a
	jp	z,cf_none
	ld	(cf_length),a
	; The path into the staging buffer.
	ex	de,hl			; DE = path
	ld	c,b
	ld	b,#0
	push	bc			; count (the callee removes it)
	ld	hl,(_iocfat_pathbuf)
	call	_kos_block_copy
	; hash = hash * 33 + byte, over the staged path.
	ld	hl,(_iocfat_pathbuf)
	ld	a,(cf_length)
	ld	b,a
	ld	de,#0
cf_hash_loop:
	push	hl
	ld	h,d
	ld	l,e
	add	hl,hl
	add	hl,hl
	add	hl,hl
	add	hl,hl
	add	hl,hl
	add	hl,de
	ex	de,hl
	pop	hl
	ld	a,(hl)
	add	a,e
	ld	e,a
	jr	nc,cf_hash_carry
	inc	d
cf_hash_carry:
	inc	hl
	djnz	cf_hash_loop
	ld	(cf_hash),de

	; Find: same length and hash, then the stored path byte for byte.
	xor	a
	ld	(cf_index),a
	ld	ix,(_iocfat_cfiles)
cf_find:
	ld	a,(cf_length)
	cp	CF_LENGTH(ix)
	jr	nz,cf_find_next
	ld	a,(cf_hash)
	cp	CF_HASH(ix)
	jr	nz,cf_find_next
	ld	a,(cf_hash+1)
	cp	CF_HASH+1(ix)
	jr	nz,cf_find_next
	xor	a
	ld	(_iocfat_ring_key),a	; the ring is given up
	ld	a,(cf_length)
	ld	c,a
	ld	b,#0
	push	bc
	ld	de,(_iocfat_ring_address)
	call	cf_path_at
	ld	a,#API_BULK_READ
	call	cf_api
	bit	7,d
	jr	nz,cf_find_next		; the extent refused: not this one
	ld	hl,(_iocfat_ring_address)
	ld	de,(_iocfat_pathbuf)
	ld	a,(cf_length)
	ld	b,a
cf_cmp:
	ld	a,(de)
	cp	(hl)
	jr	nz,cf_find_next
	inc	hl
	inc	de
	djnz	cf_cmp
	; Found.  A changed size forgets the record's blocks.
	call	cf_size_same
	jr	z,cf_found
	ld	a,(cf_index)
	inc	a
	call	_iocfat_cache_drop_key
	call	cf_size_store
cf_found:
	call	cf_stamp
	ld	a,(cf_index)
	inc	a
	jp	cf_return
cf_find_next:
	ld	de,#CF_BYTES
	add	ix,de
	ld	hl,#cf_index
	inc	(hl)
	ld	a,(hl)
	cp	#CF_FILES
	jr	c,cf_find

	; Make: a free record, else the least recently used one.
	xor	a
	ld	(cf_index),a
	ld	(cf_pick),a
	ld	bc,#0			; BC = the oldest age seen
	ld	ix,(_iocfat_cfiles)
cf_pick_loop:
	ld	a,CF_LENGTH(ix)
	or	a
	jr	z,cf_pick_free
	ld	hl,(_iocfat_clock)
	ld	e,CF_STAMP(ix)
	ld	d,CF_STAMP+1(ix)
	or	a
	sbc	hl,de			; HL = the record's age
	push	hl
	or	a
	sbc	hl,bc
	pop	hl
	jr	c,cf_pick_next
	jr	z,cf_pick_next
	ld	b,h
	ld	c,l
	ld	a,(cf_index)
	ld	(cf_pick),a
cf_pick_next:
	ld	de,#CF_BYTES
	add	ix,de
	ld	hl,#cf_index
	inc	(hl)
	ld	a,(hl)
	cp	#CF_FILES
	jr	c,cf_pick_loop
	jr	cf_pick_done
cf_pick_free:
	ld	a,(cf_index)
	ld	(cf_pick),a
cf_pick_done:
	ld	a,(cf_pick)
	ld	(cf_index),a
	inc	a
	call	_iocfat_cache_drop_key
	call	cf_rec
	ld	CF_LENGTH(ix),#0
	ld	a,(cf_length)
	ld	c,a
	ld	b,#0
	push	bc
	ld	de,(_iocfat_pathbuf_address)
	call	cf_path_at
	ld	a,#API_BULK_WRITE
	call	cf_api
	bit	7,d
	jr	nz,cf_none		; the extent refused the path
	call	cf_rec
	ld	a,(cf_length)
	ld	CF_LENGTH(ix),a
	ld	hl,(cf_hash)
	ld	CF_HASH(ix),l
	ld	CF_HASH+1(ix),h
	call	cf_size_store
	call	cf_stamp
	ld	a,(cf_index)
	inc	a
	jr	cf_return
cf_none:
	xor	a
cf_return:
	pop	iy
	pop	ix
	ret

; IX = record cf_index.  Destroys AF, DE, HL.
cf_rec:
	ld	a,(cf_index)
	ld	l,a
	ld	h,#0
	ld	d,h
	ld	e,l
	add	hl,hl
	add	hl,hl
	add	hl,hl
	add	hl,de			; index * 9
	ld	de,(_iocfat_cfiles)
	add	hl,de
	push	hl
	pop	ix
	ret

; HL = the extent offset of record cf_index's path.  Keeps DE.
cf_path_at:
	ld	a,(cf_index)
	ld	h,a
	ld	l,#0
	srl	h
	rr	l			; index * 128
	ld	a,h
	add	a,#CF_PATHS_AT_HI
	ld	h,a
	ret

; Tail-call the driver API entry at offset A with HL and DE, the caller
; having pushed the third argument (the entry removes it); DE = the result.
cf_api:
	push	hl
	ld	hl,(_iocfat_api)
	add	a,l
	ld	l,a
	jr	nc,cf_api_hi
	inc	h
cf_api_hi:
	ld	a,(hl)
	inc	hl
	ld	h,(hl)
	ld	l,a
	push	hl
	pop	iy
	pop	hl
	jp	(iy)

; Z when record IX's size is the open's.
cf_size_same:
	ld	hl,(cf_size)
	ld	a,(hl)
	cp	CF_SIZE(ix)
	ret	nz
	inc	hl
	ld	a,(hl)
	cp	CF_SIZE+1(ix)
	ret	nz
	inc	hl
	ld	a,(hl)
	cp	CF_SIZE+2(ix)
	ret	nz
	inc	hl
	ld	a,(hl)
	cp	CF_SIZE+3(ix)
	ret

; Record IX takes the open's size.
cf_size_store:
	ld	hl,(cf_size)
	ld	a,(hl)
	ld	CF_SIZE(ix),a
	inc	hl
	ld	a,(hl)
	ld	CF_SIZE+1(ix),a
	inc	hl
	ld	a,(hl)
	ld	CF_SIZE+2(ix),a
	inc	hl
	ld	a,(hl)
	ld	CF_SIZE+3(ix),a
	ret

; Record IX's stamp = ++iocfat_clock (iocfat_tick).
cf_stamp:
	ld	hl,(_iocfat_clock)
	inc	hl
	ld	(_iocfat_clock),hl
	ld	CF_STAMP(ix),l
	ld	CF_STAMP+1(ix),h
	ret
