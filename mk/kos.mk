# -----------------------------------------------------------------------------
# KonaOS EXE program rules (work packages 28c/49c).
#
# The generic rule for native KonaOS programs, shared by two users:
#
#   - the OS tree: its own rule file (the SDK directory's exe.mk) sets the
#     in-tree roots and builds libkos from the SDK sources, then includes
#     this file;
#   - the standalone SDK: this file is shipped as
#     kos-sdk-<version>/mk/kos.mk, the prebuilt libkos.lib and crt0.rel
#     come from the SDK's lib/, and a program's Makefile sets
#     KOS_SDK_ROOT to the unpacked SDK before including it.
#
# A program is declared with one line:
#
#     $(eval $(call exe_program,NAME,DIR,SOURCES[,OPT=VALUE ...]))
#
# SOURCES are relative to DIR; .c files go through SDCC and .asm files
# through sdasz80.  The rule links crt0 (unless NO_CRT0=1) and libkos,
# generates the EXE manifest from the link map, and packs and validates
# build/user/NAME.exe.
#
# Each declaration also defines a `NAME` target (`make NAME` packs that
# program), so a program Makefile builds with a plain `make` by listing
# its programs in its `all` target.
#
# Options (defaults in parentheses):
#   STACK=N             child stack bytes (256)
#   SCRATCH=SYM:SIZE    manifest scratch pin (kos_scratch:16)
#   ENTRY=SYMBOL        EXE entry symbol (kos_entry)
#   NO_CRT0=1           do not link crt0; ENTRY names the program's own
#                       entry object (used by the raw-return test child)
#   LOWBASE=ADDR        SDK gate-visible data base (0x0100)
#   CODEBASE=ADDR       program code base (0x015D)
#   SEGMENTS=FILE       JSON list of EXE v2 auxiliary segment
#                       declarations (id/kind/input or area/bss_size); FILE
#                       is a prerequisite and its payload `input` names
#                       resolve against the program's object directory
#                       (package 44a)
#   FAR1..FAR4=SRC:SRC .. far-code sources (relative to DIR) compiled as far
#                       segment n (n = 1..4): each is compiled with bank n
#                       and --codeseg FAR<n>, so a `__banked` definition in
#                       it emits `b_<fn> = <n>` (the id the caller's
#                       `ld e,#b_<fn>` names) and its code lands in the
#                       `_FAR<n>` area, linked at 8000h; the sources
#                       themselves carry no bank pragma (package 49b).
#                       Without SEGMENTS= the rule declares one FAR_CODE
#                       segment per option used, id n, area `_FAR<n>`; the
#                       ids are the numbers a `__banked` definition emits
#                       (`b_<fn>` = n), so the far segments are the
#                       container's first rows (ids 1..k) and a program
#                       that also declares DATA segments lists those after
#                       them.  The packer checks the 36.17 link rule
#                       (primary image, BSS and stack outside 8000h-BFFFh)
#                       and the 16 KiB aperture limit, naming the
#                       segment.
#   STAGE=1             copy the packed program into $(KOS_MODULES)
#                       (default: $(KOS_BUILD)/modules)
#   INCLUDES=a:b        extra include roots (colon separated), for
#                       program-owned headers only
#
# The roots below are the only place the rule file touches a path; both
# users set them to their own tree before including it, so nothing here
# names a source tree.
# -----------------------------------------------------------------------------

KOS_SDK_ROOT ?= ..
KOS_BUILD    ?= build
KOS_PYTHON   ?= python3
KOS_TOOLS    ?= $(KOS_SDK_ROOT)/tools
KOS_INCLUDE  ?= $(KOS_SDK_ROOT)/include
# The public ABI copy the SDK headers reach as "kos/…".  It is the first
# include root, exactly as in the OS tree, so a quoted "kos/fs.h" inside
# an SDK wrapper header resolves to the ABI header and not to itself.
EXE_ABI_ROOT ?= $(KOS_INCLUDE)/abi
KOS_LIB      ?= $(KOS_SDK_ROOT)/lib
KOS_MODULES  ?= $(KOS_BUILD)/modules
# Every file that defines the program rules: editing one must relink (and,
# where the sources are in-tree, re-archive).
KOS_RULES    ?= $(KOS_SDK_ROOT)/mk/kos.mk

