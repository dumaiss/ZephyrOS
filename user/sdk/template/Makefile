# KonaOS program Makefile: the SDK's smallest program.
#
# Build with `make` where this directory sits next to the unpacked SDK
# (KOS_SDK_ROOT names the SDK).  The packed program lands in
# build/user/hello.exe: copy it to the card and run it from the monitor
# with `X /fs/hello.exe`, or from the shell with `hello`.
#
# A new program is one copy of this directory and one exe_program line:
# see the SDK's docs/sdk.md.

KOS_SDK_ROOT ?= ..
include $(KOS_SDK_ROOT)/mk/kos.mk

$(eval $(call exe_program,hello,.,hello.c,STACK=256))

# `make` packs every program listed here; each declaration is a target of
# its own name too (`make hello`).
.DEFAULT_GOAL := all
all: hello
