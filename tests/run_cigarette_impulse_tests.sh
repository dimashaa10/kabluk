#!/bin/sh
# Backward-compatible entry point; shared player hooks now cover skate too.
set -eu
exec "$(dirname -- "$0")/run_gameplay_tests.sh" "$@"
