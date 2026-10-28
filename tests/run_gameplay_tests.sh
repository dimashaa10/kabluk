#!/bin/sh
# Asset-free tests using real input, physics, camera and alias-transform code.
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT HUP INT TERM
cat > "$work/SDL_config_minimal.h" <<'EOF'
#ifndef SDL_config_minimal_h_
#define SDL_config_minimal_h_
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#define HAVE_STDARG_H 1
#define HAVE_STDDEF_H 1
#define HAVE_STDINT_H 1
#endif
EOF
ln -s "$repo/Quake/cJSON.h" "$work/cjson.h"
ln -s "$repo/Quake/cJSON_Utils.h" "$work/cjson_utils.h"
cc=${CC:-cc}
compile() {
    "$cc" ${CFLAGS:-} -std=gnu11 -Wall -Wno-missing-field-initializers -DUSE_SDL2 \
        -I"$repo/Quake" -I"$repo/Windows/SDL2/include" -I"$repo/Windows/zlib/include" \
        -I"$repo/Windows/curl/include" -I"$work" -ffunction-sections -fdata-sections "$@"
}
for source in sv_user sv_phys mathlib chase gl_rmain pmove; do
    compile -c "$repo/Quake/$source.c" -o "$work/$source.o"
done
compile "$repo/tests/test_cigarette_impulse.c" "$work/sv_user.o" "$work/mathlib.o" \
    ${LDFLAGS:-} -Wl,--gc-sections -lm -o "$work/cigarette_impulse"
"$work/cigarette_impulse"
compile "$repo/tests/test_cigarette_smoke.c" "$work/chase.o" \
    ${LDFLAGS:-} -Wl,--gc-sections -lm -o "$work/cigarette_smoke"
"$work/cigarette_smoke"
compile "$repo/tests/test_skate_server.c" "$work/sv_user.o" "$work/sv_phys.o" "$work/mathlib.o" \
    ${LDFLAGS:-} -Wl,--gc-sections -lm -o "$work/skate_server"
"$work/skate_server"
compile "$repo/tests/test_crouch_server.c" "$work/sv_user.o" \
    ${LDFLAGS:-} -Wl,--gc-sections -lm -o "$work/crouch_server"
"$work/crouch_server"
# The pre-existing renderer has an unused model_matrix local unrelated to these tests.
compile -Wno-unused-variable "$repo/tests/test_skate_client.c" "$work/chase.o" "$work/mathlib.o" "$work/gl_rmain.o" \
    ${LDFLAGS:-} -Wl,--gc-sections -lm -o "$work/skate_client"
"$work/skate_client"
# Shared engine-side crouch: the same pmove code runs on the server and in
# client-side prediction, so the test drives both from one snapshot.
compile "$repo/tests/test_pmove_crouch.c" "$work/pmove.o" "$work/mathlib.o" \
    ${LDFLAGS:-} -Wl,--gc-sections -lm -o "$work/pmove_crouch"
"$work/pmove_crouch"
# Collision-entity list size and bookkeeping (busy maps used to truncate it).
compile "$repo/tests/test_pmove_physents.c" "$work/pmove.o" "$work/mathlib.o" \
    ${LDFLAGS:-} -Wl,--gc-sections -lm -o "$work/pmove_physents"
"$work/pmove_physents"
# HUD speedometer feed, including the pmove path that used to leave it at -1.
compile "$repo/tests/test_speed_info.c" "$work/sv_user.o" "$work/mathlib.o" \
    ${LDFLAGS:-} -Wl,--gc-sections -lm -o "$work/speed_info"
"$work/speed_info"
# Engine action key (+use): what it may press and how often.
compile "$repo/tests/test_use_button.c" "$work/sv_user.o" "$work/mathlib.o" \
    ${LDFLAGS:-} -Wl,--gc-sections -lm -o "$work/use_button"
"$work/use_button"
# Controls menu / default bindings: +use must be bindable and default to E.
compile -DREPO_DIR=\"$repo\" "$repo/tests/test_bind_defaults.c" \
    ${LDFLAGS:-} -Wl,--gc-sections -lm -o "$work/bind_defaults"
"$work/bind_defaults"
# The picture allocator (port/psp/tex_arena.c): no engine code, no assets.
compile -I"$repo/port/psp" "$repo/tests/test_tex_arena.c" "$repo/port/psp/tex_arena.c" \
    ${LDFLAGS:-} -Wl,--gc-sections -o "$work/tex_arena"
"$work/tex_arena"
# The PSP pad (port/psp/pad_map.c): every button is an engine key, so the
# engine's own Controls -> Keys screen can rebind it, and holding a direction
# repeats it the way Quake/in_sdl.c repeats a controller's.  This is plain C
# precisely so it can be tested without a console.
compile -I"$repo/port/psp" "$repo/tests/test_pad_map.c" "$repo/port/psp/pad_map.c" \
    ${LDFLAGS:-} -Wl,--gc-sections -lm -o "$work/pad_map"
"$work/pad_map"
# The pad is polled from IN_Commands(), which only Host_Frame calls -- and
# SCR_ModalMessage()'s yes/no dialog and the "press a key" state run their own
# loops, outside Host_Frame, waiting for a key event that nothing would ever
# send.  On the hardware that was a freeze in the single-player and load/save
# menus (power off to recover).  Sys_SendKeyEvents() is the hook the desktop
# backends use for exactly this, and it has to stay wired to the pad.
grep -q 'IN_Commands ();' port/psp/sys_psp.c
grep -q 'void Sys_SendKeyEvents (void)' port/psp/sys_psp.c
grep -q 'pad_map.o' port/psp/Makefile
grep -q 'Key_Event (k->key, true);' port/psp/in_psp.c
# A pad button must not send a keyboard key of its own (Cross was K_ENTER and
# Circle was K_ESCAPE): those are not rebindable as controller buttons and the
# engine's menus do not treat them as confirm/back.  The only keyboard names
# allowed in the input layer are the ones the table gives the directions.
if grep -qE 'Key_Event \((K_ENTER|K_ESCAPE|K_SPACE|K_TAB)' port/psp/in_psp.c; then
    echo "FAIL: in_psp.c sends a keyboard key directly again -- the button must"
    echo "FAIL: go through the pad table so Controls -> Keys can rebind it"
    exit 1
fi
grep -q '_Static_assert(PAD_BTN_CROSS    == PSP_CTRL_CROSS' port/psp/in_psp.c
# The pad's defaults must be applied AFTER the config has been read, and only
# to keys that are still unbound.  `exec default.cfg` finds the pak's own copy
# first, and that file starts with `unbindall` -- so a default compiled into
# Quake/default_cfg.h would be undone a line later, and a default applied
# unconditionally would overwrite the player's own choice on every boot.
grep -q 'IN_ApplyPadDefaults();' port/psp/main_psp.c
grep -q 'if (keybindings\[0\]\[key\] || keybindings\[1\]\[key\])' port/psp/in_psp.c
if grep -q 'ABUTTON' Quake/default_cfg.h; then
    echo "FAIL: a pad button is bound in default_cfg.h again -- the pak's"
    echo "FAIL: default.cfg says 'unbindall' and would undo it immediately"
    exit 1
fi
echo "PASS: the pad's buttons are engine keys checked against <pspctrl.h>, the"
echo "PASS: menus' confirm/back are the keys they expect, and the pad is polled"
echo "PASS: from Sys_SendKeyEvents() so the engine's modal dialogs can end."
compile -c "$repo/Quake/wad.c" -o "$work/wad.o"
compile "$repo/tests/test_wad3.c" "$work/wad.o" \
    ${LDFLAGS:-} -Wl,--gc-sections -o "$work/wad3"
"$work/wad3"
# A macro defined below the code that uses it silently deletes that code, with
# no warning and no error: the picture-cache diagnostics in gl_draw.c were
# compiled out for several rounds that way, and every hardware log then showed
# "0 idle pictures unloaded" with nothing explaining it.  This check keeps the
# two in the right order.
awk '
  /^#[[:space:]]*define[[:space:]]+(PSP_NOTE|DRAW_MARK|PSP_CACHEPIC_DIAG)/ {
      name = $2; sub(/\(.*/, "", name); seen[name] = 1
  }
  /^[[:space:]]*PSP_NOTE[[:space:]]*\(/ {
      if (!("PSP_NOTE" in seen)) { printf "FAIL: PSP_NOTE used at line %d of gl_draw.c before it is defined\n", NR; bad = 1 }
  }
  /^#[[:space:]]*ifdef[[:space:]]+PSP_CACHEPIC_DIAG/ {
      if (!("PSP_CACHEPIC_DIAG" in seen)) { printf "FAIL: PSP_CACHEPIC_DIAG tested at line %d of gl_draw.c before it is defined\n", NR; bad = 1 }
  }
  END { exit (bad ? 1 : 0) }
