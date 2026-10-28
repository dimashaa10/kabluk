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
# Sizing from sceKernelMaxFreeMemSize() in main() is the mistake that produced
#
#     mem: biggest free block 512 KB   ->   heap ceiling: 0 MB
#     Zone: 8 MB                       ->   Hunk_Alloc: failed on 1048608 bytes
#
# because pspsdk's _sbrk() takes the whole partition block at the first malloc(),
# before main().  The room has to be measured with the reversible sbrk() probe.
if grep -q 'ceiling = mem_max_free' port/psp/main_psp.c; then
    echo "FAIL: the zone is sized from sceKernelMaxFreeMemSize() again -- that"
    echo "FAIL: number is the partition leftover, not the heap's room"
    exit 1
fi
grep -q 'unsigned int psp_heap_room (void)' port/psp/psp_heap.c
grep -q 'unsigned int room = psp_heap_room();' port/psp/main_psp.c
grep -q 'room - pool - PSP_ENGINE_RESERVE' port/psp/main_psp.c
grep -q 'psp_heap_expect_failure(1);' port/psp/main_psp.c
grep -q 'void psp_heap_expect_failure (int on)' port/psp/psp_heap.c
# The pictures get their own block, and a pooled arena must never hand chunks
# back: free() of a big block is a one-way loss on this platform (build 17:53).
grep -q 'void psp_gl_shim_set_pool (void \*base, size_t bytes)' port/psp/gl_shim.c
grep -q 'void psp_gl_shim_set_pool (void \*base, size_t bytes);' port/psp/gl_shim.h
grep -q 'psp_gl_shim_set_pool(block, pool);' port/psp/main_psp.c
grep -q 'if (shim_arena_pooled)' port/psp/gl_shim.c
echo "PASS: the room malloc can still reach is measured with sbrk(), the"
echo "PASS: pictures get a block of their own, and nothing big is freed."
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
