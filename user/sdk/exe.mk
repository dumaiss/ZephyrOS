# -----------------------------------------------------------------------------
# user/sdk/exe.mk - the generic EXE program rule (work/28c-m9-user-sdk).
#
# Include this file from the top-level Makefile after BUILD/SDCC/... are
# set, then declare each program with one line:
#
#     $(eval $(call exe_program,NAME,DIR,SOURCES[,OPT=VALUE ...]))
#
# SOURCES are relative to DIR; .c files go through SDCC and .asm files
# through sdasz80.  The rule links crt0 (unless NO_CRT0=1) and libkos,
# generates the EXE manifest from the link map, and packs and validates
# build/user/NAME.exe.
#
# Options (defaults in parentheses):
#   STACK=N             child stack bytes (256)
#   SCRATCH=SYM:SIZE    manifest scratch pin (kos_scratch:16)
#   ENTRY=SYMBOL        EXE entry symbol (kos_entry)
#   NO_CRT0=1           do not link crt0; ENTRY names the program's own
#                       entry object (used by the raw-return test child)
#   LOWBASE=ADDR        SDK gate-visible data base (0x0100)
#   CODEBASE=ADDR       program code base (0x0144)
#   SEGMENTS=FILE       JSON list of EXE v2 auxiliary segment
#                       declarations (id/kind/input or area/bss_size); FILE
#                       is a prerequisite and its payload `input` names
#                       resolve against the program's object directory
#                       (package 44a)
#   FAR=SRC:SRC ...     far-code sources (relative to DIR) compiled into the
#                       _FAR_CODE area and linked at 8000h, executed in
#                       place through the aperture; a .c source is compiled
#                       with --codeseg FAR_CODE, a .asm source opens
#                       `.area _FAR_CODE` itself.  Without SEGMENTS= the
#                       rule declares one FAR_CODE segment (id 1) for the
#                       area, and the packer checks the 36.17 link rule
#                       (primary image, BSS and stack outside 8000h-BFFFh)
#                       (package 44b)
# -----------------------------------------------------------------------------

EXE_USER_ROOT   := $(BUILD)/user
EXE_SDK_BUILD   := $(EXE_USER_ROOT)/sdk
EXE_CRT0        := $(EXE_SDK_BUILD)/crt0.rel
EXE_RUNTIME     := $(EXE_SDK_BUILD)/runtime.rel
EXE_LIBKOS      := $(EXE_SDK_BUILD)/libkos.lib
EXE_SDK_LOW_SIZE := 93
# The program code base the ordinary layout uses: the SDK's gate-visible
# data area (EXE_SDK_LOW_SIZE bytes at LOWBASE) plus the low window base.
# A program that reserves a caller buffer or links a larger SDK data area
# passes CODEBASE explicitly.
EXE_DEFAULT_CODEBASE := 0x015D
KOS_SDK_ROOT    := user/sdk
KOS_SDK_INCLUDE := $(KOS_SDK_ROOT)/include
KOS_SDK_SRC     := $(KOS_SDK_ROOT)/src

# Exactly the public ABI and the SDK include root; a program never sees
# include/, platform/ or kernel/ (docs/sdk.md, tests/test_source_layout.py).
EXE_CFLAGS  := -mz80 -c --nostdinc --Werror -I abi -I $(KOS_SDK_INCLUDE)