' "$repo/Quake/gl_draw.c"
echo "PASS: the picture-cache diagnostics are defined above the code that uses them."
# The picture decoder's "ask the cache for memory" path crosses three files:
# image.c calls psp_gl_shim_reclaim_pictures(), gl_shim.h promises it and
# gl_shim.c implements it.  A missing or mismatched piece is a link error on
# the user's Windows build -- i.e. a whole round spent on a symbol -- and a
# macro defined below the code that uses it is the same silent deletion as
# above.  Both are cheap to check here.
awk '
  /^#[[:space:]]*define[[:space:]]+IMG_ALLOC[[:space:]]*\(/ { seen = 1 }
  /[^_[:alnum:]]IMG_ALLOC[[:space:]]*\(/ {
      if (!seen) { printf "FAIL: IMG_ALLOC used at line %d of image.c before it is defined\n", NR; bad = 1 }
  }
  END { exit (bad ? 1 : 0) }
' "$repo/Quake/image.c"
grep -q 'int psp_gl_shim_reclaim_pictures (void);' "$repo/port/psp/gl_shim.h"
grep -q '^int psp_gl_shim_reclaim_pictures (void)$' "$repo/port/psp/gl_shim.c"
grep -q 'psp_gl_shim_reclaim_pictures (void)' "$repo/Quake/image.c"
echo "PASS: the decoder's reclaim path is declared, defined and used, and IMG_ALLOC"
echo "PASS: is defined before the allocations that use it."
# The port's memory numbers were zeros for the whole life of the port because
# newlib's accounting was referenced WEAKLY: a weak undefined reference does
# not pull an object out of libc.a, so mallinfo resolved to 0 and every log
# line read "in use 0 KB, free 0 KB of 0 KB".  The reference has to stay
# strong, and the watchdog's once-a-second status line has to stay wired to
# the stack measurement, or the crash hunt is blind again.
if grep -q '_mallinfo_r (struct _reent \*) __attribute__((weak))' \
        "$repo/port/psp/psp_heap.c"; then
    echo "FAIL: psp_heap.c makes _mallinfo_r weak again -- every heap number is zero"
    exit 1
fi
grep -q 'extern struct mallinfo _mallinfo_r (struct _reent \*);' "$repo/port/psp/psp_heap.c"
grep -q 'mi = _mallinfo_r(_REENT);' "$repo/port/psp/psp_heap.c"
grep -q 'int psp_heap_stack_free (void)' "$repo/port/psp/psp_heap.c"
grep -q 'void psp_heap_note_main_thread (void)' "$repo/port/psp/psp_heap.c"
grep -q 'sceKernelGetThreadStackFreeSize' "$repo/port/psp/psp_heap.c"
grep -q 'status: phase %.47s' "$repo/port/psp/psp_console.c"
grep -q 'psp_heap_stack_free()' "$repo/port/psp/psp_console.c"
grep -q 'psp_heap_note_main_thread' "$repo/port/psp/main_psp.c"
echo "PASS: the heap accounting is linked in and the watchdog's status line"
echo "PASS: reports the main thread's free stack every second."
# The malloc pool was empty before the first menu was drawn:
#
#     heap: after Host_Init: in use 34430 KB, free 9 KB of 34439 KB in 1 chunks
#
# and the first attempt at reserving memory for it did not fix that, it made it
# worse: the reserve was allocated, the zone was chosen, the reserve was freed,
# and the next allocation failed with
#
#     malloc pool: 5 KB free of 36870 KB in 1 chunks
#
# because on this platform the newlib heap is ONE partition block (pspsdk
# libcglue/glue.c, _sbrk()) and free() of a big block hands the top of it back to
# the system through malloc_trim() and MORECORE(-n) for good.  So the rule the
# engine itself uses (Quake/zone.c: one Hunk_Alloc block) is the rule here too:
# measure the pool, take ONE block for the zone, never free anything big.
if grep -q 'PSP_MALLOC_RESERVE' port/psp/main_psp.c; then
    echo "FAIL: main_psp.c holds a malloc reserve again -- free() of a big block"
    echo "FAIL: gives the top of the heap away on this platform and never gets it back"
    exit 1
fi
if grep -qE 'want[[:space:]]*=[[:space:]]*psp_zone_sizes_mb' port/psp/main_psp.c; then
    echo "FAIL: the zone size is a table of guesses again, not a measurement"
    exit 1
fi
# The memory layout is one file, and it is built on the firmware's allocator
# rather than on newlib's.  Three builds in a row died the other way:
#
#   heap: after Host_Init: in use 34430 KB, free 9 KB of 34439 KB   (zone ate the heap)
#   malloc pool: 5 KB free of 36870 KB after free(reserve)          (free() never gives back)
#   heap ceiling: 0 MB -> Zone: 8 MB -> Hunk_Alloc: failed          (wrong number)
#
# pspsdk src/libcglue/glue.c _sbrk() explains all three: newlib's heap is ONE
# block, sized at the first malloc before main(), never grown, and never given
# back.  So the zone and the picture arena are taken from the partition with
# sceKernelAllocPartitionMemory() and the heap is a fixed size.
for f in port/psp/psp_mem.c port/psp/psp_mem.h; do
    if [ ! -f "$f" ]; then
        echo "FAIL: $f is missing -- the memory layout has no owner again"
        exit 1
    fi
done
grep -q 'sceKernelAllocPartitionMemory(PSP_MEMORY_PARTITION_USER' port/psp/psp_mem.c
grep -q 'sceKernelGetBlockHeadAddr' port/psp/psp_mem.c
# 14 MB since round 71: the round-71 log had the hunk 11 MB free while malloc()
# returned NULL for 5.75 MB, i.e. the split was wrong rather than the total.
grep -q 'PSP_HEAP_SIZE_KB(14 \* 1024)' port/psp/psp_boot.c
if grep -q 'PSP_HEAP_SIZE_KB(-1)' port/psp/psp_boot.c; then
    echo "FAIL: the newlib heap takes the whole partition again (-1) -- nothing"
    echo "FAIL: else can then be sized from what is left"
    exit 1
fi
# No more measuring by probing, and no more guessing tables: both were attempts
# to work out, from inside a fixed block, how much memory exists.
if grep -q 'psp_heap_room' port/psp/*.c; then
    echo "FAIL: the sbrk() probing is back -- the layout must come from psp_mem.c"
    exit 1
fi
grep -q 'psp_mem_init' port/psp/main_psp.c
grep -q 'psp_mem_zone' port/psp/main_psp.c
grep -q 'psp_gl_shim_set_pool(pics, pic_bytes);' port/psp/main_psp.c
grep -q 'psp_mem.o' port/psp/Makefile
# mallinfo is not a memory map, and reading it as one cost this port three
# rounds.  A healthy run printed
#
#     heap: after Host_Init: in use 2166 KB, free 4 KB of 2170 KB in 1 chunks
#
# which looks like a heap about to die: the allocator had simply not grown past
# 2170 KB of its 10 MB block, and `arena` (pspsdk newlib _mallinfo_r) is only
# what it has sbrk()ed so far.  The size of the block is not a secret --
# PSP_HEAP_SIZE_KB() defines `sce_newlib_heap_kb_size`, _sbrk() reads it -- so
# "how much can malloc() still reach" is block minus arena, and both the boot
# line and the per-frame line must say that instead of mallinfo's free.
grep -q 'sce_newlib_heap_kb_size' port/psp/psp_heap.c
grep -q 'psp_heap_block_left' port/psp/psp_heap.c
grep -q 'psp_heap_block_left' port/psp/psp_mem.c
grep -q 'psp_heap_block_left' port/psp/main_psp.c
grep -q 'psp_heap_block_left' Quake/image.c
if grep -q 'psp_heap_free (void);' Quake/image.c; then
    echo "FAIL: image.c measures the picture reclaim by mallinfo's free again --"
    echo "FAIL: that number stays at 0 KB on a healthy heap and the line reads"
    echo "FAIL: 'released 3 pictures, heap 4 -> 4 KB'"
    exit 1
fi
echo "PASS: the heap is reported as used-of-block with the rest reachable, not as"
echo "PASS: mallinfo's free, which reads 0 KB whenever the allocator has not grown."
# The picture arena, when it owns a block, must never ask malloc for another
# chunk and never hand one back.
grep -q 'if (shim_arena_pooled)' port/psp/gl_shim.c
grep -q 'void psp_gl_shim_set_pool (void \*base, size_t bytes)' port/psp/gl_shim.c
echo "PASS: the zone and the picture pool come from the firmware partition,"
echo "PASS: the newlib heap is a fixed 10 MB, and the layout lives in psp_mem.c.
# A malloc() that returns NULL and is not checked is a write through address 0,
# and this console reports that by switching itself off -- no log line, no
# message.  --wrap=malloc is what turns that into "malloc: NULL for N bytes --
# last <breadcrumb>" in kabluk.log.  Losing the flag, or the hook, silently
# restores the blind spot, so both halves are asserted.
grep -q -- '-Wl,--wrap=malloc' port/psp/Makefile
grep -q -- '-Wl,--wrap=memalign' port/psp/Makefile
grep -q -- '-Wl,--wrap=realloc' port/psp/Makefile
grep -q 'void \*__wrap_malloc (size_t size)' port/psp/psp_heap.c
grep -q 'void \*__wrap_memalign (size_t align, size_t size)' port/psp/psp_heap.c
grep -q 'void \*__wrap_realloc (void \*p, size_t size)' port/psp/psp_heap.c
grep -q 'void \*__real_malloc (size_t size);' port/psp/psp_heap.c
grep -q 'void \*__real_memalign (size_t align, size_t size);' port/psp/psp_heap.c
grep -q 'psp_con_note_alloc_failure(what, (unsigned int)size,' port/psp/psp_heap.c
grep -q 'void psp_con_note_alloc_failure (const char \*what, unsigned int size,' port/psp/psp_console.c
grep -q 'void psp_con_note_alloc_failure (const char \*what, unsigned int size,' port/psp/psp_console.h
grep -q 'psp_heap_malloc_fails' port/psp/main_psp.c
echo "PASS: a malloc that fails is logged with its size and breadcrumb instead"
echo "PASS: of becoming a fault, and the count is on the heartbeat line."
# Every picture-drawing entry point has to survive the two NULLs the picture
# cache can hand it: a NULL qpic_t (a load that failed) and a pic->data that is
# not a glpic_t (an upload that failed).  Draw_Pic() has checked both since
# #democontrols; the others dereferenced them, and a fault there is the console
# switching off with nothing in the log.  Counted, not merely present, because
# the point is the set of functions and not one of them.
if [ "$(grep -c '(uintptr_t)gl < 0x1000' Quake/gl_draw.c)" -lt 5 ]; then
    echo "FAIL: a picture-drawing function lost its low-pointer guard in gl_draw.c"
    exit 1
fi
grep -q 'if (!draw_backtile)' Quake/gl_draw.c
grep -q 'if (!pic || (uintptr_t)pic->data < 0x1000)' Quake/gl_draw.c
echo "PASS: no picture-drawing entry point dereferences a missing picture."
# The animated Kabluk logo (gfx/a_kzlogo/logo1..24) is menu decoration that
# costs 24 pictures in the cache within three seconds of opening the menu --
# and NOT drawing a picture does not avoid loading it: the Draw_CachePic()
# call is what loads it.  So on the PSP the call itself has to be gone, which
# is why every banner site goes through M_DrawKzLogoStrip() and the main-menu
# logo is pinned to one frame.
awk '
  /^#if defined\(PLATFORM_PSP\)/ { inpsp = 1 }
  inpsp && /define MENU_KZLOGO_PSP/ {
      value = $NF; gsub(/[^0-9]/, "", value)
      if (value != "0") { printf "FAIL: MENU_KZLOGO_PSP is not 0 on the PSP\n"; bad = 1 }
      found = 1
  }
  inpsp && /^#else/ { inpsp = 0 }
  END { if (!found) { printf "FAIL: MENU_KZLOGO_PSP is not defined in menu.c\n"; bad = 1 }
        exit (bad ? 1 : 0) }
' Quake/menu.c || exit 1
if [ "$(grep -c 'M_DrawKzLogoStrip(' Quake/menu.c)" -lt 8 ]; then
    echo "FAIL: a menu banner still loads the logo itself instead of going"
    echo "FAIL: through M_DrawKzLogoStrip(), so the PSP would load its frames"
    exit 1
fi
# Exactly three requests for the sequence may survive: the two inside the
# helper and the animated one in M_DrawLogo.  A fourth means a call site was
# added without the guard.
if [ "$(grep -c 'gfx/a_kzlogo/logo%i' Quake/menu.c)" -gt 4 ]; then
    echo "FAIL: a menu still asks for gfx/a_kzlogo/logo%i directly"
    exit 1
fi
grep -q 'p = Draw_CachePic("gfx/a_kzlogo/logo1.png");' Quake/menu.c
echo "PASS: the animated logo is PC-only: the PSP loads no frames of it and"
echo "PASS: the main-menu logo is a single still picture."
# The spinning 3D previews are the same story, one step further: the drawing
# already returns immediately on the PSP (pr_ext.c), but the callers loaded
# gfx/david/tanec1.lmp (three menus) and gfx/menuplyr.lmp (the setup menu) just
# to size a rectangle that never appeared.  Those loads are the cost, so on the
# PSP the whole block -- picture included -- has to be gone.
awk '
  /^#define MENU_MODELPREVIEW_PSP/ || /^#[[:space:]]*define MENU_MODELPREVIEW_PSP/ {
      value = $NF; gsub(/[^0-9]/, "", value)
      if (inpsp && value != "0") { printf "FAIL: MENU_MODELPREVIEW_PSP is not 0 on the PSP\n"; bad = 1 }
      if (inpsp) seen_psp = 1
  }
  /^#if defined\(PLATFORM_PSP\)/ { inpsp = 1 }
  /^#else/ { inpsp = 0 }
  /^#endif/ { inpsp = 0 }
  /^#if MENU_MODELPREVIEW_PSP/ { guard = 1 }
  /DrawSpinningModelToMenuPixels[[:space:]]*\(/ {
      if (!guard) { printf "FAIL: line %d draws a 3D preview outside MENU_MODELPREVIEW_PSP\n", NR; bad = 1 }
  }
  /gfx\/(david\/tanec1|menuplyr)\.lmp/ && !/^[[:space:]]*\*/ {
      if (!guard) { printf "FAIL: line %d loads a preview picture outside MENU_MODELPREVIEW_PSP\n", NR; bad = 1 }
  }
  guard && /^#endif/ { guard = 0 }
  END { if (!seen_psp) { printf "FAIL: MENU_MODELPREVIEW_PSP is not defined for the PSP\n"; bad = 1 }
        exit (bad ? 1 : 0) }
' Quake/menu.c || exit 1
echo "PASS: the spinning model previews and their sizing pictures are PC-only;"
echo "PASS: on the PSP neither the model nor gfx/david/tanec1.lmp is looked at."

# The port's own diagnostics must not become the thing they measure.
#
# A menu session logged 2000+ lines and every one of them was an
# open/write/close on the memory stick, because psp_tracef() was psp_trace()
# with a format string and psp_trace() ends in con_log_sync().  gl_shim.c emits
# a window of up to thirty lines inside a single frame every sixty frames, so
# those were thirty memory-stick round trips inside one frame -- a stall the
# diagnostic invented, on the console whose stalls we are trying to explain.
# The console already batches into a 4 KB buffer that the watchdog thread
# drains once a second, which is the intended granularity.
tracef_body=$(awk '/^void psp_tracef /,/^}/' port/psp/psp_console.c)
if [ -z "$tracef_body" ]; then
    echo "FAIL: psp_tracef is gone from psp_console.c"
    exit 1
fi
# The exception is the menu, where the console dies without a word (see the
# psp_con_trace_sync test below): there the flag is on and a line is synced on
# purpose.  Any other sync -- one that is not behind that flag -- turns the
# diagnostic back into the stall it used to be.
if echo "$tracef_body" | grep -q 'con_log_sync' && \
   ! echo "$tracef_body" | grep -q 'if (con_trace_sync)'; then
    echo "FAIL: psp_tracef syncs the card per line again -- a 30-line window in"
    echo "FAIL: one frame becomes 30 memory-stick round trips (see psp_console.c)"
    exit 1
fi
if echo "$tracef_body" | grep -q 'psp_trace *( *buf'; then
    echo "FAIL: psp_tracef calls psp_trace() again, which syncs the card"
    exit 1
fi
grep -q 'con_log_sync();' port/psp/psp_console.c
# And the full-frame dump is one frame, not two: the second doubled 400 lines
# to 800 to show that the same menu draws the same thing twice.
grep -q 'shim_frames == 120)' port/psp/gl_shim.c
if grep -q 'shim_frames == 121' port/psp/gl_shim.c; then
    echo "FAIL: the full-frame dump covers two frames again -- half the log of a"
    echo "FAIL: menu session was this"
    exit 1
fi
echo "PASS: diagnostic lines are batched to the card, not synced one by one, and"
echo "PASS: the full-frame draw dump covers one frame."

# The reference card is gone.  It was a diagnostic: five squares and a black
# backdrop in the top-left corner that said which step of the GL->GE
# translation was wrong.  It has done its job (it is what named the texel
# coordinate bug), and a player should not see diagnostic squares in a menu.
# If a picture on the panel ever needs explaining again, it comes back as a
# piece of code -- not as something that ships in the frame loop.
if grep -q 'psp_gl_shim_test_card\|SHIM_CARD' port/psp/gl_shim.c port/psp/gl_shim.h port/psp/gu_psp.c; then
    echo "FAIL: the reference card is back in the frame (see port/README.md)"
    exit 1
fi
grep -q 'psp_gu_frame_end' port/psp/gu_psp.c
# What the card used to do by accident, and must now be done on purpose: the
# batch the shim is still holding has to reach the GE before sceGuFinish()
# closes the list, or the last pictures of every frame are never drawn.
awk '
  /^void psp_gu_frame_end/ { inf = 1 }
  inf && /psp_gl_shim_flush_batch *\(\);/ { flushed = NR }
  inf && /sceGuFinish \(\)/ { if (!flushed) { printf "FAIL: psp_gu_frame_end closes the draw list with a batch unflushed\n"; exit 1 } done = 1 }
  inf && /^}/ { inf = 0 }
  END { if (!flushed) { printf "FAIL: psp_gu_frame_end never flushes the batch\n"; exit 1 } }
