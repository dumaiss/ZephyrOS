# -----------------------------------------------------------------------------
# user/sdk/zex.mk - the generic ZEX program rule (work/28c-m9-user-sdk).
#
# Include this file from the top-level Makefile after BUILD/SDCC/... are
# set, then declare each program with one line:
#
#     $(eval $(call zex_program,NAME,DIR,SOURCES[,OPT=VALUE ...]))
#
# SOURCES are relative to DIR; .c files go through SDCC and .asm files
# through sdasz80.  The rule links crt0 (unless NO_CRT0=1) and libzos,
# generates the ZEX manifest from the link map, and packs and validates
# build/user/NAME.zex.
#
# Options (defaults in parentheses):
#   STACK=N             child stack bytes (256)
#   SCRATCH=SYM:SIZE    manifest scratch pin (zos_scratch:16)
#   ENTRY=SYMBOL        ZEX entry symbol (zos_entry)
#   NO_CRT0=1           do not link crt0; ENTRY names the program's own
#                       entry object (used by the raw-return test child)
#   LOWBASE=ADDR        SDK gate-visible data base (0x0100)
#   CODEBASE=ADDR       program code base (0x0144)
# -----------------------------------------------------------------------------

ZEX_USER_ROOT   := $(BUILD)/user
ZEX_SDK_BUILD   := $(ZEX_USER_ROOT)/sdk
ZEX_CRT0        := $(ZEX_SDK_BUILD)/crt0.rel
ZEX_RUNTIME     := $(ZEX_SDK_BUILD)/runtime.rel
ZEX_LIBZOS      := $(ZEX_SDK_BUILD)/libzos.lib
ZEX_SDK_LOW_SIZE := 93
# The program code base the ordinary layout uses: the SDK's gate-visible
# data area (ZEX_SDK_LOW_SIZE bytes at LOWBASE) plus the low window base.
# A program that reserves a caller buffer or links a larger SDK data area
# passes CODEBASE explicitly.
ZEX_DEFAULT_CODEBASE := 0x015D
ZOS_SDK_ROOT    := user/sdk
ZOS_SDK_INCLUDE := $(ZOS_SDK_ROOT)/include
ZOS_SDK_SRC     := $(ZOS_SDK_ROOT)/src

# Exactly the public ABI and the SDK include root; a program never sees
# include/, platform/ or kernel/ (docs/sdk.md, tests/test_source_layout.py).
ZEX_CFLAGS  := -mz80 -c --nostdinc --Werror -I abi -I $(ZOS_SDK_INCLUDE)

