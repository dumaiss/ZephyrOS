; KonaOS M2 IRQ acceptance constants (assembly visible).
; Value-identical with tests/target/include/irq_selftest.h (parity-tested).

; Error code from abi/kos/errno.h (kept in sync by the host test).
KOS_IST_ENOSYS           = 0x01

KOS_IST_CALLER_BASE      = 0x1000
KOS_IST_TEST_STACK       = 0x4000
KOS_IST_SYSCALL_NUM      = 0x55
KOS_IST_ARGBLOCK         = 0x0800

KOS_IST_BC_SENTINEL      = 0xB1C2
KOS_IST_DE_SENTINEL      = 0xD3E4
KOS_IST_HL_SENTINEL      = 0xA5F6
KOS_IST_IX_SENTINEL      = 0x1978
KOS_IST_IY_SENTINEL      = 0x1B9A
KOS_IST_A_SENTINEL       = 0x55

KOS_IST_ALT_A_SENTINEL   = 0xAA
KOS_IST_ALT_BC_SENTINEL  = 0x1234
KOS_IST_ALT_DE_SENTINEL  = 0x5678
KOS_IST_ALT_HL_SENTINEL  = 0x9ABC

KOS_IST_RESULT_ARMED     = 0xFF

KOS_IST_OK               = 0x00
KOS_IST_FAIL_TIMEOUT     = 0x01
KOS_IST_FAIL_REPEAT      = 0x02
KOS_IST_FAIL_BC          = 0x03
KOS_IST_FAIL_DE          = 0x04
KOS_IST_FAIL_HL          = 0x05
KOS_IST_FAIL_IX          = 0x06
KOS_IST_FAIL_IY          = 0x07
KOS_IST_FAIL_AF          = 0x08
KOS_IST_FAIL_SP          = 0x09
KOS_IST_FAIL_MODE        = 0x0A
KOS_IST_FAIL_BANK        = 0x0B
KOS_IST_FAIL_ALT_AF      = 0x0C
KOS_IST_FAIL_ALT_BC      = 0x0D
KOS_IST_FAIL_ALT_DE      = 0x0E
KOS_IST_FAIL_ALT_HL      = 0x0F
KOS_IST_FAIL_IFF         = 0x10
KOS_IST_FAIL_SYSCALL     = 0x11
KOS_IST_FAIL_EVIDENCE    = 0x12
KOS_IST_FAIL_COUNTERS    = 0x13
KOS_IST_FAIL_UNCLAIMED   = 0x14
KOS_IST_FAIL_REGISTRY    = 0x15
