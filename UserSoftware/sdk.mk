# The SDK an application builds against, chosen by board.  Every
# application Makefile includes this file before its SDK bootstrap:
#
#     make                  the Mk2 SDK (the default board)
#     make BOARD=mk1        the Mk1 SDK
#
# KOS_SDK_ARCHIVE and KOS_SDK_ROOT can still be given on the command line
# for an SDK kept elsewhere.  BOARD is exported: the applications' replay
# tests boot the production image of the same board.  An application tree
# is built for one board at a time: the bootstrap records the board in
# build/.board, and a build for the other one is refused until `make clean`.

BOARD ?= mk2
export BOARD

ifeq ($(BOARD),mk2)
KOS_SDK_ARCHIVE ?= ../../build-mk2/sys/kos-sdk-0.2.0-mk2.tar.gz
KOS_SDK_ROOT ?= build/kos-sdk-0.2.0-mk2
else ifeq ($(BOARD),mk1)
KOS_SDK_ARCHIVE ?= ../../build/kos-sdk-0.2.0.tar.gz
KOS_SDK_ROOT ?= build/kos-sdk-0.2.0
else
$(error BOARD must be mk2 or mk1, not "$(BOARD)")
endif

KOS_BUILT_BOARD := $(shell cat build/.board 2>/dev/null)
ifneq ($(KOS_BUILT_BOARD),)
ifneq ($(KOS_BUILT_BOARD),$(BOARD))
ifneq ($(MAKECMDGOALS),clean)
$(error build/ holds a BOARD=$(KOS_BUILT_BOARD) build; make clean first)
endif
endif
endif