' port/psp/gu_psp.c || exit 1
echo "PASS: the diagnostic reference card is not part of a frame any more, and"
echo "PASS: the last batch of a frame still reaches the GE before the list closes."

# Seventeen frames a second in a menu that draws 393 quads and 2358 vertices is
# not geometry, and the log named the places a frame can stop without ever
# saying how long it stopped there.  Three numbers, taken with
# sceKernelGetSystemTimeWide (pspsdk src/kernel/... -- a microsecond clock that
# does not wrap in a session), say which of the three it is: the engine's own
# drawing, the GE, or the panel's vblank.
grep -q 'sceKernelGetSystemTimeWide' port/psp/gu_psp.c
grep -q 'psp_gu_timing_take' port/psp/gu_psp.c
grep -q 'psp_gu_timing_take' port/psp/gu_psp.h
grep -q 'psp_gu_timing_take' port/psp/main_psp.c
grep -q 'vblank %.1f' port/psp/main_psp.c
if ! grep -q 'gu_t_vb_us += gu_now_us () - t_wait;' port/psp/gu_psp.c; then
    echo "FAIL: the vblank wait is not timed -- the one number that says"
    echo "FAIL: whether the frame is merely paced or actually slow"
    exit 1
fi
echo "PASS: a frame's time is split into engine drawing, GE and vblank, and the"
echo "PASS: heartbeat prints all three."
# A name with no file name in it cannot match a file, and probing
# "textures/.tga" and twenty other names on the memory stick to prove it is
# what the tail of the last log was doing instead of drawing.
awk '
  /^byte \*Image_LoadImage/ { infunc = 1 }
  infunc && /COM_SkipPath \(name\)\[0\]/ { guarded = 1 }
  infunc && /for \(i = 0; i < sizeof\(prefixes\)/ { before = guarded }
  END {
      if (!before) {
          printf "FAIL: Image_LoadImage probes file names for an empty name again\n"
          exit 1
      }
  }
' Quake/image.c || exit 1
echo "PASS: a picture name that names no file is refused before twenty-one file"
echo "PASS: names are tried on the memory stick."

# The console's text was drawn and then painted over, every frame, in the menu.
#
# The draw dump of frame 120 from the hardware is the proof: 392 draws, of which
# 382 are 8x8 glyph quads at the console's font atlas, and then draw 384 is the
# console background again -- the same full-panel picture, in the same place.
# That draw comes from M_Draw() (menu.c: "if (scr_con_current)
# Draw_ConsoleBackground()"), which runs right after SCR_DrawConsole() has drawn
# the background AND the text.  Every one of those 382 glyph draws is therefore
# invisible, and each one is its own glBegin()/glEnd() pair.
#
# On the PSP SCR_DrawConsole() now asks the menu whether it is about to paint
# over the console, and skips the text when the answer is yes.  On the PC the
# drawing is unchanged.
grep -q 'qboolean M_WillCoverConsole (void)' Quake/menu.c
grep -q 'M_WillCoverConsole' Quake/menu.h
grep -q 'M_WillCoverConsole' Quake/gl_screen.c
awk '
  /^void SCR_DrawConsole/ { infunc = 1 }
  infunc && /Con_DrawConsole \(scr_con_current, true\)/ {
      if (!guarded) {
          printf "FAIL: SCR_DrawConsole draws the console text without asking the\n"
          printf "FAIL: menu whether it is about to cover it -- 382 invisible glyph\n"
          printf "FAIL: draws per frame in the menu\n"
          exit 1
      }
      found = 1
  }
  infunc && /M_WillCoverConsole \(\)/ { guarded = 1 }
  infunc && /^}/ { infunc = 0 }
  END { if (!found) { printf "FAIL: Con_DrawConsole is not called from SCR_DrawConsole\n"; exit 1 } }