SDCC    ?= sdcc
SDASZ80 ?= sdasz80
SDLDZ80 ?= sdldz80

EXE_USER_ROOT := $(KOS_BUILD)/user
# The SDK library and startup the program rule links.  The standalone SDK
# ships them prebuilt in lib/; the in-tree wrapper points them at the
# objects it builds from the SDK sources (and keeps the runtime object
# separate).
EXE_CRT0   ?= $(KOS_LIB)/crt0.rel
EXE_LIBKOS ?= $(KOS_LIB)/libkos.lib
EXE_RUNTIME ?=

EXE_CFLAGS  := -mz80 -c --nostdinc --Werror -I$(EXE_ABI_ROOT) -I$(KOS_INCLUDE)
# Header prerequisites (work packages 37/38).  SDCC emits no dependency
# files through this rule, and the SDK's include surface is exactly the
# public ABI plus the SDK include root, so every program object depends on
# that whole set (and on its own directory's headers): a stale program
# object can then never mask a real failure.
EXE_HEADERS := $(wildcard $(EXE_ABI_ROOT)/kos/*.h) $(wildcard $(KOS_INCLUDE)/*.h)
EXE_ASFLAGS := -I$(EXE_ABI_ROOT)

# The program code base the ordinary layout uses: the SDK's gate-visible
# data area (93 bytes at LOWBASE) plus the low window base.  A program
# that reserves a caller buffer or links a larger SDK data area passes
# CODEBASE explicitly.
EXE_DEFAULT_CODEBASE := 0x015D

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
# The far code segments this program declares: one per FAR<n>= option
# used, ascending, so segment n is always the `_FAR<n>` area (package
# 49b).  A program with a far BSS, or with an id/area layout of its own,
# declares its segments through SEGMENTS= instead.
$(1)_FARIDS := $$(strip $$(foreach n,1 2 3 4,$$(if $$(filter FAR$$(n)=%,$$($(1)_OPTS)),$$(n),)))
# One "id:source" spec per far source, in option order, and the object each
# compiles to: $(OBJDIR)/far<id>/<base>.rel, so a far source can never
# collide with a primary object and each id owns its own flags.
$(1)_FARSPECS := $$(foreach n,$$($(1)_FARIDS),$$(foreach s,$$(subst :, ,$$(patsubst FAR$$(n)=%,%,$$(filter FAR$$(n)=%,$$($(1)_OPTS)))),$$(n):$$(s)))
$(1)_FARSRC := $$(foreach spec,$$($(1)_FARSPECS),$$(word 2,$$(subst :, ,$$(spec))))
$(1)_FAROBJS := $$(foreach spec,$$($(1)_FARSPECS),$$($(1)_OBJDIR)/far$$(word 1,$$(subst :, ,$$(spec)))/$$(patsubst %.c,%.rel,$$(patsubst %.asm,%.rel,$$(notdir $$(word 2,$$(subst :, ,$$(spec)))))))
$(1)_FARSTAMP := $$($(1)_OBJDIR)/far.stamp
$(1)_FARDECL := $$(if $$($(1)_FARIDS),$$(if $$($(1)_SEGMENTS),,$$($(1)_OBJDIR)/$(1).far.json),)
$(1)_SEGARGSRC := $$(if $$($(1)_SEGMENTS),$$($(1)_SEGMENTS),$$($(1)_FARDECL))
$(1)_SEGMENTSARG := $$(if $$($(1)_SEGARGSRC), --segments $$($(1)_SEGARGSRC),)
$(1)_SEGMENTSDEP := $$(if $$($(1)_SEGARGSRC), $$($(1)_SEGARGSRC),)
$(1)_ENTRY  := $$(if $$(filter ENTRY=%,$$($(1)_OPTS)),$$(patsubst ENTRY=%,%,$$(filter ENTRY=%,$$($(1)_OPTS))),kos_entry)
# The fixed SDK area is 93 bytes (_KOS_SDK_DATA in runtime.asm and the
# gate blocks), so the ordinary layout reserves 0100h-015Ch and starts
# code at 015Dh.  A program that reserves a caller buffer supplies both
# bases explicitly.
$(1)_LOWBASE := $$(if $$(filter LOWBASE=%,$$($(1)_OPTS)),$$(patsubst LOWBASE=%,%,$$(filter LOWBASE=%,$$($(1)_OPTS))),0x0100)
$(1)_CODEBASE := $$(if $$(filter CODEBASE=%,$$($(1)_OPTS)),$$(patsubst CODEBASE=%,%,$$(filter CODEBASE=%,$$($(1)_OPTS))),$$(EXE_DEFAULT_CODEBASE))
$(1)_NOCRT0 := $$(if $$(filter NO_CRT0=1,$$($(1)_OPTS)),1,)
$(1)_STAGE  := $$(if $$(filter STAGE=1,$$($(1)_OPTS)),1,)
$(1)_INC    := $$(if $$(filter INCLUDES=%,$$($(1)_OPTS)),$$(patsubst INCLUDES=%,%,$$(filter INCLUDES=%,$$($(1)_OPTS))),)
$(1)_INCFLAGS := $$(patsubst %, -I%,$$(subst :, ,$$($(1)_INC)))
$(1)_LINKOBJS := $$(if $$($(1)_NOCRT0),,$$(EXE_CRT0)) $$(EXE_RUNTIME) $$($(1)_OBJS) $$($(1)_FAROBJS)
$(1)_EXE    := $$(EXE_USER_ROOT)/$(1).exe

# The program itself is a target of its own name (`make NAME`), so a
# program Makefile lists its programs in its `all` target and `make`
# builds them without naming an output path.
.PHONY: $(1)
$(1): $$($(1)_EXE)

$$($(1)_OBJDIR):
	mkdir -p $$@

$$($(1)_COBJS): $$($(1)_OBJDIR)/%.rel: $(2)/%.c $$($(1)_HEADERS) \
		| $$($(1)_OBJDIR)
	$(SDCC) $(EXE_CFLAGS) $$($(1)_INCFLAGS) -I $(2) -c $$< -o $$@

$$($(1)_AOBJS): $$($(1)_OBJDIR)/%.rel: $(2)/%.asm | $$($(1)_OBJDIR)
	$(SDASZ80) $(EXE_ASFLAGS) $$($(1)_INCFLAGS) -I$(2) -o $$@ $$<

# Far-code objects (package 49b): one recipe compiles every FAR<n>=
# source with its own segment's flags - bank n first, then the area, so
# the module lands in `_FAR<n>` and every `__banked` definition in it
# emits `b_<fn> = n`.  A far .asm source opens `.area _FAR<n>` itself
# (and sets its own `b_<fn>` when it defines a banked entry).
# The stamp is the one target make knows: the objects below hang off it
# order-only, so a source edit recompiles the program's far code and
# nothing else.
$$($(1)_FARSTAMP): $$(addprefix $(2)/,$$($(1)_FARSRC)) $$($(1)_HEADERS) \
		$$(KOS_RULES) | $$($(1)_OBJDIR)
	@for spec in $$($(1)_FARSPECS); do \
	  n=$$$${spec%%:*}; src=$$$${spec#*:}; \
	  mkdir -p $$($(1)_OBJDIR)/far$$$$n; \
	  base=$$$${src##*/}; base=$$$${base%.*}; \
	  obj=$$($(1)_OBJDIR)/far$$$$n/$$$$base; \
	  case $$$$src in \
	    *.c) $(SDCC) $(EXE_CFLAGS) -bo $$$$n --codeseg FAR$$$$n \
			$$($(1)_INCFLAGS) -I $(2) -c $(2)/$$$$src -o $$$$obj.rel || exit 1 ;; \
	    *.asm) $(SDASZ80) $(EXE_ASFLAGS) $$($(1)_INCFLAGS) -I$(2) \
			-o $$$$obj.rel $(2)/$$$$src || exit 1 ;; \
	    *) echo "unknown far source $$$$src" >&2; exit 1 ;; \
	  esac; \
	done
	touch $$@

