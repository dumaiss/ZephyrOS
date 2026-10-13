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
#   DATA1..DATA4=SRC:SRC  far-data sources (relative to DIR) compiled into
#                       the areas `_DATA<n>` (their `const` objects) and
#                       `_DATA<n>B` (their zeroed objects), the pair linked
#                       at 8000h - image first, then BSS - as one mappable
#                       DATA segment (package 49d).  The program reaches it
#                       with `farmap(n, &saved)`/`farunmap(saved)` while
#                       the aperture is open, or by handle with
#                       `xread`/`xwrite`.  The options are numbered 1..m
#                       without gaps, and the generated rows follow the
#                       far-code rows, in ascending n; a source with an
#                       initialized non-`const` object is refused by the
#                       packer, naming it.
#   BLOBS=a.bin:b.bin   plain DATA payload files (relative to DIR, at most
#                       32 KiB each) copied into the object directory and
#                       declared as plain DATA rows after the `DATAn=`
#                       rows, in the order given.  `blob(n)` returns the
#                       n-th handle for `xread` (package 49d).
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
# The far-data segments this program declares: one per DATA<n>= option
# used, and they are numbered 1..m without gaps - `farmap(n)` and `blob(n)`
# count the container's DATA rows in id order, so a gap would rename a
# segment (package 49d).
$(1)_DATAIDS := $$(strip $$(foreach n,1 2 3 4,$$(if $$(filter DATA$$(n)=%,$$($(1)_OPTS)),$$(n),)))
$$(if $$(and $$($(1)_DATAIDS),$$(filter-out $$(wordlist 1,$$(words $$($(1)_DATAIDS)),1 2 3 4),$$($(1)_DATAIDS))),$$(error $(1): DATA<n>= sources are numbered 1..m without gaps, got '$$($(1)_DATAIDS)'))
# One "id:source" spec per far-data source and the object each compiles to:
# $(OBJDIR)/data<id>/<base>.rel, so a far-data source can never collide
# with a primary or far-code object.
$(1)_DATASPECS := $$(foreach n,$$($(1)_DATAIDS),$$(foreach s,$$(subst :, ,$$(patsubst DATA$$(n)=%,%,$$(filter DATA$$(n)=%,$$($(1)_OPTS)))),$$(n):$$(s)))
$(1)_DATASRC := $$(foreach spec,$$($(1)_DATASPECS),$$(word 2,$$(subst :, ,$$(spec))))
$(1)_DATAOBJS := $$(foreach spec,$$($(1)_DATASPECS),$$($(1)_OBJDIR)/data$$(word 1,$$(subst :, ,$$(spec)))/$$(patsubst %.c,%.rel,$$(notdir $$(word 2,$$(subst :, ,$$(spec))))))
$(1)_DATASTAMP := $$($(1)_OBJDIR)/data.stamp
# The area-order object the far-data pairs need: sdldz80 concatenates an
# unbased area after the area declared before it, so the BSS area must
# follow its image area in the first input file's area order.  Linked
# first, an empty object that declares nothing but the areas is enough and
# the primary `_CODE`/`_DATA` order is preserved.
$(1)_ORDERREL := $$(if $$($(1)_DATAIDS),$$($(1)_OBJDIR)/kos_order.rel,)
$(1)_ORDERASM := $$($(1)_OBJDIR)/kos_order.asm
# The BLOBS payload files: copied into the object directory (the packer
# resolves `input` against it) under `blobs/`, so the manifest never names
# a path outside the build and a payload edit rebuilds the program.
$(1)_BLOBSPECS := $$(foreach opt,$$(filter BLOBS=%,$$($(1)_OPTS)),$$(patsubst BLOBS=%,%,$$(opt)))
$(1)_BLOBRELS := $$(subst :, ,$$($(1)_BLOBSPECS))
$(1)_BLOBCOPIES := $$(addprefix $$($(1)_OBJDIR)/blobs/,$$($(1)_BLOBRELS))
$(1)_SEGDECL := $$(if $$(or $$($(1)_FARIDS),$$($(1)_DATAIDS),$$($(1)_BLOBRELS)),$$(if $$($(1)_SEGMENTS),,$$($(1)_OBJDIR)/$(1).seg.json),)
$(1)_SEGARGSRC := $$(if $$($(1)_SEGMENTS),$$($(1)_SEGMENTS),$$($(1)_SEGDECL))
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
$(1)_LINKOBJS := $$($(1)_ORDERREL) $$(if $$($(1)_NOCRT0),,$$(EXE_CRT0)) $$(EXE_RUNTIME) $$($(1)_OBJS) $$($(1)_FAROBJS) $$($(1)_DATAOBJS)
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