' Quake/gl_screen.c || exit 1
# The two branches M_Draw() takes: the answer must cover both, or the skip
# becomes a black screen where the console used to be in one of them.
awk '
  /^qboolean M_WillCoverConsole/ { infunc = 1 }
  infunc && /menu_qcvm.extfuncs.m_draw/ { qcvm = 1 }
  infunc && /m_state != m_none/ { normal = 1 }
  infunc && /key_dest == key_menu/ { menu = 1 }
  infunc && /^}/ { infunc = 0 }
  END {
      if (!qcvm || !normal || !menu) {
          printf "FAIL: M_WillCoverConsole does not mirror both branches of M_Draw\n"
          exit 1
      }
  }
' Quake/menu.c || exit 1
echo "PASS: the console text is not drawn when the menu is about to paint the"
echo "PASS: same background over it -- 382 invisible glyph draws per frame."


# The log has to survive the crash, not just describe it afterwards.
#
# Both hardware logs end on an image load from the single-player menu and then
# simply stop.  That is not proof the crash is there: psp_tracef() batches its
# lines and the watchdog drains them once a second (README 3w), so a fast death
# takes up to a second of lines with it and the log ends wherever the last
# flush happened to be.  The one thing the menu needs is a line on the card
# before the next instruction runs.
grep -q 'void psp_con_trace_sync (int on)' port/psp/psp_console.c || exit 1
grep -q 'void psp_con_trace_sync (int on)' port/psp/psp_console.h || exit 1
grep -q 'psp_con_trace_sync(on_saves)' port/psp/main_psp.c || exit 1
grep -q 'int on_saves = (key_dest == key_menu && M_IsSaveMenu());' port/psp/main_psp.c || exit 1
# ...and ONLY the save/load screen may pay for it.  "every menu" was tried and
# cost twenty memory-stick round trips per frame: the menu went from 20 ms a
# frame to 350 ms, which is a broken game, not a diagnostic.
grep -q 'qboolean M_IsSaveMenu (void)' Quake/menu.c || exit 1
grep -q 'M_IsSaveMenu' Quake/menu.h || exit 1
# It has to be evaluated per frame, inside the loop, not once at startup.
awk '
  /while \(1\)/ { inloop = 1 }
  inloop && /psp_con_trace_sync/ { seen = 1 }
  inloop && /Host_Frame/ { if (!seen) { printf "FAIL: psp_con_trace_sync is not set before Host_Frame\n"; exit 1 } after = 1 }
  END { if (!after) { printf "FAIL: the frame loop does not set psp_con_trace_sync\n"; exit 1 } }
' port/psp/main_psp.c || exit 1
# And a traced line must actually reach the card: the flag has to be read in
# both the tracing and the mark paths.
awk '
  /^void psp_tracef/ { inf = 1 }
  inf && /con_trace_sync/ { t = 1 }
  inf && /^}/ { inf = 0 }
  /^void psp_mark/ { inm = 1 }
  inm && /con_trace_sync/ { m = 1 }
  inm && /^}/ { inm = 0 }
  END {
      if (!t) { printf "FAIL: psp_tracef never consults con_trace_sync\n"; exit 1 }
      if (!m) { printf "FAIL: psp_mark never consults con_trace_sync\n"; exit 1 }
  }
' port/psp/psp_console.c || exit 1

# ...but a round trip per LINE is 5 ms, and the save/load screen emits sixty
# lines a frame (the motif redraws, and the drawing code marks a picture per
# draw).  The hardware log of round 61 shows what that costs: frames 5163-5190
# in the save/load menu took 98, 285, 292, 290 and 287 ms with the GE idle at
# 0.2 ms -- three frames a second, inside the menu the trail was meant to
# explain.  So a traced line goes to the card through the throttled sync, and
# the throttle is a real bound, not a comment.
grep -q 'con_log_sync_throttled' port/psp/psp_console.c || exit 1
grep -q 'define CON_TRACE_SYNC_MIN_US' port/psp/psp_console.c || exit 1
awk '
  /^void psp_tracef/ { inf = 1 }
  inf && /con_log_sync \(\);/ { direct = 1 }
  inf && /^}/ { inf = 0 }
  END { if (direct) { printf "FAIL: a traced line goes straight to con_log_sync() again\n"; exit 1 } }
' port/psp/psp_console.c || exit 1
awk '
  /^static void con_log_sync_throttled/ { inf = 1 }
  inf && /CON_TRACE_SYNC_MIN_US/ { bound = 1 }
  inf && /con_log_sync_at_most/ { call = 1 }
  inf && /^}/ { inf = 0 }
  END {
      if (!bound) { printf "FAIL: the throttled sync has no time bound\n"; exit 1 }
      if (!call) { printf "FAIL: the throttled sync never asks for a timed write\n"; exit 1 }
  }
' port/psp/psp_console.c || exit 1
# ...and the timed write itself is the one that writes, or the bound means
# nothing and nobody ever reaches the card.
awk '
  /^static void con_log_sync_at_most/ { inf = 1 }
  inf && /since < min_us/ { cmp = 1 }
  inf && /con_log_sync\(\);/ { call = 1 }
  inf && /^}/ { inf = 0 }
  END {
      if (!cmp) { printf "FAIL: con_log_sync_at_most does not compare against its window\n"; exit 1 }
      if (!call) { printf "FAIL: con_log_sync_at_most never writes to the card\n"; exit 1 }
  }
' port/psp/psp_console.c || exit 1
# Boot marks are timed too, and the window is coarse enough to matter.
# Per-mark round trips were measured on the hardware at 18 ms each: the first
# heartbeat of the round-63 log reads "card 1345 writes 24749 ms", i.e. 24.7
# seconds of start-up spent writing a log nobody reads unless the module dies
# in one of its lines.
awk '
  /^void psp_mark/ { inm = 1 }
  inm && /if \(con_marks_live\)/ { boot = 1 }
  inm && boot && /con_log_sync_at_most *\(CON_BOOT_SYNC_MIN_US\)/ { bounded = 1 }
  inm && boot && /con_log_sync *\(\);/ { direct = 1 }
  inm && /^}/ { inm = 0 }
  END {
      if (direct) { printf "FAIL: a boot mark still pays a card round trip per line\n"; exit 1 }
      if (!bounded) { printf "FAIL: boot marks do not reach the card at all\n"; exit 1 }
  }
' port/psp/psp_console.c || exit 1
boot_us=$(sed -n 's/^#define CON_BOOT_SYNC_MIN_US \([0-9]*\).*/\1/p' port/psp/psp_console.c)
if [ -z "$boot_us" ] || [ "$boot_us" -lt 250000 ]; then
    echo "FAIL: the boot window is ${boot_us:-missing} us -- 18 ms a write means"
    echo "FAIL: anything finer still spends seconds of the boot on the card"
    exit 1
fi
# What start-up printed is complete when the frame loop starts: boot_done()
# flushes, because after it the marks stop syncing on their own.
awk '
  /^void psp_con_boot_done/ { inf = 1 }
  inf && /con_log_sync *\(\);/ { call = 1 }
  inf && /^}/ { inf = 0 }
  END { if (!call) { printf "FAIL: boot_done leaves the tail of the boot trail in RAM\n"; exit 1 } }
' port/psp/psp_console.c || exit 1
# The window has to be coarser than a FRAME, not finer.  Fifteen milliseconds
# was tried on the hardware and changed nothing (292 -> 257 ms is noise),
# because sixteen lines a frame write sixteen times a frame whatever the window
# says.  Ten writes a second is what actually removes the cost.
min_us=$(sed -n 's/^#define CON_TRACE_SYNC_MIN_US \([0-9]*\).*/\1/p' port/psp/psp_console.c)
if [ -z "$min_us" ] || [ "$min_us" -lt 50000 ]; then
    echo "FAIL: the trace window is ${min_us:-missing} us -- finer than a frame,"
    echo "FAIL: which is the setting that was measured as making no difference"
    exit 1
fi
# And the cost must be counted, not assumed: the card write is timed, and the
# heartbeat prints the total.  That is what turns the next log into an answer
# instead of another opinion.
grep -q 'void psp_con_card_take_stats (unsigned int \*writes, unsigned int \*us)' port/psp/psp_console.c
grep -q 'void psp_con_card_take_stats (unsigned int \*writes, unsigned int \*us);' port/psp/psp_console.h
grep -q 'psp_con_card_take_stats (&card_writes, &card_us);' port/psp/main_psp.c
grep -q 'card %u writes %u ms' port/psp/main_psp.c
awk '
  /^static int con_log_try/ { inf = 1 }
  inf && /sceKernelGetSystemTimeWide/ { timed = 1 }
  inf && /con_card_us \+=/ { acc = 1 }
  inf && /^}/ { inf = 0 }
  END {
      if (!timed) { printf "FAIL: the card write is not timed\n"; exit 1 }
      if (!acc) { printf "FAIL: the card write time is not accumulated\n"; exit 1 }
  }
' port/psp/psp_console.c || exit 1
echo "PASS: on the save/load screen the trail goes to the card in order, at most"
echo "PASS: ten times a second, and the heartbeat reports what those writes cost."
echo "PASS: start-up marks are timed the same way (a quarter of a second, not a"
echo "PASS: round trip per line), and boot_done hands the tail of the boot trail"
echo "PASS: to the card before the frame loop starts."

