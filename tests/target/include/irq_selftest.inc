; ZephyrOS M2 IRQ acceptance constants (assembly visible).
; Value-identical with tests/target/include/irq_selftest.h (parity-tested).

; Error code from include/zephyros/errno.h (kept in sync by the host test).
ZOS_IST_ENOSYS           = 0x01

ZOS_IST_CALLER_BASE      = 0x1000
ZOS_IST_TEST_STACK       = 0x4000
ZOS_IST_SYSCALL_NUM      = 0x55
ZOS_IST_ARGBLOCK         = 0x0800

ZOS_IST_BC_SENTINEL      = 0xB1C2
ZOS_IST_DE_SENTINEL      = 0xD3E4
ZOS_IST_HL_SENTINEL      = 0xA5F6
ZOS_IST_IX_SENTINEL      = 0x1978
ZOS_IST_IY_SENTINEL      = 0x1B9A
ZOS_IST_A_SENTINEL       = 0x55

ZOS_IST_ALT_A_SENTINEL   = 0xAA
ZOS_IST_ALT_BC_SENTINEL  = 0x1234
ZOS_IST_ALT_DE_SENTINEL  = 0x5678
ZOS_IST_ALT_HL_SENTINEL  = 0x9ABC

ZOS_IST_RESULT_ARMED     = 0xFF

ZOS_IST_OK               = 0x00
ZOS_IST_FAIL_TIMEOUT     = 0x01
ZOS_IST_FAIL_REPEAT      = 0x02
ZOS_IST_FAIL_BC          = 0x03
ZOS_IST_FAIL_DE          = 0x04
ZOS_IST_FAIL_HL          = 0x05
ZOS_IST_FAIL_IX          = 0x06
ZOS_IST_FAIL_IY          = 0x07
ZOS_IST_FAIL_AF          = 0x08
ZOS_IST_FAIL_SP          = 0x09
ZOS_IST_FAIL_MODE        = 0x0A
ZOS_IST_FAIL_BANK        = 0x0B
ZOS_IST_FAIL_ALT_AF      = 0x0C
ZOS_IST_FAIL_ALT_BC      = 0x0D
ZOS_IST_FAIL_ALT_DE      = 0x0E
ZOS_IST_FAIL_ALT_HL      = 0x0F
ZOS_IST_FAIL_IFF         = 0x10
ZOS_IST_FAIL_SYSCALL     = 0x11
ZOS_IST_FAIL_EVIDENCE    = 0x12
ZOS_IST_FAIL_COUNTERS    = 0x13
ZOS_IST_FAIL_UNCLAIMED   = 0x14
ZOS_IST_FAIL_REGISTRY    = 0x15
