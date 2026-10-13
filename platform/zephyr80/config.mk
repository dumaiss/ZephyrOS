# -----------------------------------------------------------------------------
# Zephyr-80 platform build configuration (declarative only).
#
# Included by the top-level Makefile.  This file names the platform's build
# include paths and the declarative constant files consumed by the build and
# the host layout tools.  It must never list or own kernel, boot, driver, or
# test implementation sources -- only platform configuration values.
# -----------------------------------------------------------------------------

KOS_PLATFORM     := zephyr80
KOS_PLATFORM_DIR := platform/zephyr80

# Bank-7 residency budget.  Kernel and resident-module
# C code is compiled for size: the kernel executive, the ROMFS/VFS core and
# every boot MOD share the single 2000h-DEFFh bank, so the size preference
# and a raised register-allocation budget buy back bytes.  User programs and
# EXE images are not bank-7 resident and keep their own flags.
KOS_SDCC_OPT := --opt-code-size --max-allocs-per-node 50000

# SDCC / sdasz80 include search paths.  $(BUILD) is provided by the
# including Makefile (the generated catalog headers live there).  abi/
# holds the public native ABI headers (abi/kos/...) shared by the kernel,
# modules and native user programs; kernel and module code includes them
# as "kos/...".  Each audience has its own roots: the nucleus set below plus `-I .` for the kernel/<part>
# private interfaces (the top-level Makefile's CFLAGS), and the driver
# set of DRV_CFLAGS/MOD_CFLAGS, which never gets `.`.
KOS_PLATFORM_CFLAGS  = -I abi -I kernel/include -I drivers/include \
                       -I formats/include -I platform/zephyr80/public \
                       -I platform/zephyr80/private -I $(BUILD)
KOS_PLATFORM_ASFLAGS = -Iabi -Iplatform/zephyr80/public \
                       -Iplatform/zephyr80/private

# Declarative constant files (C / assembly pairs, parity-tested).
KOS_PLATFORM_MEMORY_INC := $(KOS_PLATFORM_DIR)/private/memory.inc
KOS_PLATFORM_IRQ_INC    := $(KOS_PLATFORM_DIR)/private/irq.inc