# The load/save screen asked for a picture whose name is no name at all.
#
# The hardware log of that screen ends here, and the screen is exactly where the
# console has been dying since round 28:
#
#     mark: tex
#     img: asked for "", which names no file -- not looking
#     mark: pic 0x0
#
# M_DrawSaveSlots() was called with an empty title, the empty title went down
# Draw_CachePic() to TexMgr_LoadImage(), and the texture manager answered with a
# texture of size zero -- a cached 0x0 picture, drawn as a quad with 0/0 texture
# coordinates.
#
# Two rules now: Draw_CachePic() refuses a name that names nothing, and the
# save/load screen draws no title picture when it has no title.
awk '
  /^qpic_t\t\*Draw_TryCachePic / { inf = 1 }
  inf && /if \(!path \|\| !path\[0\]\)/ { guard = 1 }
  inf && /Draw_MissingPic *\(/ { missing = 1 }
  inf && /DRAW_MARK\(path\)/ { if (!guard) { printf "FAIL: Draw_TryCachePic marks the path before refusing an empty one\n"; exit 1 } }
  inf && /^}/ { inf = 0 }
  END {
      if (!guard) { printf "FAIL: Draw_TryCachePic accepts a picture with an empty name again\n"; exit 1 }
      if (!missing) { printf "FAIL: an empty name must return the 1x1 stand-in, not NULL\n"; exit 1 }
  }
' Quake/gl_draw.c || exit 1
awk '
  /^static void M_DrawSaveSlots / { inf = 1 }
  inf && /title_pic\[0\]/ { guarded = 1 }
  inf && /Draw_CachePic\(title_pic\)/ { if (!guarded) { printf "FAIL: M_DrawSaveSlots asks for a title picture without checking the name\n"; exit 1 } }
  inf && /^}/ { inf = 0 }
  END { if (!guarded) { printf "FAIL: M_DrawSaveSlots does not skip an empty title\n"; exit 1 } }
' Quake/menu.c || exit 1
echo "PASS: a picture whose name is empty is not loaded, not cached and not"
echo "PASS: drawn -- the save/load screen asks for no title picture at all."

# ---- the 3D pipeline starts with the near-plane clipper ------------------
#
# Stage M5 is the world, and the world cannot be drawn without clipping: a
# polygon that crosses the camera has a vertex with w <= 0, and a perspective
# divide by a number that is not positive puts the polygon on the wrong side of
# the screen.  That is not a theory -- it is the shape of the wedges this port
# has been chasing.
#
# The clipper lives in its own file with no GL, no GE and no PSP in it, so it
# is checked here, on the build machine, with numbers: clipping is the one part
# of the 3D pipeline that can be wrong in a way the panel shows only as "the
# world is inside out".
if [ ! -f port/psp/r_clip.c ] || [ ! -f port/psp/r_clip.h ]; then
    echo "FAIL: the near-plane clipper is gone (port/psp/r_clip.c)"
    exit 1
fi
if ! cc -std=gnu11 -Wall -Wextra -o "$work/test_clip" tests/test_clip.c \
        port/psp/r_clip.c 2> "$work/test_clip.log"; then
    echo "FAIL: the clipper does not build on its own:"
    sed -n 1,20p "$work/test_clip.log"
    exit 1
fi
if ! "$work/test_clip" > "$work/test_clip.out" 2>&1; then
    cat "$work/test_clip.out"
    exit 1
fi
# ...and it is actually in the frame path, and only where it belongs.  A
# primitive is routed to the clipper in glBegin() from the projection in force,
# and glEnd() has to hand it over; the 2D canvases keep the old path.
awk '
  /^void glBegin/ { inb = 1 }
  inb && /shim_clipping = shim_perspective *\(\);/ { set = 1 }
  inb && /^}/ { inb = 0 }
  /^void glEnd/ { ine = 1 }
  ine && /if \(shim_clipping\)/ { hook = 1 }
  ine && /shim_flush_3d *\(\);/ { call = 1 }
  ine && /^}/ { ine = 0 }
  END {
      if (!set)  { printf "FAIL: glBegin does not choose the path from the projection\n"; exit 1 }
      if (!hook) { printf "FAIL: glEnd never checks whether the primitive is 3D\n"; exit 1 }
      if (!call) { printf "FAIL: a 3D primitive is never clipped\n"; exit 1 }
  }
' port/psp/gl_shim.c || exit 1
# The 2D path must not have moved: an orthogonal projection still maps the
# vertex straight onto the panel inside shim_emit_vertex().
awk '
  /^static void shim_emit_vertex/ { inf = 1 }
  inf && /if \(shim_clipping\)/ { branch = 1 }
  inf && /sx = \(float\)shim_vp_x/ { map = 1 }
  inf && /^}/ { inf = 0 }
  END {
      if (!branch) { printf "FAIL: shim_emit_vertex lost its 3D branch\n"; exit 1 }
      if (!map)    { printf "FAIL: shim_emit_vertex no longer maps a 2D vertex\n"; exit 1 }
  }
' port/psp/gl_shim.c || exit 1
# And the module links it: a source that is not in PSP_BACKEND is a source that
# the linker never sees.
grep -q 'r_clip.o' port/psp/Makefile || {
    echo "FAIL: r_clip.o is not in the PSP build"
    exit 1
}
echo "PASS: the near-plane clipper is in the build, checked by numbers on the"
echo "PASS: build machine, and only a perspective primitive goes through it."

# ---- a picture too big to decode is refused, not attempted ----------------
#
# Starting a map ended a hardware session with the console switching itself off,
# and the line before it was
#
#     malloc: NULL for 17400000 bytes -- last pic: cached
#
# A 17.4 MB request while eight were free.  Round 66 found out that the request
# was not the decoder's after all (see the progs.dat section at the end of this
# file), but the limits below are still right on their own: an image decoder
# asking for the impossible is a missing texture, and the alternative was a
# lost session.  Refusing the file by name costs nothing that was not already
# lost.
grep -q 'PSP_IMAGE_FILE_LIMIT' Quake/image.c || {
    echo "FAIL: the picture loader has no file-size limit again"
    exit 1
}
# The limit has to be checked before EVERY decoder, or the one format that was
# forgotten is the one that asks for the 17 MB.
decoders=$(grep -c 'IMG_REFUSE_IF_HUGE (loadfilename)' Quake/image.c)
if [ "${decoders:-0}" -lt 7 ]; then
    echo "FAIL: only ${decoders:-0} of the 7 picture decoders check the file size"
    exit 1
fi
awk '
  /^static qboolean Image_RefuseHugeFile/ { inf = 1 }
  inf && /PSP_IMAGE_FILE_LIMIT/ { limit = 1 }
  inf && /^}/ { inf = 0 }
  /^#define IMG_REFUSE_IF_HUGE/ { macro = 2 }
  macro > 0 { if (/fclose *\(f\)/) close_ = 1; macro-- }
  END {
      if (!limit)  { printf "FAIL: the size check has no limit\n"; exit 1 }
      if (!close_) { printf "FAIL: a refused file is left open\n"; exit 1 }
  }
' Quake/image.c || exit 1
echo "PASS: a picture file larger than this port can decode is refused by name"
echo "PASS: and skipped before any decoder allocates its buffer for it."

# ---- a failed allocation names its caller --------------------------------
#
# "last pic: cached" says what the consequence was.  The six breadcrumbs
# before it say which file was being loaded when the memory ran out, and they
# have to reach the card before the next instruction can fault.
awk '
  /^static void con_steps_tail/ { inf = 1 }
  inf && /con_steps\[/ { ring = 1 }
  inf && /^}/ { inf = 0 }
  /^void psp_con_note_alloc_failure/ { ina = 1 }
  ina && /con_steps_tail *\(/ { called = 1 }
  ina && /hist: / { named = 1 }
  ina && /con_log_sync *\(\);/ { synced = 1 }
  ina && /^}/ { ina = 0 }
  END {
      if (!ring)   { printf "FAIL: the breadcrumb tail does not read the ring\n"; exit 1 }
      if (!called) { printf "FAIL: a failed allocation no longer names its caller\n"; exit 1 }
      if (!named)  { printf "FAIL: the caller line is gone\n"; exit 1 }
      if (!synced) { printf "FAIL: the failure line is not pushed to the card\n"; exit 1 }
  }
' port/psp/psp_console.c || exit 1
echo "PASS: a failed allocation prints the trail that led to it -- which file was"
echo "PASS: loading -- and writes it to the card before anything else runs."

# ---- a picture that DECODES to more than the console holds ---------------
#
# Round 64's log proved the file-size limit was not the whole story: the crash
# still happened, and this time the breadcrumb trail showed it inside a picture
# load (pic 256x128 -> pic: copy -> pic: cached).  A small PNG can declare
# 2048x2048, and lodepng allocates w*h*4 = 16 MB for it on a console whose whole
# newlib heap block is 10 MB.  The header is in hand before any decoder
# allocates, so the size is asked for first.
grep -q 'PSP_IMAGE_PIXEL_LIMIT' Quake/image.c || {
    echo "FAIL: the picture loaders have no limit on decoded size"
    exit 1
}
pixelguards=$(grep -c 'IMG_REFUSE_IF_HUGE_PIXELS (' Quake/image.c)
if [ "${pixelguards:-0}" -lt 6 ]; then
    echo "FAIL: only ${pixelguards:-0} of 6 decoders check the decoded size"
    exit 1
fi
# Each decoder that can be handed a lying header has to be one of them: PNG
# (lodepng allocates internally), JPEG/BMP via stb, TGA, PCX, BMP and DDS.
for fn in Image_LoadPNG Image_LoadSTBI Image_LoadTGA Image_LoadPCX Image_LoadBMP Image_LoadDDS; do
    awk -v fn="$fn" '
      $0 ~ (fn " *\\(FILE") { inf = 1 }
      inf && /IMG_REFUSE_IF_HUGE_PIXELS/ { found = 1 }
      inf && /^}/ { if (!found) { printf "FAIL: %s does not check the decoded size\n", fn; exit 1 } inf = 0 }
    ' Quake/image.c || exit 1
