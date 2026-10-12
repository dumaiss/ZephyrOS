# KonaOS SDK examples (work package 49c).
#
# Build with `make` where this directory sits next to the unpacked SDK
# (KOS_SDK_ROOT names the SDK): every example is one exe_program line
# from the SDK's mk/kos.mk, and the packed programs land in
# build/user/<name>.exe.  The same sources are built into the OS tree's
# helloworld demo image (user/helloworld/Makefile), so an example that
# breaks fails the normal `make check` run too.

KOS_SDK_ROOT ?= ..
include $(KOS_SDK_ROOT)/mk/kos.mk

$(eval $(call exe_program,hello,.,hello.c,STACK=256))

# The far-code example: fardemo_far.c is compiled into `_FAR1` at 8000h
# with its own bank, so the `__banked` entry is called as plain C
# through the SDK's trampoline (work package 49b).
$(eval $(call exe_program,fardemo,.,fardemo.c,\
	FAR1=fardemo_far.c,STACK=256))

$(eval $(call exe_program,files,.,files.c,STACK=256))

# `make` packs every program listed here; each declaration is a target of
# its own name too (`make fardemo`).
.DEFAULT_GOAL := all
all: hello fardemo files
