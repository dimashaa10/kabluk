#!/bin/sh
# Native, asset-free regression test. Needs only a C compiler and the headers
# already checked into this repository; does not link SDL or build the engine.
set -eu

repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT HUP INT TERM

# The bundled Windows SDL headers fall back to this file on Linux. JSON header
# aliases accommodate the existing lowercase includes on case-sensitive hosts.
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
# Function sections let the test link just the real input parser/think hooks,
# without the engine's unrelated networking, renderer or movement routines.
"$cc" ${CFLAGS:-} -std=gnu11 -Wall -Wno-missing-field-initializers -DUSE_SDL2 \
    -I"$repo/Quake" -I"$repo/Windows/SDL2/include" -I"$work" \
    -ffunction-sections -fdata-sections \
    -c "$repo/Quake/sv_user.c" -o "$work/sv_user.o"
"$cc" ${CFLAGS:-} -std=gnu11 -Wall -Wno-missing-field-initializers -DUSE_SDL2 \
    -I"$repo/Quake" -I"$repo/Windows/SDL2/include" -I"$work" \
    "$repo/tests/test_cigarette_impulse.c" "$work/sv_user.o" \
    ${LDFLAGS:-} -Wl,--gc-sections -lm -o "$work/test_cigarette_impulse"
"$work/test_cigarette_impulse"
