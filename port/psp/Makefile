# port/psp/Makefile -- native PSP build.
#
#   make            same as `make core-objs`: compile the whole engine core for
#                   MIPS. Use it as a compile check while porting.
#   make tests      build one EBOOT.PBP per gameplay test (see TESTS below).
#   make check      verify that the toolchain paths in this file exist.
#   make clean
#
# Toolchain: pspdev (psp-gcc + pspsdk). On Windows this is the MSYS2-hosted
# pspdev-win build.  IMPORTANT: every path that is handed to a toolchain
# binary must be written in Windows form ("C:/pspdev"), because those binaries
# are Cygwin-hosted and do not understand the MSYS "/c/..." style.
# If your toolchain lives elsewhere, override it on the command line:
#
#   make PSPDEV=D:/pspdev tests
#
# This file only *selects* sources and flags.  It must never need #ifdefs in
# the shared engine sources -- that split is what port/objs.mk is for.

# ---------------------------
# toolchain
# ---------------------------

PSPDEV ?= C:/pspdev

# PSPDEV often arrives from the environment, where MSYS exports it as
# "/c/pspdev".  The toolchain cannot use that form (see the note on paths
# below), so normalise it to "C:/pspdev" no matter how it was set.
PSPDEV := $(shell cygpath -m "$(PSPDEV)" 2>/dev/null || echo "$(PSPDEV)")

PSPSDK ?= $(PSPDEV)/psp/sdk

CC      := $(PSPDEV)/bin/psp-gcc
FIXUP   := $(PSPDEV)/bin/psp-fixup-imports
STRIP   := $(PSPDEV)/bin/psp-strip
MKSFO   := $(PSPDEV)/bin/mksfoex
PACKPBP := $(PSPDEV)/bin/pack-pbp

# Name of the game module (EBOOT title lives in PARAM.SFO).
TARGET ?= kabluk

# Firmware the EBOOT advertises. 6.00 is the safe choice for CFW.
PSP_FW_VERSION ?= 600

# ---------------------------
# layout
# ---------------------------

# The toolchain binaries are Cygwin-hosted: they understand "C:/dev/kabluk"
# but not the MSYS form "/c/dev/kabluk" (psp-gcc would silently look in the
# wrong place and then claim a header does not exist).  So every path we hand
# to psp-gcc is converted to the Windows form here.  `cygpath -m` does exactly
# that and exists in MSYS2; on Linux it is absent and the path is used as is.
ROOT_RAW := $(abspath $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))/../..)
ROOT     := $(shell cygpath -m "$(ROOT_RAW)" 2>/dev/null || echo "$(ROOT_RAW)")

HERE   := $(ROOT)/port/psp
ENGINE := $(ROOT)/Quake
TESTS  := $(ROOT)/tests
BUILD  := $(HERE)/build

# ---------------------------
# flags
# ---------------------------

# SDL2 comes from pspdev (Tier-1 library).  The engine includes SDL.h and
# SDL_opengl.h unconditionally through quakedef.h, so the headers must be on
# the include path even for the headless test builds.
CPPFLAGS += -I$(ENGINE)
CPPFLAGS += -I$(PSPDEV)/psp/include
CPPFLAGS += -I$(PSPDEV)/psp/include/SDL2
CPPFLAGS += -I$(PSPSDK)/include
CPPFLAGS += -DUSE_SDL2
CPPFLAGS += -D_PSP_FW_VERSION=$(PSP_FW_VERSION)

# No libcurl and no internet on a PSP-E100x: drop the external-IP lookup in
# net_main.c rather than pulling another third-party library into the port.
CPPFLAGS += -DFEATURE_CURL=0

# zlib ships with pspdev, and the engine needs it unconditionally: fs_zip.c
# and common.c call inflate() whether or not USE_ZLIB is defined, so leaving it
# off would only trade the header for a link error.
CPPFLAGS += -DUSE_ZLIB

# SDL2 for PSP declares SDL_MAIN_AVAILABLE/NEEDED, which makes SDL_main.h do
# "#define main SDL_main" and expect int SDL_main(int, char**).  Our entry
# points are plain int main(void) (the tests) or will be our own module entry
# (main_psp.c), so switch that substitution off with the documented flag.
# Anything that does call SDL_Init() must call SDL_SetMainReady() first.
CPPFLAGS += -DSDL_MAIN_HANDLED


