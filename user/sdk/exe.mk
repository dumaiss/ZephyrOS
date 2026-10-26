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
# LIBS= names resolve against the SDK build, where the in-tree archives
# live, exactly as they resolve against lib/ in the standalone SDK.
EXE_LIB_DIR   := $(EXE_SDK_BUILD)
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

KOS_SDK_C_SRC := args.c fs.c truncate.c ipc.c exec.c console_io.c \
    xmem.c seg.c xload.c event.c \
    console_proto.c console_render.c console_info.c timer.c sound.c idle.c stream.c \
    stdio.c fread.c fwrite.c fseek.c feof.c \
    reader.c reader_full.c reader_handle.c reader_stdio.c \
    klibc/strrchr.c klibc/strlcpy.c klibc/strlcat.c \
    klibc/utoa.c klibc/ultoa.c klibc/itoa.c klibc/ltoa.c \
    klibc/printf.c klibc/sdcc_arith.c klibc/sdcc_arith_signed.c
KOS_SDK_ASM_SRC := gate.asm ipcgate.asm diaggate.asm nsgate.asm \
    xmemgate.asm bcall.asm sdcc_calls.asm event_entry.asm sound_out.asm idle_halt.asm \
    klibc/strlen.asm klibc/strcmp.asm klibc/strncmp.asm klibc/strchr.asm \
    klibc/memcpy.asm klibc/memmove.asm klibc/memset.asm klibc/memcmp.asm \
    klibc/divmod.asm klibc/sdcc_arith16.asm klibc/mul.asm \
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

# --- libtui ------------------------------------------------------------
# The TUI SDK is a separately linked library: one archive member per
# coherent function group, so a program that calls only part of the
# library does not link the rest.  The private state and declarations
# stay in this directory.
KOS_TUI_SRC     := $(KOS_SDK_ROOT)/tui/src
KOS_TUI_PRIVATE := $(KOS_TUI_SRC)/tui_internal.h
KOS_TUI_C_SRC   := tui_state.c tui_screen.c tui_lifecycle.c tui_flush.c \
	tui_printf.c tui_window.c tui_style.c tui_key.c tui_region.c
KOS_TUI_ASM_SRC := tui_scroll.asm tui_text.asm
EXE_TUI_OBJS    := $(addprefix $(EXE_SDK_BUILD)/tui/,\
	$(patsubst %.c,%.rel,$(KOS_TUI_C_SRC)) \
	$(patsubst %.asm,%.rel,$(KOS_TUI_ASM_SRC)))
EXE_LIBTUI      := $(EXE_SDK_BUILD)/libtui.lib

$(EXE_SDK_BUILD)/tui/%.rel: $(KOS_TUI_SRC)/%.c $(EXE_HEADERS) \
		$(KOS_TUI_PRIVATE)
	@mkdir -p $(dir $@)
	$(SDCC) $(EXE_CFLAGS) -I $(KOS_TUI_SRC) -c $< -o $@

$(EXE_SDK_BUILD)/tui/%.rel: $(KOS_TUI_SRC)/%.asm $(EXE_HEADERS) \
		$(KOS_TUI_PRIVATE)
	@mkdir -p $(dir $@)
	$(SDASZ80) $(EXE_ASFLAGS) -o $@ $<

# `-D` zeroes the members' dates and ids.  The in-tree programs link this
# archive and the standalone SDK ships it, so the library never records
# when it was built.
$(EXE_LIBTUI): $(EXE_TUI_OBJS) $(KOS_RULES)
	@mkdir -p $(dir $@)
	rm -f $@
	sdar -rcD $@ $(EXE_TUI_OBJS)

# The wide capacity (a program's TUI_GRID=128x35, mk/kos.mk): the same
# sources built for a 128 x 35 cell map, `libtui_128x35.lib`.
EXE_TUI_WIDE_OBJS := $(addprefix $(EXE_SDK_BUILD)/tui_128x35/,\
	$(patsubst %.c,%.rel,$(KOS_TUI_C_SRC)) \
	$(patsubst %.asm,%.rel,$(KOS_TUI_ASM_SRC)))
EXE_LIBTUI_WIDE   := $(EXE_SDK_BUILD)/libtui_128x35.lib

$(EXE_SDK_BUILD)/tui_128x35/%.rel: $(KOS_TUI_SRC)/%.c $(EXE_HEADERS) \
		$(KOS_TUI_PRIVATE)
	@mkdir -p $(dir $@)
	$(SDCC) $(EXE_CFLAGS) -DTUI_GRID_COLS=128u -DTUI_GRID_ROWS=35u \
		-I $(KOS_TUI_SRC) -c $< -o $@

$(EXE_SDK_BUILD)/tui_128x35/%.rel: $(KOS_TUI_SRC)/%.asm $(EXE_HEADERS) \
		$(KOS_TUI_PRIVATE)
	@mkdir -p $(dir $@)
	$(SDASZ80) $(EXE_ASFLAGS) -o $@ $<

