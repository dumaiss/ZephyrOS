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
# as "zos/..." (work/28c-m9-user-sdk/WORK.md).
ZOS_PLATFORM_CFLAGS  = -I abi -I include -I platform -I $(BUILD)
ZOS_PLATFORM_ASFLAGS = -Iabi -Iplatform

# Declarative constant files (C / assembly pairs, parity-tested).
ZOS_PLATFORM_MEMORY_INC := $(ZOS_PLATFORM_DIR)/memory.inc
ZOS_PLATFORM_IRQ_INC    := $(ZOS_PLATFORM_DIR)/irq.inc