# gnu11, not the compiler default: GCC 15 defaults to gnu23, where true and
# false are keywords, and q_stdinc.h declares them as enum constants.
CFLAGS += -std=gnu11
CFLAGS += -O2 -G0 -Wall
CFLAGS += -Wno-unused-variable -Wno-unused-but-set-variable
CFLAGS += -fno-strict-aliasing
# One section per function: lets the linker drop the parts of the engine a
# given target does not reach (the renderer, for the test builds).
CFLAGS += -ffunction-sections -fdata-sections

# Where -lpsp* actually live.  pspsdk's build.mak adds these itself; this
# makefile writes its own link line, so it has to add them too -- without them
# the linker reports "cannot find -lpspdebug" even though the files are there.
LIBDIR := $(PSPDEV)/psp/lib $(PSPSDK)/lib
LDFLAGS += $(addprefix -L,$(LIBDIR))

# --gc-sections is not optional here.  Each test is one translation unit that
# exercises part of sv_user.c/pmove.c; the rest of those files is dead code
# whose references (svs, Cmd_*, MSG_*, Sys_Error, ...) would otherwise have to
# be satisfied by linking half the engine.  The desktop test script collects
# the same way, so the two builds agree.
#
# It does eat the NID table, though: .rodata.sceNid is only reached by the
# loader, never by code, so nothing keeps it alive and psp-fixup-imports then
# aborts with "no nid section found" (or with a size mismatch, once only half
# of the pair survives).  keep_sce.ld re-adds both halves with KEEP()
# without touching the default script (see the comment in that file).
LDFLAGS += -Wl,--gc-sections
LDFLAGS += -Wl,-T,$(HERE)/keep_sce.ld
LDFLAGS += -Wl,-zmax-page-size=128

# PSP user-mode libraries.  -lpspdebug is what our console prints through.
# sceKernel* (DelayThread, ExitGame) live in libpspkernel, which pspsdk's
# build.mak does not add for user-mode programs.
PSP_LIBS := -lpspdebug -lpspdisplay -lpspge -lpspctrl -lpspkernel
TEST_LIBS := $(PSP_LIBS) -lm
# SDL2 for PSP is not a self-contained black box: its video backend talks to
# sceGu directly and its audio backend to sceAudio, so linking it drags in most
# of the SDK.  This is SDL's own Makefile.psp list minus the optional pspgl
# trio (-lGL -lGLU -lglut) we do not use.  libpspgu.a also carries
# vramalloc/vfree/vrelptr (pspsdk src/gu/vram.c).
#
# --start-group matters: these are static archives, and ld scans an archive
# only once, in command-line order.  With a plain list, a library needed by one
# that follows it is never revisited -- -lm before -lSDL2, for instance, leaves
# SDL's sin()/cos() undefined.  A group makes the list order-independent.
# No -lpspthreadman: current pspsdk has no such archive.  ThreadMan lives in
# src/kernel/ThreadManForKernel.S and therefore in libpspkernel.a, which is
# already in PSP_LIBS -- that is also why psp_boot.c's sceKernelDelayThread
# links without it.
# No -lSDL2 and therefore no pspgl (-lGL -lGLU -lglut) and no libpspvram:
# sdl_stub.c answers the 52 SDL entry points the engine calls.  Linking the
# real SDL2 pulls in its pspgl/vram imports, and a module whose import list
# names a library that is not resident at boot is refused by the firmware with
# 8002013C (SCE_KERNEL_ERROR_LIBRARY_NOTFOUND) before a single instruction
# runs.  These are the same libraries the test EBOOTs link, and they boot.
GAME_LIB_NAMES := -lm -lz -lpspgu -lpspgum -lpspvfpu -lpsprtc \
                  -lpsppower -lpspaudiolib -lpspaudio -lpsphprm -lpspumd \
                  $(PSP_LIBS) -lpspsdk
GAME_LIBS := -Wl,--start-group $(GAME_LIB_NAMES) -Wl,--end-group

# libpspkernel.a carries the SysclibForKernel import stubs, and two of them are
# called strtol and strtoul -- the same names newlib exports.  Both end up in
# the link, and ld calls that a multiple definition.  Keeping the first, which
# is the kernel stub forwarding to the firmware's Sysclib module, is exactly
# what we want; the alternative is to stop linking libpspkernel, which we
# cannot do (sceKernelDelayThread and friends live there).
GAME_LDFLAGS := -Wl,--allow-multiple-definition

# Without this, a recipe that fails after the linker has written the output
# leaves the file in place and the next make calls it up to date.  That is how
# a broken kabluk.elf survived the fixup error earlier.
.DELETE_ON_ERROR:

# Redirect the tests' stdio to the on-screen console.  The rename happens at
# compile time and only for tests/*.c, so the engine sources keep using the
# real stdio.
TEST_CPPFLAGS += -Dputs=psp_con_puts -Dprintf=psp_con_printf
TEST_CPPFLAGS += -Dfputs=psp_con_fputs -Dfprintf=psp_con_fprintf
TEST_CPPFLAGS += -Dputchar=psp_con_putchar -Dfflush=psp_con_fflush

# ---------------------------
# object groups (shared with the desktop build)
# ---------------------------

include $(ROOT)/port/objs.mk

# Everything that is pure engine/game code: no window, no GL device, no
# sockets, no CD.  This is what has to compile on MIPS before anything can
# be played.
DESKTOP_ONLY := main_sdl.o sys_sdl_unix.o pl_linux.o in_sdl.o snd_sdl.o \
                gl_vidsdl.o net_bsd.o net_udp.o cd_sdl.o

# net_dgrm.c is the UDP datagram driver.  pspsdk's socket headers are IPv4-only
# (no struct sockaddr_in6) while the driver speaks both protocols unconditionally,
# so it cannot build here.  A PSP-E100x has no wireless hardware at all, so
# nothing is lost; a replacement net_psp.c lands with the platform layer.
PSP_UNSUPPORTED := net_dgrm.o

CORE_OBJS := $(filter-out $(DESKTOP_ONLY) $(PSP_UNSUPPORTED),$(OBJS))

# ---------------------------
# core compile check
# ---------------------------

CORE_DIR := $(BUILD)/core
CORE_OUT := $(addprefix $(CORE_DIR)/,$(CORE_OBJS))

all: core-objs

core-objs: $(CORE_OUT)

$(CORE_DIR)/%.o: $(ENGINE)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

# ---------------------------
# tests
# ---------------------------

# Tests that need no files, no video and no network, so they can run on a
# bare PSP.  Excluded on purpose:
#   test_skate_client  -- pulls gl_rmain.c, i.e. the GL front end
#   test_wad3          -- needs a .wad on the memstick
#   test_bind_defaults -- scans the repo on the build host (REPO_DIR)
TESTS_ALL := cigarette_impulse cigarette_smoke skate_server crouch_server \
             pmove_crouch pmove_physents speed_info use_button

# Engine objects each test links in (same sets as tests/run_gameplay_tests.sh).
TEST_DEPS_cigarette_impulse := sv_user mathlib
TEST_DEPS_cigarette_smoke   := chase
TEST_DEPS_skate_server      := sv_user sv_phys mathlib
TEST_DEPS_crouch_server     := sv_user
TEST_DEPS_pmove_crouch      := pmove mathlib
TEST_DEPS_pmove_physents    := pmove mathlib
TEST_DEPS_speed_info        := sv_user mathlib
TEST_DEPS_use_button        := sv_user mathlib

TEST_EBOOTS :=

define TEST_template

$(1)_DIR := $$(BUILD)/tests/$(1)

$(1)_OBJS := $$($(1)_DIR)/psp_boot.o \
             $$($(1)_DIR)/psp_console.o \
             $$(addprefix $$($(1)_DIR)/,$$(addsuffix .o,$(TEST_DEPS_$(1)))) \
             $$($(1)_DIR)/$(1).o

$$($(1)_DIR)/%.o: $(ENGINE)/%.c
	@mkdir -p $$(dir $$@)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $$< -o $$@

$$($(1)_DIR)/%.o: $(HERE)/%.c
	@mkdir -p $$(dir $$@)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $$< -o $$@

$$($(1)_DIR)/$(1).o: $(TESTS)/test_$(1).c
	@mkdir -p $$(dir $$@)
	$(CC) $(CFLAGS) $(CPPFLAGS) $(TEST_CPPFLAGS) -c $$< -o $$@

$$($(1)_DIR)/$(1).elf: $$($(1)_OBJS)
	$(CC) -o $$@ $$^ $(LDFLAGS) $(TEST_LIBS)
	$(FIXUP) $$@

$$($(1)_DIR)/PARAM.SFO:
	$(MKSFO) -d MEMSIZE=2 'kabluk $(1)' $$@

$$($(1)_DIR)/EBOOT.PBP: $$($(1)_DIR)/$(1).elf $$($(1)_DIR)/PARAM.SFO
	$(STRIP) $$($(1)_DIR)/$(1).elf -o $$($(1)_DIR)/data.psp
	$(PACKPBP) $$@ $$($(1)_DIR)/PARAM.SFO NULL NULL NULL NULL NULL $$($(1)_DIR)/data.psp NULL

TEST_EBOOTS += $$($(1)_DIR)/EBOOT.PBP

endef

$(foreach t,$(TESTS_ALL),$(eval $(call TEST_template,$(t))))