done
# The limit is a limit, not a comment, and it is above everything that works
# today (the biggest picture the GE can sample is 512x512).
awk '
  /^#define PSP_IMAGE_PIXEL_LIMIT/ { print }
' Quake/image.c | grep -q '1024u \* 1024u' || {
    echo "FAIL: the decoded-size limit is missing or below the 512x512 a GE texture can be"
    exit 1
}
echo "PASS: every picture decoder asks the header how big the picture claims to be"
echo "PASS: and refuses it by name before allocating -- 512x512 is the largest the"
echo "PASS: GE can sample, so a million pixels is already four times too much."

# ---- the line that explains a failure is not allowed to be lost ----------
#
# The failure log of round 64 has the breadcrumb tail and NOT the line above it
# with the size in it: the card was busy and con_log_direct() did not retry.
awk '
  /^static void con_log_direct/ { inf = 1 }
  inf && /tries < 2/ { retry = 1 }
  inf && /con_log_try *\(buf\)/ { call = 1 }
  inf && /^}/ { inf = 0 }
  END {
      if (!retry) { printf "FAIL: a crash-path line is written once, and can be lost\n"; exit 1 }
      if (!call)  { printf "FAIL: con_log_direct no longer writes at all\n"; exit 1 }
  }
' port/psp/psp_console.c || exit 1
echo "PASS: a line written on the crash path is retried, so a busy card cannot"
echo "PASS: swallow the one line that says what failed."

# ---- the 17.4 MB request was a whole file, not a picture -----------------
#
# The log that started round 66 was
#
#     mark: gfx/a_kazik/lemon.pcx
#     img: gfx/conchars_ru.png ok 256x256 (3499 bytes)
#     malloc: NULL for 17400000 bytes -- last pic: cached, phase host frame:
#     hist: tex32: done <- tex: free hunk <- tex: done <- pic 256x128 <- pic: copy <- pic: cached
#
# and the trail ends with a picture that was *cached*, i.e. finished.  The
# picture decoders were not asking for anything (both limits above are in
# place); the menu was.  HasBots() in Quake/menu.c read the whole of progs.dat
# into one malloc() to look at each byte in order -- 17.4 MB, once, in the menu
# drawn just before a map starts.  It is a scan now (Quake/crc.c), fed a window
# per frame, and it allocates nothing.
#
# The scan has to keep giving the same answers as the two functions it
# replaces, so it is checked against them on random data, split and resumed at
# random points: tests/test_crc_scan.c.
compile -c "$repo/Quake/crc.c" -o "$work/crc.o"
compile "$repo/tests/test_crc_scan.c" "$work/crc.o" \
    ${LDFLAGS:-} -Wl,--gc-sections -o "$work/crc_scan"
if ! "$work/crc_scan" > "$work/crc_scan.out" 2>&1; then
    cat "$work/crc_scan.out"
    exit 1
