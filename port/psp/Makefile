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
# The port's own headers (gl_shim.h, gu_psp.h, tex_arena.h, ...) are included
# by plain name from the sources in this directory.
CPPFLAGS += -I$(HERE)
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

# --gc-sections for the TEST links only.  Each test is one translation unit
# that exercises part of sv_user.c/pmove.c; the rest of those files is dead code
# whose references (svs, Cmd_*, MSG_*, Sys_Error, ...) would otherwise have to
# be satisfied by linking half the engine.  The desktop test script collects
# the same way, so the two builds agree.
#
# It does eat the NID table, though: .rodata.sceNid is only reached by the
# loader, never by code, so nothing keeps it alive and psp-fixup-imports then
# aborts.  keep_sce.ld re-adds both halves with KEEP().
#
# The GAME does not use either.  psp-fixup-imports walks .sceStub.text and
# .rodata.sceNid in lockstep and expects each import library's stubs to stay
# together; the KEEP overlay puts every stub in one output section of ours,
# which lets the sections of different libraries interleave once more than a
# couple of SDK archives are involved.  The fixup then gives up halfway:
#
#	Warning: could not fixup imports, stubs out of order.
#	Continuing, your binary may or not work.
#
# "May or not work" turned out to be "crashes a few seconds into Host_Init".
# The game links every object explicitly, so nothing needs collecting -- the
# plain link leaves the stubs exactly where pspsdk put them, which is the
# configuration every working pspsdk EBOOT uses.
TEST_GC_LDFLAGS := -Wl,--gc-sections -Wl,-T,$(HERE)/keep_sce.ld
LDFLAGS += -Wl,-zmax-page-size=128

# PSP user-mode libraries.  -lpspdebug is what our console prints through.
# sceKernel* (DelayThread, ExitGame) live in libpspkernel, which pspsdk's
# build.mak does not add for user-mode programs.
PSP_LIBS := -lpspdebug -lpspdisplay -lpspge -lpspctrl -lpspkernel
# -lpsppower for scePowerTick(), which psp_console.c's watchdog calls once a
# second (see the note there: without it the firmware is free to switch the
# unit off as soon as the engine stops feeding the display).  The GAME link
# already names -lpsppower further down; the test EBOOTs also contain
# psp_console.o, so they need it too.
TEST_LIBS := $(PSP_LIBS) -lpsppower -lm
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
# No -lpspkernel: libpspkernel.a is built from src/kernel/*, i.e. the
# *ForKernel import stubs, and those carry the same symbol names as the
# user-mode ones.  Asking for sceIoOpen, sceIoRead, sceKernelDelayThread with
# -lpspkernel on the command line gets the kernel stub, the import table then
# names ThreadManForKernel / IoFileMgrForKernel / LoadExecForKernel /
# ModuleMgrForKernel, and a user-mode EBOOT may not link any of those:
#
#	Запуск игры невозможен (8002013C)  = SCE_KERNEL_ERROR_LIBRARY_NOTFOUND
#
# libpspuser.a (src/user/*) is the user-mode half with the ForUser NIDs.  It is
# NOT named here, and that is the fix for the "stubs out of order" warning.
#
# The compiler appends the SDK's own libraries after anything on the command
# line, and that set includes libpspuser.a and libpspsdk.a.  Naming -lpspuser
# ourselves therefore put the archive on the link line twice, and an archive
# scanned twice yields its members in two separate bursts.  The import stubs
# travel with the members, so the same library's stubs ended up on both sides
# of other libraries' stubs -- exactly what the verbose dump showed:
#
#     7  IoFileMgrForUser
#     5  sceNetInet        <- libpspsdk, pulled by the compiler's own list
#     6  IoFileMgrForUser  <- back to the same library: split
#     2  SysMemUserForUser ... 3 SysMemUserForUser
#     4  ThreadManForUser ... 15 ThreadManForUser
#
# Three libraries split, three warnings.  psp-fixup-imports records the first
# run's start address and count in the module's import table, so the loader
# walks past the end of that run and patches other libraries' stubs -- the
# EBOOT loads and dies a few seconds later.
#
# Leaving -lpspuser to the compiler gives it a single scan.  Everything below is
# still needed: build.mak names these explicitly, so the compiler does not.
GAME_LIB_NAMES := -lm -lz -lpspvfpu -lpsphprm -lpspctrl -lpspumd -lpsprtc \
                  -lpsppower -lpspgum -lpspgu -lpspaudiolib -lpspaudio \
                  -lpspdebug -lpspdisplay -lpspge

# ...and no -Wl,--start-group, which is what produced
#
#	Warning: could not fixup imports, stubs out of order.
#
# A group makes ld rescan every archive until nothing is left to resolve.  That
# is normally a convenience, but here it means a library's members get pulled
# in several separate bursts with other libraries' members pulled in between,
# and pspsdk's import stubs travel with their members.  psp-fixup-imports walks
# .sceStub.text and .rodata.sceNid in lockstep and requires each library's
# stubs to be contiguous; when they are not it skips the fixup and leaves a
# "jr $31" with no address patched in -- a call into nowhere.
#
# A single left-to-right pass pulls each library's members in one burst, so the
# stubs stay together, exactly as in a stock pspsdk EBOOT.  If the order above
# ever leaves a symbol unresolved, the fix is to move that library left, not to
# put the group back.
GAME_LIBS := $(GAME_LIB_NAMES)

