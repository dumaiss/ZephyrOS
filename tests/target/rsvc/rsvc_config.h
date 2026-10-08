/* ZephyrOS resident-service spike: transport configuration (test only).
 *
 * The resident service bank is mapped into the mode-11 window with one
 * page field: every service lives in one domain at a time and
 * service -> service calls never change the mapping.  The spike runs at
 * the revision-13 6000h split, so the window is 6000h-DFFFh and the
 * fixed core is 2000h-5FFFh; the services themselves are relocated at
 * 8000h inside that window.
 */

#ifndef ZEPHYROS_TARGET_RSVC_CONFIG_H
#define ZEPHYROS_TARGET_RSVC_CONFIG_H

#define ZOS_RSVC_SPIKE 1

#define ZOS_RSVC_MAX_NESTING 4u

/* Page 1 shows bank 6 over the whole 32 KiB window. */
#define ZOS_RSVC_PAGE_BITS 0x20u
#define ZOS_RSVC_DOMAIN_BASE 0x8000u
#define ZOS_RSVC_DOMAIN_SIZE 0x6000u

/* The deepest chain (client -> vfs -> fat -> ioc) saturates 256 bytes, so
 * the spike runs 384: the largest size that still fits the common ceiling
 * (limit 2560, usage with this size 2488). */
#define ZOS_RSVC_SERVICE_STACK_SIZE 384u
#define ZOS_RSVC_DESC_MAX 4096u

#endif /* ZEPHYROS_TARGET_RSVC_CONFIG_H */