# Header prerequisites (work package 38; work package 37 finding 2).  SDCC
# emits no dependency files through this rule, and the SDK's include
# surface is exactly the public ABI plus the SDK include root, so every
# program object depends on that whole set (and on its own directory's
# headers): a stale program object can then never mask a real failure.
ZEX_HEADERS := $(wildcard abi/zos/*.h) $(wildcard $(ZOS_SDK_INCLUDE)/*.h)
ZEX_ASFLAGS := -Iabi

ZOS_SDK_C_SRC := args.c fs.c ipc.c exec.c console_io.c \
    console_service.c \
    zlibc/strlen.c zlibc/strcmp.c zlibc/strncmp.c zlibc/strchr.c \
    zlibc/strrchr.c zlibc/memcpy.c zlibc/memmove.c zlibc/memset.c \
    zlibc/memcmp.c zlibc/strlcpy.c zlibc/strlcat.c \
    zlibc/utoa.c zlibc/ultoa.c zlibc/itoa.c zlibc/ltoa.c \
    zlibc/printf.c zlibc/sdcc_arith.c zlibc/sdcc_arith_signed.c
ZOS_SDK_ASM_SRC := gate.asm ipcgate.asm diaggate.asm nsgate.asm \
    zlibc/sdcc_arith_entries.asm zlibc/sdcc_arith_signed_entries.asm

ZOS_SDK_C_OBJS   := $(addprefix $(ZEX_SDK_BUILD)/,$(patsubst %.c,%.rel,$(ZOS_SDK_C_SRC)))
ZOS_SDK_ASM_OBJS := $(addprefix $(ZEX_SDK_BUILD)/,$(patsubst %.asm,%.rel,$(ZOS_SDK_ASM_SRC)))

# The installed headers are prerequisites: a change to an ABI or SDK
# header (a struct layout, an inline constant such as the lookup cache
# depth) must rebuild the archive members, or every program would keep
# linking the previous layout.
$(ZEX_SDK_BUILD)/%.rel: $(ZOS_SDK_SRC)/%.c $(ZEX_HEADERS)
	@mkdir -p $(dir $@)
	$(SDCC) $(ZEX_CFLAGS) -c $< -o $@

$(ZEX_SDK_BUILD)/%.rel: $(ZOS_SDK_SRC)/%.asm $(ZEX_HEADERS)
	@mkdir -p $(dir $@)
	$(SDASZ80) $(ZEX_ASFLAGS) -o $@ $<

$(ZEX_CRT0): $(ZOS_SDK_SRC)/crt0.asm
	@mkdir -p $(dir $@)
	$(SDASZ80) $(ZEX_ASFLAGS) -o $@ $<

# The shared runtime object owns the fixed low-data area and errno mapper.
# It is linked explicitly for every program, including programs that pull
# no archive member needing a gate block.
$(ZEX_RUNTIME): $(ZOS_SDK_SRC)/runtime.asm
	@mkdir -p $(dir $@)
	$(SDASZ80) $(ZEX_ASFLAGS) -o $@ $<

# The rule file itself is a prerequisite: editing the source list or the
# flags must re-archive (the recipe starts from scratch) and relink every
# program, never leave a stale member behind.
$(ZEX_LIBZOS): $(ZOS_SDK_C_OBJS) $(ZOS_SDK_ASM_OBJS) $(ZOS_SDK_ROOT)/zex.mk
	@mkdir -p $(dir $@)
	rm -f $@
	sdar -rc $@ $(ZOS_SDK_C_OBJS) $(ZOS_SDK_ASM_OBJS)

# $(call zex_program,NAME,DIR,SOURCES,[OPT=VALUE ...])
define zex_program
$(1)_SRC    := $$(addprefix $(2)/,$(3))
$(1)_CSRC   := $$(filter %.c,$$($(1)_SRC))
$(1)_ASMSRC := $$(filter %.asm,$$($(1)_SRC))
$(1)_OBJDIR := $$(ZEX_USER_ROOT)/$(1)
$(1)_COBJS  := $$(addprefix $$($(1)_OBJDIR)/,$$(patsubst %.c,%.rel,$$(notdir $$($(1)_CSRC))))
$(1)_AOBJS  := $$(addprefix $$($(1)_OBJDIR)/,$$(patsubst %.asm,%.rel,$$(notdir $$($(1)_ASMSRC))))
$(1)_OBJS   := $$($(1)_COBJS) $$($(1)_AOBJS)
$(1)_HEADERS:= $$(ZEX_HEADERS) $$(wildcard $(2)/*.h)
$(1)_OPTS   := $(4) $(5) $(6) $(7) $(8) $(9)
$(1)_STACK  := $$(if $$(filter STACK=%,$$($(1)_OPTS)),$$(patsubst STACK=%,%,$$(filter STACK=%,$$($(1)_OPTS))),256)
$(1)_SCRATCH:= $$(if $$(filter SCRATCH=%,$$($(1)_OPTS)),$$(patsubst SCRATCH=%,%,$$(filter SCRATCH=%,$$($(1)_OPTS))),zos_scratch:16)
$(1)_SCRATCHARG := $$(if $$($(1)_SCRATCH), --scratch $$($(1)_SCRATCH),)
$(1)_ENTRY  := $$(if $$(filter ENTRY=%,$$($(1)_OPTS)),$$(patsubst ENTRY=%,%,$$(filter ENTRY=%,$$($(1)_OPTS))),zos_entry)
# The fixed SDK area is 68 bytes (_ZOS_SDK_DATA in runtime.asm), so the
# ordinary layout reserves 0100h-0143h and starts code at 0144h.  A program
# that reserves a caller buffer supplies both bases explicitly.
$(1)_LOWBASE := $$(if $$(filter LOWBASE=%,$$($(1)_OPTS)),$$(patsubst LOWBASE=%,%,$$(filter LOWBASE=%,$$($(1)_OPTS))),0x0100)
$(1)_CODEBASE := $$(if $$(filter CODEBASE=%,$$($(1)_OPTS)),$$(patsubst CODEBASE=%,%,$$(filter CODEBASE=%,$$($(1)_OPTS))),$$(ZEX_DEFAULT_CODEBASE))
$(1)_NOCRT0 := $$(if $$(filter NO_CRT0=1,$$($(1)_OPTS)),1,)
$(1)_STAGE  := $$(if $$(filter STAGE=1,$$($(1)_OPTS)),1,)
$(1)_INC    := $$(if $$(filter INCLUDES=%,$$($(1)_OPTS)),$$(patsubst INCLUDES=%,%,$$(filter INCLUDES=%,$$($(1)_OPTS))),)
$(1)_INCFLAGS := $$(patsubst %, -I%,$(subst :, ,$$($(1)_INC)))
$(1)_LINKOBJS := $$(if $$($(1)_NOCRT0),,$$(ZEX_CRT0)) $$(ZEX_RUNTIME) $$($(1)_OBJS)
$(1)_ZEX    := $$(ZEX_USER_ROOT)/$(1).zex

$$($(1)_OBJDIR):
	mkdir -p $$@

$$($(1)_COBJS): $$($(1)_OBJDIR)/%.rel: $(2)/%.c $$($(1)_HEADERS) \
		| $$($(1)_OBJDIR)
	$(SDCC) $(ZEX_CFLAGS) $$($(1)_INCFLAGS) -I $(2) -c $$< -o $$@

$$($(1)_AOBJS): $$($(1)_OBJDIR)/%.rel: $(2)/%.asm | $$($(1)_OBJDIR)
	$(SDASZ80) $(ZEX_ASFLAGS) $$($(1)_INCFLAGS) -I$(2) -o $$@ $$<

$$($(1)_OBJDIR)/$(1).lk: $$($(1)_LINKOBJS) $$(ZEX_LIBZOS) $(ZOS_SDK_ROOT)/zex.mk | $$($(1)_OBJDIR)
	{ \
	  printf '%s\n' '-n' '-m' '-w'; \
	  printf '%s\n' '-i $$($(1)_OBJDIR)/$(1).ihx'; \
	  printf '%s\n' '-b _ZOS_SDK_DATA = $$($(1)_LOWBASE)'; \
	  printf '%s\n' '-b _CODE = $$($(1)_CODEBASE)'; \
	  printf '%s\n' $$($(1)_LINKOBJS); \
	  printf '%s\n' '-l $$(ZEX_LIBZOS)'; \
	  printf '%s\n' '-e'; \
	} > $$@

$$($(1)_OBJDIR)/$(1).link: $$($(1)_OBJDIR)/$(1).lk
	$(SDLDZ80) -f $$<
	touch $$@

$$($(1)_OBJDIR)/$(1).json: $$($(1)_OBJDIR)/$(1).link
	$(PYTHON) tools/zexmanifest.py $$($(1)_OBJDIR)/$(1).map $$@ \
	    --name $(1).zex --entry $$($(1)_ENTRY) --stack $$($(1)_STACK) \
	    $$($(1)_SCRATCHARG)

$$($(1)_ZEX): $$($(1)_OBJDIR)/$(1).json tools/zexpack.py
	$(PYTHON) tools/zexpack.py pack $$($(1)_OBJDIR)/$(1).json $$@ \
	    --objdir $$($(1)_OBJDIR) --report $$($(1)_OBJDIR)/$(1).report.json
	$(PYTHON) tools/zexpack.py validate $$@

ifneq ($$($(1)_STAGE),)
# Recovery images consume their programs from $(MODULES); stage the SDK
# output there without a second build (work/28c-m9-user-sdk).
$$(MODULES)/$(1).zex: $$($(1)_ZEX) | $$(MODULES)
	cp $$< $$@
endif
endef
