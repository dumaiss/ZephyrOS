# -----------------------------------------------------------------------------
# Zephyr-80 platform build configuration (declarative only).
#
# Included by the top-level Makefile.  This file names the platform's build
# include paths and the declarative constant files consumed by the build and
# the host layout tools.  It must never list or own kernel, boot, driver, or
# test implementation sources -- only platform configuration values.
# -----------------------------------------------------------------------------

ZOS_PLATFORM     := zephyr80
ZOS_PLATFORM_DIR := platform/zephyr80

# Bank-7 residency budget (work package 25b).  Kernel and resident-module
# C code is compiled for size: the kernel executive, the ROMFS/VFS core and
# every boot ZMOD share the single 2000h-DEFFh bank, so the size preference
# and a raised register-allocation budget buy back bytes.  User programs and
# ZEX images are not bank-7 resident and keep their own flags.
ZOS_SDCC_OPT := --opt-code-size --max-allocs-per-node 50000

# SDCC / sdasz80 include search paths.  $(BUILD) is provided by the
# including Makefile (the generated catalog headers live there).  abi/
# holds the public native ABI headers (abi/zos/...) shared by the kernel,
# modules and native user programs; kernel and module code includes them
# as "zos/..." (work/28c-m9-user-sdk/WORK.md).  Package 46 T3 split the
# audience roots: the nucleus set below plus `-I .` for the kernel/<part>
# private interfaces (the top-level Makefile's CFLAGS), and the driver
# set of DRV_CFLAGS/MOD_CFLAGS, which never gets `.`.
ZOS_PLATFORM_CFLAGS  = -I abi -I kernel/include -I drivers/include \
                       -I formats/include -I platform/zephyr80/public \
                       -I platform/zephyr80/private -I $(BUILD)
ZOS_PLATFORM_ASFLAGS = -Iabi -Iplatform/zephyr80/public \
                       -Iplatform/zephyr80/private

# Declarative constant files (C / assembly pairs, parity-tested).
ZOS_PLATFORM_MEMORY_INC := $(ZOS_PLATFORM_DIR)/private/memory.inc
ZOS_PLATFORM_IRQ_INC    := $(ZOS_PLATFORM_DIR)/private/irq.inc
