# Out-of-tree TUI program: copied beside an unpacked SDK by
# tests/test_sdk_package.py.  The SDK must build it with LIBS=tui and
# without any path back into the OS tree.

KOS_SDK_ROOT ?= ..
include $(KOS_SDK_ROOT)/mk/kos.mk

$(eval $(call exe_program,tui_sdkprobe,.,out_of_tree.c,STACK=256,LIBS=tui))

.DEFAULT_GOAL := all
all: tui_sdkprobe
