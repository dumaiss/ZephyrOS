/* ZephyrOS public fixed-width integer types (native ABI).
 *
 * Freestanding: these do not depend on any hosted header.  They are defined
 * in terms of the C types the SDCC Z80 compiler provides.  This is the one
 * definition shared by the kernel, modules and native user programs; the
 * kernel includes it as "zos/types.h" (see docs/sdk.md).
 */

#ifndef ZOS_TYPES_H
#define ZOS_TYPES_H

typedef unsigned char       uint8_t;
typedef unsigned int        uint16_t;
typedef unsigned long       uint32_t;

typedef signed char         int8_t;
typedef signed int          int16_t;
typedef signed long         int32_t;

/* Short aliases used by ZephyrOS kernel code. */
typedef uint8_t             u8;
typedef uint16_t            u16;
typedef uint32_t            u32;

typedef int8_t              s8;
typedef int16_t             s16;
typedef int32_t             s32;

#endif /* ZOS_TYPES_H */
