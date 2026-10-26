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


# gnu11, not the compiler default: GCC 15 defaults to gnu23, where true and
# false are keywords, and q_stdinc.h declares them as enum constants.
CFLAGS += -std=gnu11
CFLAGS += -O2 -G0 -Wall
CFLAGS += -Wno-unused-variable -Wno-unused-but-set-variable
CFLAGS += -fno-strict-aliasing
# One section per function: lets the linker drop the parts of the engine a
# given target does not reach (the renderer, for the test builds).
CFLAGS += -ffunction-sections -fdata-sections

LDFLAGS += -Wl,--gc-sections
LDFLAGS += -Wl,-zmax-page-size=128

# PSP user-mode libraries.  -lpspdebug is what our console prints through.
PSP_LIBS := -lpspdebug -lpspdisplay -lpspge -lpspctrl
TEST_LIBS := $(PSP_LIBS) -lm

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

clean:
	rm -rf $(BUILD)

.PHONY: all core-objs tests check clean