$(EXE_LIBTUI_WIDE): $(EXE_TUI_WIDE_OBJS) $(KOS_RULES)
	@mkdir -p $(dir $@)
	rm -f $@
	sdar -rcD $@ $(EXE_TUI_WIDE_OBJS)

# --- libtui_widgets ----------------------------------------------------
# The optional procedural widget layer is a second deterministic archive.
# It depends only on the public tui.h surface; the modal member is kept
# separate so render-only programs do not pull its 60 x 16 save buffer.
KOS_WIDGET_SRC     := $(KOS_SDK_ROOT)/widgets/src
KOS_WIDGET_PRIVATE := $(KOS_WIDGET_SRC)/widget_internal.h
KOS_WIDGET_C_SRC   := widget_common.c widget_render.c widget_menu.c \
	widget_input.c widget_modal.c
EXE_WIDGET_OBJS    := $(addprefix $(EXE_SDK_BUILD)/widgets/,\
	$(patsubst %.c,%.rel,$(KOS_WIDGET_C_SRC)))
EXE_LIBTUI_WIDGETS := $(EXE_SDK_BUILD)/libtui_widgets.lib

$(EXE_SDK_BUILD)/widgets/%.rel: $(KOS_WIDGET_SRC)/%.c $(EXE_HEADERS) \
		$(KOS_WIDGET_PRIVATE)
	@mkdir -p $(dir $@)
	$(SDCC) $(EXE_CFLAGS) -I $(KOS_WIDGET_SRC) -c $< -o $@

$(EXE_LIBTUI_WIDGETS): $(EXE_WIDGET_OBJS) $(KOS_RULES)
	@mkdir -p $(dir $@)
	rm -f $@
	sdar -rcD $@ $(EXE_WIDGET_OBJS)

# --- libgfx -------------------------------------------------------------
# The graphics SDK is a separately linked archive: one member per
# function group, so a program that calls only part of the library does
# not link the rest.  The public surface is kos/gfx.h; the private state
# and the backend interface stay in this directory.
KOS_GFX_SRC     := $(KOS_SDK_ROOT)/gfx/src
KOS_GFX_PRIVATE := $(KOS_GFX_SRC)/gfx_internal.h
KOS_GFX_C_SRC   := gfx.c gfx_mode.c gfx_palette.c gfx_font.c gfx_font4.c \
	gfx_atlas.c backends/v9958/v9958_gfx.c
KOS_GFX_ASM_SRC := backends/v9958/v9958_io.asm backends/v9958/gfx_cmd.asm \
	backends/v9958/gfx_ellipse.asm backends/v9958/gfx_text.asm \
	backends/v9958/gfx_atlas_draw.asm backends/v9958/gfx_upload.asm
EXE_GFX_OBJS    := $(addprefix $(EXE_SDK_BUILD)/gfx/,\
	$(patsubst %.c,%.rel,$(KOS_GFX_C_SRC)) \
	$(patsubst %.asm,%.rel,$(KOS_GFX_ASM_SRC)))
EXE_LIBGFX      := $(EXE_SDK_BUILD)/libgfx.lib

$(EXE_SDK_BUILD)/gfx/%.rel: $(KOS_GFX_SRC)/%.c $(EXE_HEADERS) \
		$(KOS_GFX_PRIVATE)
	@mkdir -p $(dir $@)
	$(SDCC) $(EXE_CFLAGS) -I $(KOS_GFX_SRC) -I $(EXE_SDK_BUILD)/gfx \
		-c $< -o $@

$(EXE_SDK_BUILD)/gfx/%.rel: $(KOS_GFX_SRC)/%.asm $(EXE_HEADERS) \
		$(KOS_GFX_PRIVATE)
	@mkdir -p $(dir $@)
	$(SDASZ80) $(EXE_ASFLAGS) -o $@ $<

# gfx_font.c includes the generated font byte list (the OS tree's
# Makefile builds it from the pinned asset with tools/fontpack.py).
$(EXE_SDK_BUILD)/gfx/gfx_font.rel: $(EXE_SDK_BUILD)/gfx/font_builtin.inc
$(EXE_SDK_BUILD)/gfx/gfx_font4.rel: $(EXE_SDK_BUILD)/gfx/font4_builtin.inc

# `-D` zeroes the members' dates and ids.  The in-tree programs link
# this archive and the standalone SDK ships it, so the library never
# records when it was built.
$(EXE_LIBGFX): $(EXE_GFX_OBJS) $(KOS_RULES)
	@mkdir -p $(dir $@)
	rm -f $@
	sdar -rcD $@ $(EXE_GFX_OBJS)

# --- libkres -------------------------------------------------------------
# The portable KRES core plus the KonaOS adapter and the XMEM catalog
# cache.  One archive member per source, so a program that only reads
# a sealed container links the read path, the adapter's read side and
# the cache, and none of the mutation, flush, builder, compaction,
# extract or verify code (design §8).

