; KonaOS syscall-gate acceptance constants (assembly visible).
; Value-identical with tests/target/include/syscall_selftest.h (parity-tested).

; Error code from abi/kos/errno.h (kept in sync by the host test).
KOS_ENOSYS              = 0x01

KOS_ST_CALLER_BASE      = 0x1000
KOS_ST_TEST_STACK       = 0x4000
KOS_ST_SYSCALL_NUM      = 0x55
KOS_ST_ARGBLOCK         = 0x0800

KOS_ST_BC_SENTINEL      = 0xB0BC
KOS_ST_DE_SENTINEL      = 0xD0DE
KOS_ST_IX_SENTINEL      = 0x19A0
KOS_ST_IY_SENTINEL      = 0x1BA1

KOS_ST_OK               = 0x00
KOS_ST_FAIL_CARRY       = 0x01
KOS_ST_FAIL_ERRNO       = 0x02
KOS_ST_FAIL_RESULT      = 0x03
KOS_ST_FAIL_BC          = 0x04
KOS_ST_FAIL_DE          = 0x05
KOS_ST_FAIL_IX          = 0x06
KOS_ST_FAIL_IY          = 0x07
KOS_ST_FAIL_SP          = 0x08
KOS_ST_FAIL_MODE        = 0x09
KOS_ST_FAIL_BANK        = 0x0A