# Header prerequisites (work package 38; work package 37 finding 2).  SDCC
# emits no dependency files through this rule, and the SDK's include
# surface is exactly the public ABI plus the SDK include root, so every
# program object depends on that whole set (and on its own directory's
# headers): a stale program object can then never mask a real failure.
EXE_HEADERS := $(wildcard abi/kos/*.h) $(wildcard $(KOS_SDK_INCLUDE)/*.h)
EXE_ASFLAGS := -Iabi

KOS_SDK_C_SRC := args.c fs.c ipc.c exec.c console_io.c \
    xmem.c seg.c \
    console_proto.c \
    klibc/strlen.c klibc/strcmp.c klibc/strncmp.c klibc/strchr.c \
    klibc/strrchr.c klibc/memcpy.c klibc/memmove.c klibc/memset.c \
    klibc/memcmp.c klibc/strlcpy.c klibc/strlcat.c \
    klibc/utoa.c klibc/ultoa.c klibc/itoa.c klibc/ltoa.c \
    klibc/printf.c klibc/sdcc_arith.c klibc/sdcc_arith_signed.c
KOS_SDK_ASM_SRC := gate.asm ipcgate.asm diaggate.asm nsgate.asm \
    xmemgate.asm sdcc_calls.asm \
    klibc/sdcc_arith_entries.asm klibc/sdcc_arith_signed_entries.asm

KOS_SDK_C_OBJS   := $(addprefix $(EXE_SDK_BUILD)/,$(patsubst %.c,%.rel,$(KOS_SDK_C_SRC)))
KOS_SDK_ASM_OBJS := $(addprefix $(EXE_SDK_BUILD)/,$(patsubst %.asm,%.rel,$(KOS_SDK_ASM_SRC)))

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

# The rule file itself is a prerequisite: editing the source list or the
# flags must re-archive (the recipe starts from scratch) and relink every
# program, never leave a stale member behind.
$(EXE_LIBKOS): $(KOS_SDK_C_OBJS) $(KOS_SDK_ASM_OBJS) $(KOS_SDK_ROOT)/exe.mk
	@mkdir -p $(dir $@)
	rm -f $@
	sdar -rc $@ $(KOS_SDK_C_OBJS) $(KOS_SDK_ASM_OBJS)

# $(call exe_program,NAME,DIR,SOURCES,[OPT=VALUE ...])
define exe_program
$(1)_SRC    := $$(addprefix $(2)/,$(3))
$(1)_CSRC   := $$(filter %.c,$$($(1)_SRC))
$(1)_ASMSRC := $$(filter %.asm,$$($(1)_SRC))
$(1)_OBJDIR := $$(EXE_USER_ROOT)/$(1)
$(1)_COBJS  := $$(addprefix $$($(1)_OBJDIR)/,$$(patsubst %.c,%.rel,$$(notdir $$($(1)_CSRC))))
$(1)_AOBJS  := $$(addprefix $$($(1)_OBJDIR)/,$$(patsubst %.asm,%.rel,$$(notdir $$($(1)_ASMSRC))))
$(1)_OBJS   := $$($(1)_COBJS) $$($(1)_AOBJS)
$(1)_HEADERS:= $$(EXE_HEADERS) $$(wildcard $(2)/*.h)
$(1)_OPTS   := $(4) $(5) $(6) $(7) $(8) $(9)
$(1)_STACK  := $$(if $$(filter STACK=%,$$($(1)_OPTS)),$$(patsubst STACK=%,%,$$(filter STACK=%,$$($(1)_OPTS))),256)
$(1)_SCRATCH:= $$(if $$(filter SCRATCH=%,$$($(1)_OPTS)),$$(patsubst SCRATCH=%,%,$$(filter SCRATCH=%,$$($(1)_OPTS))),kos_scratch:16)
$(1)_SCRATCHARG := $$(if $$($(1)_SCRATCH), --scratch $$($(1)_SCRATCH),)
$(1)_SEGMENTS := $$(if $$(filter SEGMENTS=%,$$($(1)_OPTS)),$$(patsubst SEGMENTS=%,%,$$(filter SEGMENTS=%,$$($(1)_OPTS))),)
$(1)_FAR    := $$(if $$(filter FAR=%,$$($(1)_OPTS)),$$(patsubst FAR=%,%,$$(filter FAR=%,$$($(1)_OPTS))),)
$(1)_FARSRC := $$(subst :, ,$$($(1)_FAR))
$(1)_FARCOBJS := $$(addprefix $$($(1)_OBJDIR)/,$$(patsubst %.c,%.rel,$$(notdir $$(filter %.c,$$($(1)_FARSRC)))))
$(1)_FARAOBJS := $$(addprefix $$($(1)_OBJDIR)/,$$(patsubst %.asm,%.rel,$$(notdir $$(filter %.asm,$$($(1)_FARSRC)))))
$(1)_FAROBJS  := $$($(1)_FARCOBJS) $$($(1)_FARAOBJS)
# Without SEGMENTS= the rule declares the one far-code segment the linked
# _FAR_CODE area becomes; a program with several far segments (or a far
# BSS) declares them itself through SEGMENTS=.
$(1)_FARDECL := $$(if $$($(1)_FAR),$$(if $$($(1)_SEGMENTS),,$$($(1)_OBJDIR)/$(1).far.json),)
$(1)_SEGARGSRC := $$(if $$($(1)_SEGMENTS),$$($(1)_SEGMENTS),$$($(1)_FARDECL))
$(1)_SEGMENTSARG := $$(if $$($(1)_SEGARGSRC), --segments $$($(1)_SEGARGSRC),)
$(1)_SEGMENTSDEP := $$(if $$($(1)_SEGARGSRC), $$($(1)_SEGARGSRC),)
$(1)_ENTRY  := $$(if $$(filter ENTRY=%,$$($(1)_OPTS)),$$(patsubst ENTRY=%,%,$$(filter ENTRY=%,$$($(1)_OPTS))),kos_entry)
# The fixed SDK area is 68 bytes (_KOS_SDK_DATA in runtime.asm), so the
# ordinary layout reserves 0100h-0143h and starts code at 0144h.  A program
# that reserves a caller buffer supplies both bases explicitly.
$(1)_LOWBASE := $$(if $$(filter LOWBASE=%,$$($(1)_OPTS)),$$(patsubst LOWBASE=%,%,$$(filter LOWBASE=%,$$($(1)_OPTS))),0x0100)
$(1)_CODEBASE := $$(if $$(filter CODEBASE=%,$$($(1)_OPTS)),$$(patsubst CODEBASE=%,%,$$(filter CODEBASE=%,$$($(1)_OPTS))),$$(EXE_DEFAULT_CODEBASE))
$(1)_NOCRT0 := $$(if $$(filter NO_CRT0=1,$$($(1)_OPTS)),1,)
$(1)_STAGE  := $$(if $$(filter STAGE=1,$$($(1)_OPTS)),1,)
$(1)_INC    := $$(if $$(filter INCLUDES=%,$$($(1)_OPTS)),$$(patsubst INCLUDES=%,%,$$(filter INCLUDES=%,$$($(1)_OPTS))),)
$(1)_INCFLAGS := $$(patsubst %, -I%,$$(subst :, ,$$($(1)_INC)))
$(1)_LINKOBJS := $$(if $$($(1)_NOCRT0),,$$(EXE_CRT0)) $$(EXE_RUNTIME) $$($(1)_OBJS) $$($(1)_FAROBJS)
$(1)_EXE    := $$(EXE_USER_ROOT)/$(1).exe

$$($(1)_OBJDIR):
	mkdir -p $$@

$$($(1)_COBJS): $$($(1)_OBJDIR)/%.rel: $(2)/%.c $$($(1)_HEADERS) \
		| $$($(1)_OBJDIR)
	$(SDCC) $(EXE_CFLAGS) $$($(1)_INCFLAGS) -I $(2) -c $$< -o $$@

$$($(1)_AOBJS): $$($(1)_OBJDIR)/%.rel: $(2)/%.asm | $$($(1)_OBJDIR)
	$(SDASZ80) $(EXE_ASFLAGS) $$($(1)_INCFLAGS) -I$(2) -o $$@ $$<

# Far-code objects (package 44b): the same compile with --codeseg
# FAR_CODE, so their _FAR_CODE area is based at 8000h by the link script.
$$($(1)_FARCOBJS): $$($(1)_OBJDIR)/%.rel: $(2)/%.c $$($(1)_HEADERS) \
		| $$($(1)_OBJDIR)
	$(SDCC) $(EXE_CFLAGS) --codeseg FAR_CODE $$($(1)_INCFLAGS) -I $(2) -c $$< -o $$@

$$($(1)_FARAOBJS): $$($(1)_OBJDIR)/%.rel: $(2)/%.asm | $$($(1)_OBJDIR)
	$(SDASZ80) $(EXE_ASFLAGS) $$($(1)_INCFLAGS) -I$(2) -o $$@ $$<

ifneq ($$($(1)_FARDECL),)
# The default far-segment declaration: the linked _FAR_CODE area becomes
# segment id 1 (regenerated when the rule changes, so an id/bss edit is
# never masked by a stale file).
$$($(1)_FARDECL): $(KOS_SDK_ROOT)/exe.mk | $$($(1)_OBJDIR)
	@printf '%s\n' '[{"id": 1, "kind": "FAR_CODE", "area": "_FAR_CODE", "bss_size": 0}]' > $$@
endif

$$($(1)_OBJDIR)/$(1).lk: $$($(1)_LINKOBJS) $$(EXE_LIBKOS) $(KOS_SDK_ROOT)/exe.mk | $$($(1)_OBJDIR)
	{ \
	  printf '%s\n' '-n' '-m' '-w'; \
	  printf '%s\n' '-i $$($(1)_OBJDIR)/$(1).ihx'; \
	  printf '%s\n' '-b _KOS_SDK_DATA = $$($(1)_LOWBASE)'; \
	  printf '%s\n' '-b _CODE = $$($(1)_CODEBASE)'; \
	  $$(if $$($(1)_FAR),printf '%s\n' '-b _FAR_CODE = 0x8000';,) \
	  printf '%s\n' $$($(1)_LINKOBJS); \
	  printf '%s\n' '-l $$(EXE_LIBKOS)'; \
	  printf '%s\n' '-e'; \
	} > $$@

$$($(1)_OBJDIR)/$(1).link: $$($(1)_OBJDIR)/$(1).lk
	$(SDLDZ80) -f $$<
	touch $$@

$$($(1)_OBJDIR)/$(1).json: $$($(1)_OBJDIR)/$(1).link $$($(1)_SEGMENTSDEP)
	$(PYTHON) tools/exemanifest.py $$($(1)_OBJDIR)/$(1).map $$@ \
	    --name $(1).exe --entry $$($(1)_ENTRY) --stack $$($(1)_STACK) \
	    $$($(1)_SCRATCHARG) $$($(1)_SEGMENTSARG)

$$($(1)_EXE): $$($(1)_OBJDIR)/$(1).json tools/exepack.py
	$(PYTHON) tools/exepack.py pack $$($(1)_OBJDIR)/$(1).json $$@ \
	    --objdir $$($(1)_OBJDIR) --report $$($(1)_OBJDIR)/$(1).report.json
	$(PYTHON) tools/exepack.py validate $$@

ifneq ($$($(1)_STAGE),)
# Recovery images consume their programs from $(MODULES); stage the SDK
# output there without a second build (work/28c-m9-user-sdk).
$$(MODULES)/$(1).exe: $$($(1)_EXE) | $$(MODULES)
	cp $$< $$@
endif
endef