KOS_KRES_SRC       := $(KOS_SDK_ROOT)/kres/src
KOS_KRES_KONAOS    := $(KOS_SDK_ROOT)/kres/konaos
KOS_KRES_PRIVATE   := $(KOS_KRES_SRC)/kres_internal.h \
	$(KOS_KRES_SRC)/kres_layout.h $(KOS_KRES_KONAOS)/kona_internal.h
KOS_KRES_PUBLIC    := $(KOS_INCLUDE)/kos/kres.h \
	$(KOS_INCLUDE)/kos/kres_konaos.h \
	$(KOS_INCLUDE)/kos/reader.h
KOS_KRES_C_SRC     := kres_util.c kres_read.c kres_mut.c kres_build.c \
	kres_compact.c kres_verify.c kres_extract.c kres_reader.c
KOS_KRES_KONAOS_SRC := kres_adapter.c kres_adapter_mut.c kres_cache_xmem.c
EXE_KRES_OBJS      := $(addprefix $(EXE_SDK_BUILD)/kres/,\
	$(patsubst %.c,%.rel,$(KOS_KRES_C_SRC))) \
	$(addprefix $(EXE_SDK_BUILD)/kres/konaos/,\
	$(patsubst %.c,%.rel,$(KOS_KRES_KONAOS_SRC)))
EXE_LIBKRES        := $(EXE_SDK_BUILD)/libkres.lib

$(EXE_SDK_BUILD)/kres/konaos/%.rel: $(KOS_KRES_KONAOS)/%.c \
		$(EXE_HEADERS) $(KOS_KRES_PRIVATE) $(KOS_KRES_PUBLIC)
	@mkdir -p $(dir $@)
	$(SDCC) $(EXE_CFLAGS) -I $(KOS_KRES_SRC) -I $(KOS_KRES_KONAOS) \
		-c $< -o $@

$(EXE_SDK_BUILD)/kres/%.rel: $(KOS_KRES_SRC)/%.c $(EXE_HEADERS) \
		$(KOS_KRES_PRIVATE) $(KOS_KRES_PUBLIC)
	@mkdir -p $(dir $@)
	$(SDCC) $(EXE_CFLAGS) -I $(KOS_KRES_SRC) -c $< -o $@

$(EXE_LIBKRES): $(EXE_KRES_OBJS) $(KOS_RULES)
	@mkdir -p $(dir $@)
	rm -f $@
	sdar -rcD $@ $(EXE_KRES_OBJS)

# --- libsnd -------------------------------------------------------------
# The sound SDK is a separately linked archive: one member per function
# group, so a manual-note program links no ZTR, ZVG, reader, stdio or
# KRES member.  The public surface is kos/snd.h; the private state, the
# voice-program format and the assembly interface stay in this directory.
# Rendering is assembly: snd_tick, the voice engine, the shadow and the
# flush.  Setup is C: the lifecycle and the note compiler.
KOS_SND_SRC     := $(KOS_SDK_ROOT)/snd/src
KOS_SND_PRIVATE := $(KOS_SND_SRC)/snd_private.h
KOS_SND_C_SRC   := snd.c snd_manual.c snd_tables.c snd_zvg.c snd_ztr.c
KOS_SND_ASM_SRC := snd_tick.asm snd_voice.asm snd_out.asm \
	snd_zvg_dec.asm snd_ztr_dec.asm
EXE_SND_OBJS    := $(addprefix $(EXE_SDK_BUILD)/snd/,\
	$(patsubst %.c,%.rel,$(KOS_SND_C_SRC)) \
	$(patsubst %.asm,%.rel,$(KOS_SND_ASM_SRC)))
EXE_LIBSND      := $(EXE_SDK_BUILD)/libsnd.lib

$(EXE_SDK_BUILD)/snd/%.rel: $(KOS_SND_SRC)/%.c $(EXE_HEADERS) \
		$(KOS_SND_PRIVATE)
	@mkdir -p $(dir $@)
	$(SDCC) $(EXE_CFLAGS) -I $(KOS_SND_SRC) -c $< -o $@

$(EXE_SDK_BUILD)/snd/%.rel: $(KOS_SND_SRC)/%.asm $(EXE_HEADERS) \
		$(KOS_SND_PRIVATE)
	@mkdir -p $(dir $@)
	$(SDASZ80) $(EXE_ASFLAGS) -o $@ $<

# `-D` zeroes the members' dates and ids.  The in-tree programs link
# this archive and the standalone SDK ships it, so the library never
# records when it was built.
$(EXE_LIBSND): $(EXE_SND_OBJS) $(KOS_RULES)
	@mkdir -p $(dir $@)
	rm -f $@
	sdar -rcD $@ $(EXE_SND_OBJS)