fi
# The whole-file read must not come back: no malloc in the progs check at all,
# and the file read through the resumable scan instead.
awk '
  /^static qboolean Bots_ScanStep/ { inf = 1 }
  inf && /malloc/ { printf "FAIL: the progs check allocates again\n"; exit 1 }
  inf && /CRC_ScanStep *\(/ { step = 1 }
  inf && /CRC_ScanFinish *\(/ { fin = 1 }
  inf && /fclose *\(bots_file\)/ { closed = 1 }
  inf && /^}/ { inf = 0 }
  /^qboolean HasBots/ { inh = 1 }
  inh && /Bots_ScanStep *\(/ { called = 1 }
  inh && /^}/ { inh = 0 }
  END {
      if (!step)   { printf "FAIL: the progs check no longer scans\n"; exit 1 }
      if (!fin)    { printf "FAIL: the last window is never finished\n"; exit 1 }
      if (!closed) { printf "FAIL: the progs check leaves the file open\n"; exit 1 }
      if (!called) { printf "FAIL: HasBots does not run the scan\n"; exit 1 }
  }
' Quake/menu.c || exit 1
# One window per call, and it is not on the stack: the main thread had 249 KB
# of stack free on the hardware, and a 256 KB local would end the run.
grep -q '#define BOTS_SCAN_WINDOW (256 \* 1024)' Quake/menu.c || {
    echo "FAIL: the progs scan window changed size without a measurement"
    exit 1
}
grep -q 'static byte       bots_window \[BOTS_SCAN_WINDOW\]' Quake/menu.c || {
    echo "FAIL: the progs scan window is on the stack again"
    exit 1
}
# The CRC has to stay CRC_Block()'s: the same initial value, the same table,
# and no CRC_Value() on the way out (CRC_Block() does not apply it either).
grep -q 'CRC_Init (&scan->crc);' Quake/crc.c || {
    echo "FAIL: the streaming scan no longer starts from the CRC initial value"
    exit 1
}
grep -q 'crctable\[(scan->crc >> 8) ^ c\]' Quake/crc.c || {
    echo "FAIL: the streaming scan updates the CRC differently from CRC_Block()"
    exit 1
}
echo "PASS: the progs check reads the file a window per frame and allocates"
echo "PASS: nothing, and it gives CRC_Block()'s CRC and q_memmem()'s whole-word"
echo "PASS: answer -- checked against both on the build machine."

# ---- the 17.4 MB request was the map, read to be checksummed --------------
#
# Round 66 fixed the menu's progs.dat read and the next log still ended with
#
#     malloc: NULL for 17400000 bytes at 0x08949898 -- last pic: cached,
#     phase host frame: input/commands/clien
#
# -- 17.4 MB, i.e. COM_LoadFile()'s len+1 of a 17 399 999-byte file, asked for
# while drawing a menu.  The one place in the engine that reads a file that
# size, in one malloc, with no breadcrumb of its own, is the map CRC of
# sv_mapcrc: SV_SpawnServer() (Quake/sv_main.c) and CL_MapCRC_Validate()
# (Quake/crc.c) both read the *whole* .bsp just to hash it, at the top of every
# level.  A checksum is a chain over 64-byte blocks, so neither needs the file
# in memory: COM_BlockChecksumFile() (Quake/common.c) reads it in 4 KB windows
# and returns the same two numbers (Quake/mdfour.c,
# Com_BlockChecksumBegin/Update/End).
#
# Those numbers are compared between a client and a server to decide that they
# are playing the same map, so "the same" has to be checked, not assumed:
# tests/test_checksum_stream.c compares the streaming chain with
# Com_BlockChecksum() on random data, split at every awkward place.
compile -c "$repo/Quake/mdfour.c" -o "$work/mdfour.o"
compile "$repo/tests/test_checksum_stream.c" "$work/mdfour.o" \
    ${LDFLAGS:-} -Wl,--gc-sections -o "$work/checksum_stream"
if ! "$work/checksum_stream" > "$work/checksum_stream.out" 2>&1; then
    cat "$work/checksum_stream.out"
    exit 1
fi
cat "$work/checksum_stream.out"
# And the whole-file read must not come back into either caller.
if grep -q 'COM_LoadMallocFile *( *sv\.modelname' Quake/sv_main.c; then
    echo "FAIL: the server reads the whole .bsp to checksum it again"
    exit 1
fi
if grep -q 'COM_LoadMallocFile' Quake/crc.c; then
    echo "FAIL: CL_MapCRC_Validate reads the whole .bsp again"
    exit 1
fi
grep -q 'COM_BlockChecksumFile *( *sv\.modelname' Quake/sv_main.c || {
    echo "FAIL: the server no longer checksums the map through the window"
    exit 1
}
grep -q 'COM_BlockChecksumFile *( *bsp' Quake/crc.c || {
    echo "FAIL: the client no longer checksums the map through the window"
    exit 1
}
# The window must be a buffer on the stack, not a malloc of the file.
grep -q 'define COM_CHECKSUM_WINDOW 4096' Quake/common.c || {
    echo "FAIL: the map checksum window changed size without a measurement"
    exit 1
}
grep -q 'byte	buf\[COM_CHECKSUM_WINDOW\];' Quake/common.c || {
    echo "FAIL: the map checksum window is not the 4 KB on-stack buffer"
    exit 1
}
echo "PASS: the map CRC reads the .bsp in 4 KB windows and returns the same"
echo "PASS: numbers as the one-piece Com_BlockChecksum() it replaces, checked"
echo "PASS: on the build machine."

# ---- the 17.4 MB request was the entity array, not a file ------------------
#
# The round-67 log answered the failure with three lines that each try to load
# a level on their own -- and none of them fit:
#
#     malloc: NULL for 17400000 bytes at 0x08949d64 -- last pic: cached
#     progs: progs.dat is 362 KB; malloc can still reach 8068 KB
#
# The arithmetic names it.  Every VM allocates its entities as one block
#
#     qcvm->max_edicts = CLAMP (MIN_EDICTS, (int)max_edicts.value, MAX_EDICTS);
#     qcvm->edicts = (edict_t *) malloc (qcvm->max_edicts * qcvm->edict_size);
#
# with max_edicts defaulting to 15000 (Quake/host.c) and edict_size 1160 bytes
# for this game's progs: 15000 * 1160 = 17 400 000, exactly the size in the log.
# Against a 10 MB heap block that request can never succeed, and the NULL is
# written through by the memset two lines later -- which is why the console
# died at the top of every level with no message.
#
# So the size now comes from the room the platform actually has
# (PR_AllocEdicts, Quake/pr_edict.c): the most edicts that leave a reserve for
# the level, printed, with a false return instead of a NULL to write through.
awk '
  /^qboolean PR_AllocEdicts/ { inf = 1 }
  inf && /psp_heap_block_left *\(/ { room = 1 }
  inf && /reserve *>/ { reserve = 1 }
  inf && /vm->edicts = \(edict_t \*\) malloc/ { alloc = 1 }
  inf && /return vm->edicts != NULL/ { nonnull = 1 }
  inf && /^}/ { inf = 0 }
  END {
      if (!room)    { printf "FAIL: the entity array is not sized to the heap\n"; exit 1 }
      if (!reserve) { printf "FAIL: the entity array takes the whole heap\n"; exit 1 }
      if (!alloc)   { printf "FAIL: the entity array is not allocated in one place\n"; exit 1 }
      if (!nonnull) { printf "FAIL: a failed entity allocation is not reported\n"; exit 1 }
  }
' Quake/pr_edict.c || exit 1
# All three VMs (server, CSQC, menu) go through it, and none of them keeps its
# own unchecked malloc.
for f in Quake/sv_main.c Quake/host.c Quake/menu.c; do
    grep -q 'PR_AllocEdicts' "$f" || {
        echo "FAIL: $f still allocates the entity array by hand"
        exit 1
    }
done
if grep -rn 'malloc (qcvm->max_edicts' Quake/*.c; then
    echo "FAIL: an unchecked entity array malloc is back"
    exit 1
fi
# And the number is on the card, not in the batch: it is written where the last
# crash happened, and the next instruction is a memset through the pointer.
grep -q 'psp_trace_syncf ("edicts:' Quake/pr_edict.c || {
    echo "FAIL: the entity allocation no longer reports its size durably"
    exit 1
}
echo "PASS: the entity array is 15000 x 1160 = 17.4 MB, which is the number in"
echo "PASS: the log; it is now sized to the heap the console has, reported on"
echo "PASS: the card before the memset, and fails as an error instead of a NULL."

# ---- "the hunk is full" now says what holds it ---------------------------
#
# Round 68 got past the entity array (15000 * 1160 = 17.4 MB) and the map
# loaded -- and then stopped on
#
#     Cache_TryAlloc: 3536192 is greater then free hunk
#
# progs/player.mdl, 3.5 MB, into a zone that the world map is already holding.
# The message named neither the size of the hunk, nor what was free, nor what
# the rest of it was: on a 64 MB console with a 17.4 MB map, that is the only
# number that decides anything.  So the two failure paths report the hunk by
# name before they stop, and SV_SpawnServer reports it once the world is loaded
# (so a good log has it too).
grep -q 'void Hunk_Report (const char \*why)' Quake/zone.c || {
    echo "FAIL: the hunk has no report"
    exit 1
}
grep -q 'Hunk_Report (va ("Cache_Alloc' Quake/zone.c || {
    echo "FAIL: a failed cache allocation no longer says what holds the hunk"
    exit 1
}
grep -q 'Hunk_Report (va ("Hunk_Alloc' Quake/zone.c || {
    echo "FAIL: a failed hunk allocation no longer says what holds the hunk"
    exit 1
}
grep -q 'Hunk_Report ("after the map loaded")' Quake/sv_main.c || {
    echo "FAIL: the map's own cost is not reported"
    exit 1
}
# The report walks the hunk on a path that is already failing: a diagnosis that
# crashes is worse than none, so it must not Sys_Error on a block it cannot read.
awk '
  /^void Hunk_Report/ { inf = 1 }
  inf && /Sys_Error/ { printf "FAIL: the hunk report can die itself\n"; exit 1 }
  inf && /^}/ { inf = 0 }
' Quake/zone.c || exit 1
# The picture pool and the arena cap are one number in two files: the block
# taken from the firmware and the ceiling the arena believes it has.
grep -q 'define PSP_PICTURE_POOL	(6u \* 1024u \* 1024u)' port/psp/psp_mem.c || {
    echo "FAIL: the picture pool changed without measuring the map's textures"
    exit 1
}
grep -q 'define SHIM_ARENA_LIMIT       (6u \* 1024u \* 1024u)' port/psp/gl_shim.c || {
    echo "FAIL: the arena cap no longer matches the block it is given"
    exit 1
}
# A fatal error is the end of the log, not the start of a thousand status lines.
grep -q 'static volatile int con_panicking;' port/psp/psp_console.c || {
    echo "FAIL: the status line is back after a fatal error"
    exit 1
}
echo "PASS: the hunk reports its size, its free bytes and the six biggest"
echo "PASS: allocations by name before it says it is full, and a fatal error"
echo "PASS: ends the log instead of filling it with status lines."

# ---- the textures spilled into the engine's heap -------------------------
#
# Round 69's log got the map loaded and the game running, and then showed what
# the texture budget really is on a console with no VRAM:
#
#     tex: arena short for 256 KB: ... retry STILL FAILED (6019 KB used of 6144 KB)
#     tex: ... kept outside the pool, 17 pictures outside it now
#     run: f 408 ..., malloc 10232 KB used, 1 KB left in the heap block, NULL 17
#
# A 512x512 miptex is 512 KB of the pool, so one map fills it; what did not fit
# fell back to memalign() from the same heap the engine keeps its entity array
# and lightmaps in, and filled it to the last kilobyte -- after which seventeen
# allocations *inside the engine* failed.  A missing texture is ugly; a failed
# engine allocation ends the run.  So the borrowing is capped.
grep -q 'define SHIM_FALLBACK_LIMIT    (2u \* 1024u \* 1024u)' port/psp/gl_shim.c || {
    echo "FAIL: pictures may borrow the whole engine heap again"
    exit 1
}
awk '
  /if \(!t->pixels\)/ { pending = 1 }
  pending && /memalign\(16, need\)/ { guarded = 1 }
  pending && /^\t\t}/ { pending = 0 }
  END { if (!guarded) { printf "FAIL: the heap fallback is not capped\n"; exit 1 } }
' port/psp/gl_shim.c || exit 1
grep -q 'shim_fallback_bytes += need' port/psp/gl_shim.c || {
    echo "FAIL: the heap a spilled picture takes is not counted"
    exit 1
}
grep -q 'shim_fallback_bytes -= t->bytes' port/psp/gl_shim.c || {
    echo "FAIL: a released outside-picture never gives its allowance back"
    exit 1
}
# Two megabytes of the heap that nothing on this console uses: IPLog's table of
# player addresses, 65536 entries for a machine with no Wi-Fi.
grep -q 'define DEFAULT_IPLOGSIZE	0x400' Quake/iplog.c || {
    echo "FAIL: IPLog takes its desktop share of the console's heap again"
    exit 1
}
echo "PASS: the texture pool is 6 MB because the map's miptex live in it, what"
echo "PASS: spills over may borrow at most 2 MB of the engine's heap, and"
echo "PASS: IPLog's two megabytes are back for the engine."

# ---- single player could not connect to itself ---------------------------
#
# Starting a map is a listen server plus a client that connects to it:
#
#     map X -> SV_SpawnServer() -> Cmd_ExecuteString ("connect local")
#           -> CL_EstablishConnection ("local") -> NET_Connect ("local")
#
# port/psp/net_psp.c registered NO network driver, because a PSP-E1008 has no
# wireless.  That is true of the Internet and false of the game: NET_Connect
# walked an empty driver list, returned NULL, and CL_EstablishConnection
# answered Host_Error("connect failed") -- after the whole map had been loaded
# and precached:
#
#     mark: model maps/PSP.bsp
#     edicts: 3401 of 1160 bytes = 3852 KB
#     connecting to local:26000
#     Host_Error: connect failed
#
# So the level loaded and the player was thrown back to the menu, every time.
# net_loop.c is the driver that needs no network -- two qsockets in this
# process with a memcpy between them -- and it is already compiled into this
# build; it only has to be registered.
grep -q 'include "net_loop.h"' port/psp/net_psp.c || {
    echo "FAIL: the PSP build does not use the loopback driver"
    exit 1
}
grep -q 'Loop_Connect,' port/psp/net_psp.c || {
    echo "FAIL: the loopback driver is not registered, so no map can start"
    exit 1
}
grep -q 'const int	net_numdrivers = (int)(sizeof(net_drivers) / sizeof(net_drivers\[0\]))' port/psp/net_psp.c || {
    echo "FAIL: net_numdrivers does not count the registered driver"
    exit 1
}
if grep -q 'net_numdrivers = 0' port/psp/net_psp.c; then
    echo "FAIL: the driver list is empty again"
    exit 1
fi
# The loopback must be driver 0: net_defs.h keys the 'not in the Internet
# browser' checks on IS_LOOP_DRIVER(p) == (p == 0).
awk '
  /^net_driver_t net_drivers\[\]/ { inf = 1; n = 0 }
  inf && /^\t\{/ { n++ }
  inf && /^};/ { inf = 0 }
  END { if (n != 1) { printf "FAIL: %d PSP network drivers, expected only the loopback\n", n; exit 1 } }
' port/psp/net_psp.c || exit 1
# And the message that used to be lost is now on the card before the longjmp.
grep -q 'psp_trace_syncf ("Host_Error: %s", string)' Quake/host.c || {
    echo "FAIL: Host_Error is back to a buffered line only"
    exit 1
}
echo "PASS: single player connects to its own listen server over the loopback"
echo "PASS: driver, so starting a map enters the game instead of ending in"
echo "PASS: Host_Error: connect failed -- and that error is on the card now."

# ---- a failed allocation names the line that asked for it ----------------
#
# Round 65 read `last pic: cached` and concluded "a picture decoder".  Round 66
# found the actual caller -- menu.c reading progs.dat -- by reading the code, not
# the log, because the log did not name it.  That is what this closes: the
# breadcrumbs say which file was being worked on, and the return address of the
# malloc says which of the thousand lines in that file asked.
#
# __wrap_malloc() is called by the code that calls malloc(), so
# __builtin_return_address(0) inside it is the caller.  It is read into a local
# before the real malloc runs -- the register it arrives in is not something to
# hand to argument evaluation order on MIPS.
grep -q 'void \*caller = __builtin_return_address(0);' port/psp/psp_heap.c || {
    echo "FAIL: the malloc hook no longer records who called it"
    exit 1
}
grep -q 'psp_con_note_alloc_failure(what, (unsigned int)size,' port/psp/psp_heap.c || {
    echo "FAIL: the caller address is not handed to the failure line"
    exit 1
}
# ...and the line has to print it, or the address is collected and thrown away.
awk '
  /^void psp_con_note_alloc_failure/ { inf = 1 }
  inf && /at 0x%08x/ { printed = 1 }
  inf && /^}/ { inf = 0 }
  END { if (!printed) { printf "FAIL: the failure line drops the caller address\n"; exit 1 } }
' port/psp/psp_console.c || exit 1
grep -q 'unsigned int caller);' port/psp/psp_console.h || {
    echo "FAIL: psp_con_note_alloc_failure lost its caller argument"
    exit 1
}
echo "PASS: an allocation that fails says which instruction asked for it, so the"
echo "PASS: next one has a name and a line (psp-addr2line) and not just a size."

# ---- the 4 MB hunk request was a texture the GE could not draw anyway ----
#
# Round 70's log, right after the listen server connected:
#
#     connecting to ^mlocal:26000
#     Kabluk Engine 0.2.2-lox Server (56774 CRC)
#     Using protocol fte999
#     run: f 203 state 2, malloc 10229 KB used, 7 KB left in the heap block, ...
#     hunk: Hunk_Alloc 4194336 bytes failed: 19456 KB total, 18128 KB used,
#           (low 14673 + cache 3454), 1327 KB free
#     *** FATAL ERROR *** Hunk_Alloc: failed on 4194336 bytes (holding for 30s)
#     *** watchdog: last step: tex: convert
#
# 4194336 = sizeof(hunk_t) 32 + exactly 4 MB, and "tex: convert" is the
# breadcrumb TexMgr_LoadImage() sets before it dispatches.  The 4 MB is
# TexMgr_8to32(): a 1024x1024 miptex expanded to four bytes a texel, in the
# hunk, while the hunk also holds the zone (4096 KB), the console buffer
# (1024 KB), progs/player.mdl (1024 KB) and cl_entities (5507 KB).
#
# The texture could not have been drawn either: pspsdk src/gu/sceGuTexImage.c
# keeps width and height as 4-bit logs, so gl_shim.c refuses anything past
# SHIM_TEX_MAX 512.  The picture was converted at full size and thrown away.
grep -q 'define TEX_PSP_MAX 512' Quake/gl_texmgr.c || {
    echo "FAIL: nothing caps a texture at what the GE can sample"
    exit 1
}
grep -q 'static byte \*TexMgr_Downscale8 (byte \*data' Quake/gl_texmgr.c || {
    echo "FAIL: the indexed downscale is gone, so big miptexes cost 4 bytes a texel"
    exit 1
}
# The two limits are one limit: if the shim refuses 512 and the engine scales to
# 1024, every such texture is converted and then dropped.
shimmax=$(awk '/#define SHIM_TEX_MAX/ { print $3 }' port/psp/gl_shim.c)
texmax=$(awk '/#define TEX_PSP_MAX/ { print $3 }' Quake/gl_texmgr.c)
if [ "$shimmax" != "$texmax" ]; then
    echo "FAIL: engine scales to $texmax, the shim samples at most $shimmax"
    exit 1
fi
# ...and it has to run on the *indexed* texels, before TexMgr_8to32().
down=$(grep -n 'data = TexMgr_Downscale8 (data, &glt->width' Quake/gl_texmgr.c | cut -d: -f1 | head -1)
t32=$(grep -n 'data = (byte\*)TexMgr_8to32(data' Quake/gl_texmgr.c | cut -d: -f1 | head -1)
if [ -z "$down" ] || [ -z "$t32" ] || [ "$down" -ge "$t32" ]; then
    echo "FAIL: the downscale is not before the 32-bit expansion (lines $down, $t32)"
    exit 1
fi
# Nearest neighbour, not an average: these are palette indices.
awk '
  /static byte \*TexMgr_Downscale8/ { inf = 1 }
  inf && /dst\[x\] = src\[x \* f\]/ { ok = 1 }
  inf && /^}/ { inf = 0 }
  END { if (!ok) { printf "FAIL: the downscale averages palette indices\n"; exit 1 } }
' Quake/gl_texmgr.c || exit 1
echo "PASS: a miptex bigger than the GE can sample is halved in 8-bit space, so"
echo "PASS: the hunk never holds a 4 MB RGBA copy of a picture it cannot draw."

# ---- and a failed hunk allocation names the line that asked -------------
#
# The round-70 line gave the size and nothing else, so finding TexMgr_8to32()
# took reading every Hunk_Alloc() in the tree.  Hunk_AllocName() knows the name;
# it goes into the report now.
grep -q 'Hunk_Alloc %i bytes failed (%s)' Quake/zone.c || {
    echo "FAIL: a failed Hunk_Alloc no longer names what asked for the bytes"
    exit 1
}
grep -q 'name ? name : "?"' Quake/zone.c || {
    echo "FAIL: an unnamed allocation must still say so instead of printing NULL"
    exit 1
}
echo "PASS: a hunk that runs out names the allocation that did not fit."

# ---- cl_entities was the biggest block in that hunk ----------------------
#
# sizeof(entity_t) is 376 bytes and max_edicts defaults to 15000, so the client
# entity array was 5507 KB of a 19456 KB hunk -- while the server's own entity
# array was 3852 KB at the 3401 edicts this heap can hold (PR_AllocEdicts).
# An edict number on the wire is an index into the *server's* array, so on the
# PSP, where the only connection is the loopback one, the local server's limit
# is the whole bound: 1249 KB instead of 5507 KB.
grep -q 'sv.qcvm.max_edicts < cl.max_edicts' Quake/cl_main.c || {
    echo "FAIL: the client entity array ignores the server's own limit again"
    exit 1
}
grep -q 'cl.max_edicts = CLAMP (MIN_EDICTS, sv.qcvm.max_edicts, MAX_EDICTS);' Quake/cl_main.c || {
    echo "FAIL: the client entity array is not sized from the server's edicts"
    exit 1
}
echo "PASS: cl_entities is as big as the server can talk, not as big as max_edicts."

# ---- the world is drawn by the GE, not by the CPU ------------------------
#
# Round 71.  Until now every vertex the port drew was transformed on the CPU
# and handed to the GE as a screen coordinate (GU_TRANSFORM_2D).  The world is
# drawn with the GE's own transform stage: the vertices keep the world
# coordinates the engine gave them and sceGuSetMatrix() loads the projection
# and the view.
grep -q 'GU_COLOR_8888 | GU_VERTEX_32BITF)' port/psp/gl_shim.c || {
    echo "FAIL: there is no 3D vertex format (one without GU_TRANSFORM_2D)"
    exit 1
}
grep -q 'sceGuSetMatrix(GU_PROJECTION' port/psp/gl_shim.c || {
    echo "FAIL: the projection is never given to the GE"
    exit 1
}
grep -q 'sceGuSetMatrix(GU_VIEW' port/psp/gl_shim.c || {
    echo "FAIL: the view is never given to the GE"
    exit 1
}
grep -q 'sceGuSetMatrix(GU_MODEL' port/psp/gl_shim.c || {
    echo "FAIL: the model-view the engine loaded never reaches the GE"
    exit 1
}
# The world vertex must not be divided on the CPU any more: shim_emit_world()
# copies the position through.
awk '
  /static void shim_emit_world/ { inf = 1 }
  inf && /1\.0f \/ cv->w/ { bad = 1 }
  inf && /^}/ { inf = 0 }
  END { if (bad) { printf "FAIL: the world vertex is still divided by w on the CPU\n"; exit 1 } }
' port/psp/gl_shim.c || exit 1
echo "PASS: world vertices go to the GE in world space, with the engine's own"
echo "PASS: projection and view matrices loaded into the transform stage."

# ---- and the depth buffer is cleared to the far plane, GL style ----------
#
# Before this the GE range was (65535, 0) and the buffer was cleared to 0.  That
# only ever worked because nothing was depth-tested in 3D; with the GL
# convention (0, 65535) and GL_LEQUAL a cleared buffer must hold the far value
# or the first world fragment is rejected against it.
grep -q 'sceGuDepthRange(0, 65535);' port/psp/gu_psp.c || {
    echo "FAIL: the GE depth range is not the GL one (0 near, 65535 far)"
    exit 1
}
grep -q 'sceGuClearDepth(65535);' port/psp/gu_psp.c || {
    echo "FAIL: the depth buffer is not cleared to the far plane"
    exit 1
}
grep -q 'GL_GEQUAL:  shim_guDepthFunc(GU_GEQUAL)' port/psp/gl_shim.c || {
    echo "FAIL: gl_sky's GL_GEQUAL has no mapping"
    exit 1
}
echo "PASS: the depth test has the convention the projection was written for."

# ---- the scene cache cannot work here, and fails silently ----------------
#
# RSceneCache_Queue() builds its batches on a worker thread and draws them with
# glDrawElements() out of a VBO.  port/psp/sdl_stub.c never runs a thread (it
# would spin on a condition variable and freeze Host_Init) and gl_stub.c's
# glDrawElements()/glVertexPointer() are empty, so with the cache on the world
# would never be drawn at all -- with no error anywhere.
awk '
  /static qboolean RSceneCache_Queue/ { inf = 1 }
  inf && /PLATFORM_PSP/ { psp = 1 }
  inf && /return false/ { ret = 1 }
  inf && /^}/ { inf = 0 }
  END { if (!(psp && ret)) { printf "FAIL: the PSP still queues the scene cache\n"; exit 1 } }
' Quake/r_world.c || exit 1
echo "PASS: the PSP takes the classic immediate-mode world path, which the shim draws."

# ---- the heap and the hunk are sized the right way round -----------------
#
# The round-71 log had the hunk 11 MB free and malloc() returning NULL at the
# same moment, so the split was wrong, not the total.
grep -q 'PSP_HEAP_SIZE_KB(14 \* 1024);' port/psp/psp_boot.c || {
    echo "FAIL: the newlib heap is back to a size the map cannot load in"
    exit 1
}
echo "PASS: the heap has room for the 5.75 MB load, the hunk keeps 7 MB spare."

# ---- and the signon names its own stage ---------------------------------
grep -q 'signon: downloads stage %d of 5' Quake/cl_main.c || {
    echo "FAIL: a stalled signon still prints nothing about which stage it is on"
    exit 1
}
grep -q 'signon: reply %d of %d' Quake/cl_main.c || {
    echo "FAIL: the signon replies are not traced"
    exit 1
}
echo "PASS: a log that ends during the connect says which stage never finished."