tests: $(TEST_EBOOTS)

# ---------------------------
# game: the single runnable EBOOT
# ---------------------------

# Platform backends that replace the desktop ones.  Same grouping objs.mk
# describes: main(), system, input, sound device, CD, video/GL device, network.
# psp_boot.o carries PSP_MODULE_INFO and psp_console.o the on-screen console;
# without them the module has no header and psp_con_* stay undefined.
PSP_BACKEND := psp_boot.o psp_console.o \
               main_psp.o sys_psp.o pl_psp.o in_psp.o snd_psp.o cd_psp.o \
               vid_psp.o gl_stub.o sdl_stub.o net_psp.o

GAME_ENGINE_OBJS := $(filter-out $(DESKTOP_ONLY) $(PSP_UNSUPPORTED),$(OBJS))
GAME_DIR    := $(BUILD)/game
GAME_OBJS   := $(addprefix $(GAME_DIR)/engine/,$(GAME_ENGINE_OBJS)) \
               $(addprefix $(GAME_DIR)/port/,$(PSP_BACKEND))

$(GAME_DIR)/engine/%.o: $(ENGINE)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

$(GAME_DIR)/port/%.o: $(HERE)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

# The flags themselves are a dependency: changing what we link (dropping SDL2,
# say) has to force a relink, otherwise make looks at the timestamps of the
# .o files, sees them older than the existing .elf, and says "nothing to be
# done" while the stale binary sits there.
$(GAME_DIR)/$(TARGET).elf: $(GAME_OBJS) $(MAKEFILE_LIST) $(HERE)/keep_sce.ld
	$(CC) -o $@ $^ $(LDFLAGS) $(GAME_LDFLAGS) $(GAME_LIBS)
	$(FIXUP) $@

$(GAME_DIR)/PARAM.SFO:
	@mkdir -p $(GAME_DIR)
	$(MKSFO) -d MEMSIZE=2 'Kabluk' $@

$(GAME_DIR)/EBOOT.PBP: $(GAME_DIR)/$(TARGET).elf $(GAME_DIR)/PARAM.SFO
	$(STRIP) $(GAME_DIR)/$(TARGET).elf -o $(GAME_DIR)/data.psp
	$(PACKPBP) $@ $(GAME_DIR)/PARAM.SFO NULL NULL NULL NULL NULL $(GAME_DIR)/data.psp NULL

game: $(GAME_DIR)/EBOOT.PBP

# ---------------------------
# housekeeping
# ---------------------------

check:
	@echo "PSPDEV  = $(PSPDEV)"
	@echo "PSPSDK  = $(PSPSDK)"
	@for t in $(CC) $(FIXUP) $(STRIP) $(MKSFO) $(PACKPBP); do \
		if [ -x "$$t" ] || [ -x "$$t.exe" ]; then echo "  ok   $$t"; \
		else echo "  MISSING $$t"; fi; \
	done
	@if [ -f "$(PSPSDK)/lib/build.mak" ]; then echo "  ok   $(PSPSDK)/lib/build.mak"; \
	else echo "  MISSING $(PSPSDK)/lib/build.mak"; fi
	@if [ -f "$(HERE)/keep_sce.ld" ]; then echo "  ok   $(HERE)/keep_sce.ld"; \
	else echo "  MISSING $(HERE)/keep_sce.ld"; fi
	@for sect in .sceStub.text .rodata.sceNid; do \
		if grep -q "$$sect" "$(HERE)/keep_sce.ld" 2>/dev/null; then echo "  ok   KEEP $$sect"; \
		else echo "  MISSING KEEP $$sect in keep_sce.ld"; fi; \
	done
	@for l in $(GAME_LIB_NAMES); do \
		n=$${l#-l}; found=; \
		for d in $(LIBDIR); do \
			for e in a so; do \
				if [ -f "$$d/lib$$n.$$e" ]; then found=1; break 2; fi; \
			done; \
		done; \
		if [ -n "$$found" ]; then echo "  ok   lib$$n"; \
		else echo "  MISSING lib$$n   (нет ни lib$$n.a, ни lib$$n.so в $(LIBDIR))"; fi; \
	done

# Prints what the toolchain actually installed, so a missing archive is one
# command away instead of a build error to read backwards.
libs:
	@for d in $(LIBDIR); do \
		echo "== $$d"; \
		ls "$$d"/*.a 2>/dev/null | xargs -n1 basename 2>/dev/null | sort | tr '\n' ' '; \
		echo; \
	done

clean:
	rm -rf $(BUILD)

.PHONY: all core-objs tests game check libs clean
