# Cigarette input, animation and smoke regression tests

On Linux, from the repository root:

```sh
./tests/run_gameplay_tests.sh
```

Requires a C compiler (GCC or Clang). The runner uses the bundled SDL headers
with temporary compatibility headers; no SDL libraries or game assets are
required, and build artifacts are removed on exit. `CC`, `CFLAGS` and `LDFLAGS`
can be overridden, for example to enable AddressSanitizer/UndefinedBehaviorSanitizer.

The test links the **real `Quake/sv_user.c`** input parser and cigarette think
hooks with network, filesystem/model and QuakeC test doubles. The QC double
intentionally retains the original `impulse 9` cheat, so tests verify that:

- Incoming `9` never reaches `self.impulse` / `input_impulse` as a cheat,
  even without `progs/v_siga.mdl` or while dead.
- Incoming `101` reaches the original QC cheat without bytecode patches or
  named-function lookups; deathmatch restrictions remain in QC.
- Cigarette selection suppresses attack before QC, preserves other buttons,
  equips the viewmodel and drives its animation; weapon impulses 1–8 still work.
- Standard player-think ordering, engine pmove, `SV_RunClientCommand`, extended
  button packets, queued commands and duplicate prediction packets are covered.
- Held attack advances to frame 10 and stays there, without looping; release
  finishes the remaining frames once and returns to idle. An early release also
  finishes the animation, and a new press cannot rewind the finish phase.
- Held input survives ordinary physics ticks without a new movement packet.
- Fixed idle/start/hold frames (0/1/10), end at the last real model frame and
  0.1s timing still work for short/single-frame models; switching and death
  cannot leave stale phases or out-of-range frames.
- Cosmetic `currentammo` is always 1 while equipped, including after repeated
  smoking cycles and QC ammo refreshes. Real ammo pools are unchanged, empty
  pools remain empty, and ordinary weapons show their actual ammo on switching.

`tests/test_cigarette_smoke.c` includes the **real `Quake/view.c`** private smoke
emitter with camera/particle-backend doubles. It checks that held attack emits
nothing (including at frame 10), released non-idle frames emit smoke, idle stops
emission, the fixed 0.04s interval is respected, and visibility/health gates
still work. Bursts contain 8 particles (16 in the classic fallback), with a
rate near 25 bursts/second at high frame rates. No production test-only API
is needed.

These tests exercise frame/state transitions and particle emission decisions,
not the visual appearance of the model or smoke in-game. Game assets are not
included. See `Quake/cigarette.h` for the hardcoded settings and the root README
for behavior and deployment instructions.


## Skate regression coverage

`test_skate_server.c` links the real `SV_Skate_f`, `SV_ClientThink`, skate movement,
board update/cleanup and stat encoding with entity/string/trace/command doubles.
It covers:
- Toggle and explicit on/off, duplicate enable, model missing/limits, invalid
  arguments, disconnected/unspawned/dead players, bad model origin and a full
  entity pool (without a fatal allocation).
- Automatic progressive acceleration with no keys, 420 speed limit, gradual
  turns and lateral momentum, S braking and frame-time independence.
- Unchanged origin/bounds, real ammo and vertical jump velocity; no air boost.
- Shared model origin, wheels above flat ground and sloped ground beneath the
  corners, non-solid board, ownership, follow/yaw and cleanup after death,
  noclip, deep water, ladder entry and a VM-context change.
- Cleanup does not free an unrelated entity that reused the board's slot.

`test_skate_client.c` includes the real `r_alias.c` transform helper and links
real chase/visual helpers, frustum culling and math code. No OpenGL calls are
executed. Tests cover rider/board alignment (including remote players), render
height without moving prediction origins, no accumulated/double height, frustum
bounds, preserving ordinary chase settings, rider-focused camera, wall tracing,
foreign/malformed stat guards and invalid board-owner metadata.

`run_cigarette_impulse_tests.sh` remains a compatibility wrapper for the full
suite. Some legacy renderer code has an existing unused local; only that warning
is suppressed when compiling the renderer-inclusive client test.

These are asset-free logic/transform tests, not a live map or screenshot test.
Actual `progs.dat`, `player.mdl` and the user's prepared `skate.mdl` are needed to
verify the final visual fit. The prototype has no tricks, banking or separate
wheel collision; normal player world collision/gravity still handles motion.
