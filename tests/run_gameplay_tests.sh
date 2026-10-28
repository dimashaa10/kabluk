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
