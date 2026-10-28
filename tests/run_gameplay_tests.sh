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
grep -q 'PSP_HEAP_SIZE_KB(10 \* 1024)' port/psp/psp_boot.c
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
grep -q 'psp_con_note_alloc_failure(what, (unsigned int)size);' port/psp/psp_heap.c
grep -q 'void psp_con_note_alloc_failure (const char \*what, unsigned int size)' port/psp/psp_console.c
grep -q 'void psp_con_note_alloc_failure (const char \*what, unsigned int size);' port/psp/psp_console.h
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
if echo "$tracef_body" | grep -q 'con_log_sync'; then
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

