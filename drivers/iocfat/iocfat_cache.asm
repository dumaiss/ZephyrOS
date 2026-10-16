; -----------------------------------------------------------------------------
; The IOC-FAT block cache's storage: BSS in this driver's bulk segment, which
; the packer places in bank 6's boot/bulk tier (0000h-5FFFh) and the loader
; zeroes and registers as the driver's bulk extent.  The driver never
; addresses it: it reaches the extent only through the nucleus's bulk_read
; and bulk_write, by offset.  Layout (iocfat.c, IOCFAT_CACHE_*): 32 block
; slots of 512 bytes, then 8 file-path records of 128 bytes.
; -----------------------------------------------------------------------------

	.module iocfat_cache

	.area _FSCACHE

iocfat_cache_extent:
	.ds	32 * 512 + 8 * 128