ifneq ($$($(1)_FAROBJS),)
$$($(1)_FAROBJS): | $$($(1)_FARSTAMP)
endif

ifneq ($$($(1)_FARDECL),)
# The default far-segment declaration: one FAR_CODE segment per FAR<n>=
# option used, the area that option compiled its sources into
# (regenerated when the rule changes, so a segment edit is never masked
# by a stale file).
$$($(1)_FARDECL): $$(KOS_RULES) | $$($(1)_OBJDIR)
	@printf '%s' '[' > $$@
	@sep=''; for n in $$($(1)_FARIDS); do \
	  printf '%s' "$$$$sep{\"id\": $$$$n, \"kind\": \"FAR_CODE\", \"area\": \"_FAR$$$$n\", \"bss_size\": 0}" >> $$@; \
	  sep=,; \
	done
	@printf '%s\n' ']' >> $$@
endif

$$($(1)_OBJDIR)/$(1).lk: $$($(1)_LINKOBJS) $$(EXE_LIBKOS) $$(KOS_RULES) | $$($(1)_OBJDIR)
	{ \
	  printf '%s\n' '-n' '-m' '-w'; \
	  printf '%s\n' '-i $$($(1)_OBJDIR)/$(1).ihx'; \
	  printf '%s\n' '-b _KOS_SDK_DATA = $$($(1)_LOWBASE)'; \
	  printf '%s\n' '-b _CODE = $$($(1)_CODEBASE)'; \
	  $$(if $$($(1)_FARIDS),for n in $$($(1)_FARIDS); do printf '%s\n' "-b _FAR$$$$n = 0x8000"; done;,) \
	  printf '%s\n' $$($(1)_LINKOBJS); \
	  printf '%s\n' '-l $$(EXE_LIBKOS)'; \
	  printf '%s\n' '-e'; \
	} > $$@

