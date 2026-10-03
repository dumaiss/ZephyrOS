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

# SDCC / sdasz80 include search paths.  $(BUILD) is provided by the
# including Makefile (the generated catalog headers live there).
ZOS_PLATFORM_CFLAGS  = -I include -I platform -I $(BUILD)
ZOS_PLATFORM_ASFLAGS = -Iplatform

# Declarative constant files (C / assembly pairs, parity-tested).
ZOS_PLATFORM_MEMORY_INC := $(ZOS_PLATFORM_DIR)/memory.inc
ZOS_PLATFORM_IRQ_INC    := $(ZOS_PLATFORM_DIR)/irq.inc