# Far-data objects (package 49d): every DATA<n>= source is compiled with
# its own pair of areas - `--constseg DATA<n>` for the `const` objects and
# `--dataseg DATA<n>B` for the zeroed ones - so one segment holds the
# table followed by its zeroed variables.  C only: the areas are named by
# the compiler flags, and a source that needs its own layout uses
# SEGMENTS=.
ifneq ($$($(1)_DATASPECS),)
$$($(1)_DATASTAMP): $$(addprefix $(2)/,$$($(1)_DATASRC)) $$($(1)_HEADERS) \
		$$(KOS_RULES) | $$($(1)_OBJDIR)
	@for spec in $$($(1)_DATASPECS); do \
	  n=$$$${spec%%:*}; src=$$$${spec#*:}; \
	  mkdir -p $$($(1)_OBJDIR)/data$$$$n; \
	  base=$$$${src##*/}; base=$$$${base%.*}; \
	  obj=$$($(1)_OBJDIR)/data$$$$n/$$$$base; \
	  case $$$$src in \
	    *.c) $(SDCC) $(EXE_CFLAGS) --constseg DATA$$$$n \
			--dataseg DATA$$$${n}B $$($(1)_INCFLAGS) -I $(2) \
			-c $(2)/$$$$src -o $$$$obj.rel || exit 1 ;; \
	    *) echo "$(1): DATA$$$${n}= source $$$$src is not a .c file" >&2; \
	       exit 1 ;; \
	  esac; \
	done
	touch $$@

$$($(1)_DATAOBJS): | $$($(1)_DATASTAMP)

# The area-order object: `_CODE` and `_DATA` first (the primary chain,
# unchanged), then the far-code areas and each far-data pair, BSS area
# immediately after its image area.  Declaring them first fixes both the
# linker's concatenation order and the order sdldz80 writes the record
# streams of the areas that share 8000h, which the packer reads back.
$$($(1)_ORDERASM): $$(KOS_RULES) | $$($(1)_OBJDIR)
	{ \
	  printf '%s\n' '	.module kos_order' '	.area _CODE' '	.area _DATA'; \
	  for n in $$($(1)_FARIDS); do \
	    printf '%s\n' "	.area _FAR$$$${n}"; \
	  done; \
	  for n in $$($(1)_DATAIDS); do \
	    printf '%s\n' "	.area _DATA$$$${n}" "	.area _DATA$$$${n}B"; \
	  done; \
	} > $$@

$$($(1)_ORDERREL): $$($(1)_ORDERASM)
	$(SDASZ80) $(EXE_ASFLAGS) -o $$@ $$<
endif

# BLOBS payload files: copied into the object directory, so the manifest
# names a file the packer finds next to the link products and `make` sees
# the payload as a prerequisite.
$$($(1)_OBJDIR)/blobs/%: $(2)/% $$(KOS_RULES) | $$($(1)_OBJDIR)
	@mkdir -p $$(dir $$@)
	cp $$< $$@

ifneq ($$($(1)_SEGDECL),)
# The generated segment declaration: every row the rules own, in the order
# the loader expects - the far-code segments first (ids 1..k, the numbers
# `b_<fn>` emits), then one mappable DATA row per DATA<n>= option
# (ascending n), then one plain DATA row per BLOBS file (in the order
# given) (packages 44a/49b/49d).  Regenerated when the rule changes, so a
# layout edit is never masked by a stale file.
$$($(1)_SEGDECL): $$(KOS_RULES) | $$($(1)_OBJDIR)
	@printf '%s' '[' > $$@
	@sep=''; id=1; for n in $$($(1)_FARIDS); do \
	  printf '%s' "$$$$sep{\"id\": $$$$id, \"kind\": \"FAR_CODE\", \"area\": \"_FAR$$$${n}\", \"bss_size\": 0}" >> $$@; \
	  sep=,; id=$$$$((id+1)); \
	done; \
	for n in $$($(1)_DATAIDS); do \
	  printf '%s' "$$$$sep{\"id\": $$$$id, \"kind\": \"DATA\", \"flags\": 1, \"area\": \"_DATA$$$${n}\", \"bss_area\": \"_DATA$$$${n}B\"}" >> $$@; \
	  sep=,; id=$$$$((id+1)); \
	done; \
	for f in $$($(1)_BLOBRELS); do \
	  printf '%s' "$$$$sep{\"id\": $$$$id, \"kind\": \"DATA\", \"flags\": 0, \"input\": \"blobs/$$$${f}\", \"bss_size\": 0}" >> $$@; \
	  sep=,; id=$$$$((id+1)); \
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
	  $$(if $$($(1)_DATAIDS),for n in $$($(1)_DATAIDS); do printf '%s\n' "-b _DATA$$$$n = 0x8000"; done;,) \
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

$$($(1)_EXE): $$($(1)_OBJDIR)/$(1).json $$($(1)_BLOBCOPIES) \
		$$(KOS_TOOLS)/exepack.py
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
