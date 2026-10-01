# Server impulse regression tests

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

These are asset-free regression tests, not an in-game smoke/rendering test.
