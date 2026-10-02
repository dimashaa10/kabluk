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
for source in sv_user sv_phys mathlib chase gl_rmain; do
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
# The pre-existing renderer has an unused model_matrix local unrelated to these tests.
compile -Wno-unused-variable "$repo/tests/test_skate_client.c" "$work/chase.o" "$work/mathlib.o" "$work/gl_rmain.o" \
    ${LDFLAGS:-} -Wl,--gc-sections -lm -o "$work/skate_client"
"$work/skate_client"
