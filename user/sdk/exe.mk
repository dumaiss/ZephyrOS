# -----------------------------------------------------------------------------
# user/sdk/exe.mk - the in-tree wrapper around the program rules
#.
#
# The generic `exe_program` rule lives in mk/kos.mk, the file the packaged
# SDK also ships, with no OS-tree paths of its own.  This wrapper points
# its roots at the OS tree and builds libkos (and the shared runtime
# object) from user/sdk/src, so an in-tree program and an SDK-built
# program go through exactly the same rule:
#
#     $(eval $(call exe_program,NAME,DIR,SOURCES[,OPT=VALUE ...]))
#
# mk/kos.mk documents the options (STACK, STAGE, FAR1..FAR4, SEGMENTS,
# INCLUDES, ...).  The archive and the link scripts depend on both rule
# files, so editing either relinks every program.
# -----------------------------------------------------------------------------

KOS_BUILD    := $(BUILD)
KOS_PYTHON   := $(PYTHON)
KOS_TOOLS    := tools
KOS_SDK_ROOT := user/sdk
KOS_INCLUDE  := $(KOS_SDK_ROOT)/include
EXE_ABI_ROOT := abi
KOS_MODULES  := $(MODULES)
KOS_RULES    := $(KOS_SDK_ROOT)/exe.mk mk/kos.mk

EXE_SDK_BUILD := $(KOS_BUILD)/user/sdk
EXE_CRT0      := $(EXE_SDK_BUILD)/crt0.rel
EXE_RUNTIME   := $(EXE_SDK_BUILD)/runtime.rel
EXE_LIBKOS    := $(EXE_SDK_BUILD)/libkos.lib
KOS_SDK_SRC   := $(KOS_SDK_ROOT)/src

include mk/kos.mk

# The installed headers are prerequisites: a change to an ABI or SDK
# header (a struct layout, an inline constant such as the lookup cache
# depth) must rebuild the archive members, or every program would keep
# linking the previous layout.
$(EXE_SDK_BUILD)/%.rel: $(KOS_SDK_SRC)/%.c $(EXE_HEADERS)
	@mkdir -p $(dir $@)
	$(SDCC) $(EXE_CFLAGS) -c $< -o $@

$(EXE_SDK_BUILD)/%.rel: $(KOS_SDK_SRC)/%.asm $(EXE_HEADERS)
	@mkdir -p $(dir $@)
	$(SDASZ80) $(EXE_ASFLAGS) -o $@ $<

$(EXE_CRT0): $(KOS_SDK_SRC)/crt0.asm
	@mkdir -p $(dir $@)
	$(SDASZ80) $(EXE_ASFLAGS) -o $@ $<

# The shared runtime object owns the fixed low-data area and errno mapper.
# It is linked explicitly for every program, including programs that pull
# no archive member needing a gate block.
$(EXE_RUNTIME): $(KOS_SDK_SRC)/runtime.asm
	@mkdir -p $(dir $@)
	$(SDASZ80) $(EXE_ASFLAGS) -o $@ $<

KOS_SDK_C_SRC := args.c fs.c ipc.c exec.c console_io.c \
    xmem.c seg.c xload.c \
    console_proto.c \
    klibc/strlen.c klibc/strcmp.c klibc/strncmp.c klibc/strchr.c \
    klibc/strrchr.c klibc/memcpy.c klibc/memmove.c klibc/memset.c \
    klibc/memcmp.c klibc/strlcpy.c klibc/strlcat.c \
    klibc/utoa.c klibc/ultoa.c klibc/itoa.c klibc/ltoa.c \
    klibc/printf.c klibc/sdcc_arith.c klibc/sdcc_arith_signed.c
KOS_SDK_ASM_SRC := gate.asm ipcgate.asm diaggate.asm nsgate.asm \
    xmemgate.asm bcall.asm sdcc_calls.asm \
    klibc/sdcc_arith_entries.asm klibc/sdcc_arith_signed_entries.asm

KOS_SDK_C_OBJS   := $(addprefix $(EXE_SDK_BUILD)/,$(patsubst %.c,%.rel,$(KOS_SDK_C_SRC)))
KOS_SDK_ASM_OBJS := $(addprefix $(EXE_SDK_BUILD)/,$(patsubst %.asm,%.rel,$(KOS_SDK_ASM_SRC)))

# The rule file itself is a prerequisite: editing the source list or the
# flags must re-archive (the recipe starts from scratch) and relink every
# program, never leave a stale member behind.
$(EXE_LIBKOS): $(KOS_SDK_C_OBJS) $(KOS_SDK_ASM_OBJS) $(KOS_RULES)
	@mkdir -p $(dir $@)
	rm -f $@
	sdar -rc $@ $(KOS_SDK_C_OBJS) $(KOS_SDK_ASM_OBJS)

# The archive the packaged SDK ships: the same members
# with the shared runtime appended, because the SDK's lib/ holds only
# libkos.lib and crt0.rel and a program pulls the runtime through the
# archive.  The members in front keep the archive's member order, so the
# runtime's gate blocks are found after the wrappers that name them.
# `-D` zeroes the members' dates and ids: the shipped archive is a
# distribution artifact, so repacking unchanged sources reproduces it
# byte for byte (unlike the in-tree libkos, whose members carry their
# build's dates, `sdar`'s default).
EXE_SDK_RELEASE_LIB := $(EXE_SDK_BUILD)/libkos-sdk.lib
$(EXE_SDK_RELEASE_LIB): $(KOS_SDK_C_OBJS) $(KOS_SDK_ASM_OBJS) $(EXE_RUNTIME) \
		$(KOS_RULES)
	@mkdir -p $(dir $@)
	rm -f $@
	sdar -rcD $@ $(KOS_SDK_C_OBJS) $(KOS_SDK_ASM_OBJS) $(EXE_RUNTIME)
