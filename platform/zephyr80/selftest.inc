; ZephyrOS M1 syscall-gate self-test constants (assembly visible).
; Value-identical with include/zephyros/selftest.h (parity-tested).

; Error code from include/zephyros/errno.h (kept in sync by the host test).
ZOS_ENOSYS              = 0x01

ZOS_ST_CALLER_BASE      = 0x1000
ZOS_ST_TEST_STACK       = 0x4000
ZOS_ST_SYSCALL_NUM      = 0x55
ZOS_ST_ARGBLOCK         = 0x0800

ZOS_ST_BC_SENTINEL      = 0xB0BC
ZOS_ST_DE_SENTINEL      = 0xD0DE
ZOS_ST_IX_SENTINEL      = 0x19A0
ZOS_ST_IY_SENTINEL      = 0x1BA1

ZOS_ST_OK               = 0x00
ZOS_ST_FAIL_CARRY       = 0x01
ZOS_ST_FAIL_ERRNO       = 0x02
ZOS_ST_FAIL_RESULT      = 0x03
ZOS_ST_FAIL_BC          = 0x04
ZOS_ST_FAIL_DE          = 0x05
ZOS_ST_FAIL_IX          = 0x06
ZOS_ST_FAIL_IY          = 0x07
ZOS_ST_FAIL_SP          = 0x08
ZOS_ST_FAIL_MODE        = 0x09
ZOS_ST_FAIL_BANK        = 0x0A
