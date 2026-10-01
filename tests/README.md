# Cigarette input, animation and smoke regression tests

On Linux, from the repository root:

```sh
./tests/run_cigarette_impulse_tests.sh
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
- Configurable start/hold/end/idle frames, short models, live frame-range changes,
  weapon switching and death cannot leave stale phases or out-of-range frames.

`tests/test_cigarette_smoke.c` includes the **real `Quake/view.c`** private smoke
emitter with camera/particle-backend doubles. It checks that held attack emits
nothing (including at frame 10), released non-idle frames emit smoke, idle stops
emission, the interval is respected, and visibility/health/config gates and the
classic particle fallback still work. No production test-only API is needed.

These tests exercise frame/state transitions and particle emission decisions,
not the visual appearance of the model or smoke in-game. Game assets are not
included. See the root README for animation settings and deployment instructions.