# No --allow-multiple-definition here any more.  It was hiding the duplicate
# strtol above, and what it hid was a kernel-library import that made the EBOOT
# unloadable.  With -lc first there is no duplicate to allow, and if another
# one ever appears we want the link to fail loudly.
# --wrap=malloc: every undefined reference to malloc in the engine, the shim and
# the picture decoder is rewritten to __wrap_malloc (psp_heap.c), which calls
# the real allocator and, when it answers NULL, writes one line to kabluk.log
# naming the size and the last breadcrumb.  A NULL nothing checks is a write
# through address 0, and this console reports that by switching itself off.
# -lm and the SDK libraries keep working: only references that are still
# undefined at link time are rewritten, and __wrap_malloc calls __real_malloc.
GAME_LDFLAGS := -Wl,--wrap=malloc

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
             pmove_crouch pmove_physents speed_info use_button tex_arena

# Engine objects each test links in (same sets as tests/run_gameplay_tests.sh).
TEST_DEPS_cigarette_impulse := sv_user mathlib
TEST_DEPS_cigarette_smoke   := chase
TEST_DEPS_skate_server      := sv_user sv_phys mathlib
TEST_DEPS_crouch_server     := sv_user
TEST_DEPS_pmove_crouch      := pmove mathlib
TEST_DEPS_pmove_physents    := pmove mathlib
TEST_DEPS_speed_info        := sv_user mathlib
TEST_DEPS_use_button        := sv_user mathlib
# The picture allocator: no engine objects at all, just port/psp/tex_arena.c.
TEST_DEPS_tex_arena         :=

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
	$(CC) -o $$@ $$^ $(LDFLAGS) $(TEST_GC_LDFLAGS) $(TEST_LIBS)
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
PSP_BACKEND := psp_boot.o psp_console.o psp_heap.o \
               main_psp.o sys_psp.o pl_psp.o in_psp.o snd_psp.o cd_psp.o \
               vid_psp.o gu_psp.o gl_shim.o tex_arena.o gl_stub.o sdl_stub.o \
               net_psp.o

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
#
# The command spells out $(GAME_OBJS) rather than using $^, because $^ is every
# prerequisite -- including the Makefile and the linker fragment, which the
# linker would then try to read as object files.
$(GAME_DIR)/$(TARGET).elf: $(GAME_OBJS) $(MAKEFILE_LIST) $(HERE)/keep_sce.ld
	$(CC) -o $@ $(GAME_OBJS) $(LDFLAGS) $(GAME_LDFLAGS) $(GAME_LIBS)
	@bad=`grep -ao "[A-Za-z_0-9]*ForKernel" $@ | sort -u`; \
	if [ -n "$$bad" ]; then \
		echo "ERROR: модуль импортирует kernel-библиотеку:"; \
		echo "$$bad" | sed "s/^/    /"; \
		echo "Такой EBOOT прошивка не запустит (8002013C = LIBRARY_NOTFOUND)."; \
		echo "Причина: в списке библиотек есть -lpspkernel, и он перехватывает"; \
		echo "символы sce* раньше libpspuser.a. Для user-модуля нужен -lpspuser."; \
		rm -f $@; exit 1; \
	fi
	@cp $@ $@.pre; \
	$(FIXUP) -v $@.pre > $@.vlog 2>&1; \
	out=`$(FIXUP) $@ 2>&1`; \
	if [ -n "$$out" ]; then echo "$$out"; fi; \
	if echo "$$out" | grep -q "could not fixup imports"; then \
		echo ""; \
		echo "ERROR: psp-fixup-imports не смог дописать часть стабов"; \
		echo "стабы одной библиотеки идут не подряд по таблице импорта."; \
		echo ""; \
		echo "Порядок библиотек в таблице импорта (соседние одинаковые"; \
		echo "свёрнуты в строку N имя). Имя, встретившееся дважды,"; \
		echo "это библиотека, чьи стабы разъехались:"; \
		awk '/Import Stub/ { split($$0, a, "("); split(a[2], b, ")"); print b[1] }' \
			$@.vlog | uniq -c | awk '{ printf "    %-3s %s\n", $$1, $$2 }'; \
		echo ""; \
		echo "Первые строки подробного лога (для контекста):"; \
		head -12 $@.vlog | sed "s/^/    /"; \
		rm -f $@ $@.pre $@.vlog; exit 1; \
	fi; \
	rm -f $@.pre $@.vlog

# MEMSIZE is the extra-memory request: without it the user partition is 24 MB,
# with it (value 1 = "give me everything") a PSP-2000/3000/E1000 hands out
# around 52 MB.  The game needs it: its data files alone are 64 MB, and while
# Quake streams from the packs instead of loading them whole, the zone that
# holds the loaded models, sounds and maps has to be far bigger than 16 MB.
# Whether it is granted is the firmware's call, so the code must cope either
# way -- see the zone sizes tried in main_psp.c.
$(GAME_DIR)/PARAM.SFO:
	@mkdir -p $(GAME_DIR)
	$(MKSFO) -d MEMSIZE=1 'Kabluk' $@

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
