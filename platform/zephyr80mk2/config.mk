# -----------------------------------------------------------------------------
# Zephyr-80 Mk2 platform build configuration (declarative only).
#
# Included by the top-level Makefile beside platform/zephyr80/config.mk.
# The Mk2 CPU board replaces the Mk1 banking latch with the LS612 DAT
# mapper (architecture 2.7); the peripheral cards are the Mk1 board's.
# This file names the Mk2 include roots only.
# -----------------------------------------------------------------------------

KOS_MK2_PLATFORM_DIR := platform/zephyr80mk2

KOS_MK2_PLATFORM_ASFLAGS = -Iabi -Iplatform/zephyr80mk2/public \
                           -Iplatform/zephyr80mk2/private
KOS_MK2_PLATFORM_CFLAGS  = -I abi -I kernel/include -I drivers/include \
                           -I formats/include -I platform/zephyr80mk2/public \
                           -I platform/zephyr80mk2/private