$$($(1)_OBJDIR)/$(1).link: $$($(1)_OBJDIR)/$(1).lk
	$(SDLDZ80) -f $$<
	touch $$@

$$($(1)_OBJDIR)/$(1).json: $$($(1)_OBJDIR)/$(1).link $$($(1)_SEGMENTSDEP)
	$(KOS_PYTHON) $$(KOS_TOOLS)/exemanifest.py $$($(1)_OBJDIR)/$(1).map $$@ \
	    --name $(1).exe --entry $$($(1)_ENTRY) --stack $$($(1)_STACK) \
	    $$($(1)_SCRATCHARG) $$($(1)_SEGMENTSARG)

$$($(1)_EXE): $$($(1)_OBJDIR)/$(1).json $$(KOS_TOOLS)/exepack.py
	$(KOS_PYTHON) $$(KOS_TOOLS)/exepack.py pack $$($(1)_OBJDIR)/$(1).json $$@ \
	    --objdir $$($(1)_OBJDIR) --report $$($(1)_OBJDIR)/$(1).report.json
	$(KOS_PYTHON) $$(KOS_TOOLS)/exepack.py validate $$@

ifneq ($$($(1)_STAGE),)
# Recovery images consume their programs from $(KOS_MODULES); stage the
# SDK output there without a second build (work/28c-m9-user-sdk).
$$(KOS_MODULES)/$(1).exe: $$($(1)_EXE)
	@mkdir -p $$(dir $$@)
	cp $$< $$@
endif
endef
