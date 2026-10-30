# Cigarette and skate gameplay regression tests

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
- One time-based 360-degree sideways roll per upward takeoff, including
  quarter turns, descending motion, completion/wrap to upright, repeated
  updates at the same time, landing/rearm and exiting during a flip.
- No flip on a held jump without takeoff, falling off a ledge, an airborne
  velocity change, enabling in midair or a water jump; early landing and
  mode cleanup clear the animation state.
- A jump is only a takeoff when `+jump` was pressed: rising from a jump pad,
  push trigger or teleport never awards a flip, the input latch is one-shot,
  and riding up a slope keeps the ground poses and never flips or wall-bounces.
- The slide toggle is rate limited; a tap only latches for a grounded rider who
  is already rolling and only reaches a small curb above the board, so tapping
  cannot be used to climb a wall.
- Shared model origin, wheels above flat ground and sloped ground beneath the
  corners, non-solid board, ownership, follow/yaw and cleanup after death,
  noclip, deep water, ladder entry and a VM-context change.
- Cleanup does not free an unrelated entity that reused the board's slot.

`test_skate_client.c` includes the real `r_alias.c` transform helper and links
real chase/visual helpers, frustum culling and math code. No OpenGL calls are
executed. Tests cover rider/board alignment (including remote players), render
height without moving prediction origins, no accumulated/double height, frustum
bounds, preserving ordinary chase settings, rider-focused camera, wall tracing,
foreign/malformed stat guards and invalid board-owner metadata. Jump-flip
tests preserve the received roll and verify the real alias matrix keeps
the board center fixed beneath the upright rider (local and remote),
including off-center models, different yaw/scale, repeated updates,
render culling and restoring the original fit at zero roll.

## Crouch and pmove coverage

The crouch lives in the shared `pmove.c`, which runs both on the server and in
client-side prediction, so `test_pmove_crouch.c` links the real `pmove.c` with a
tiny stand-in world (an endless floor plus an optional ceiling) and checks:
- the crouched hull is half the standing height with the feet left in place, the
  eye height eases over 0.3 s while the collision hull snaps;
- standing up is refused while the ceiling is too low and recovers on its own
  once there is head room;
- `PM_SeedCrouch` rebuilds exactly the same hull from a snapshot, standing or
  crouched;
- crouching caps the *wish* speed on the ground (160) and never brakes momentum
  already in hand, so crouch-jumps keep their speed;
- replaying one command batch from one snapshot is bit-identical for the
  "server" and the "predicted" run, in both postures;
- `PM_CrouchedAim` moves a hard-coded standing-eye shot onto the real crouched
  eye while keeping the prog's aim and range, and leaves correct traces
  (already at the eye, floor probes, standing players) untouched.

`test_crouch_server.c` additionally covers the engine path: the 160 speed clamp
applies on the ground only, and never touches airborne, swimming, noclip or
pmove-driven velocity.

## Collision-entity list and HUD speed

`test_pmove_physents.c` links the real `pmove.c` and fills the collision list the
way a busy map would:
- the list is large enough for a crowd and `physents`, `touchindex` and `touchvel`
  all share its size;
- a solid in the *last* slot blocks exactly like one in the first, its index ends
  up in `touchindex`, and the player stays on the floor;
- with every slot used, `numtouch`, `touchindex[]` and `groundent` stay inside the
  arrays;
- movement itself is unchanged: a full list of far-away dummies and a list with
  only the world produce the same position, so the bigger cap only decides *what*
  may block, not *how* the move runs.

`test_speed_info.c` links the real `sv_user.c` to pin the speedometer feed:
horizontal speed only (vertical is ignored), recomputed every frame including
zero and overspeed, and the bunny-hop key sample is recorded only for a ground
frame with `+jump` and is not overwritten by a later airborne frame.

## Engine action key (+use)

`test_use_button.c` links the real `sv_user.c` and drives the action key against a
tiny world (a wall the player can aim at, plus a floor):

- only `func_button` may be pressed - the whitelist is the `SV_USE_CLASSES`
  constant, not a cvar, so the console cannot add doors to it. `func_door`,
  `func_door_secret`, `func_plat`, `func_train`, `func_wall` and triggers are
  never pressed, which is what keeps the key from opening what a map trigger or a
  key is supposed to open;
- the target must also be a pushing brush solid with a `.use`, and must not be a
  shoot-only button (`health > 0`);
- pressing runs the prog's use function with `self` = button, `other` and
  `activator` = player, and restores the globals afterwards;
- aiming: what is in front is pressed, out-of-range (`SV_USE_RANGE`) and
  behind-the-player brushes are not, and a button flush with the wall counts only
  when the crosshair really is next to it;
- `SV_UseInputEdge` treats holding the key as one press, drops presses inside the
  0.2 s cooldown, and ignores attack/jump/other buttons.

## Controls menu and default bindings

`test_bind_defaults.c` parses the real default config (the `default_cfg` string from
`Quake/default_cfg.h`) and, when present, `Misc/qs_pak/default.cfg`:
- `+use` is part of `engine_required_binds` (`Quake/bindlist.h`), so the controls menu
  offers it even if the game ships its own `bindlist.lst`; every entry there is a
  single-word `+` command with a non-empty label and no duplicates;
- `E` is bound to `+use` exactly once, `+moveup` moved to `PGUP` (so freeing E costs
  nothing) and `+movedown` is still on `C`/`CTRL`;
- no physical key carries two actions in the defaults, every menu action has a default
  key, and the two copies of the default config (binary + pak) agree.

## Skate presentation and stat channel

`test_skate_client.c` also checks the board's screen pose end to end:
- the grind tilt sent by the server (`SKATE_SLIDE_BOARD_PITCH`) reaches the render
  transform instead of being zeroed on the client, for local and remote riders;
- both the tilt and the kickflip roll rotate the board about its own center, verified
  against the real `R_EntityMatrix`, including off-center models, other yaw angles and
  network scales;
- a level board is restored exactly (no accumulated offset) when the tilt goes away.

The stat channel is covered from both sides: `test_skate_server.c` asserts the server
tilt while grinding and its removal afterwards, and `test_skate_client.c` asserts that
an ordinary value in stat 19 from another mod is not mistaken for skate mode.

`run_cigarette_impulse_tests.sh` remains a compatibility wrapper for the full
suite. Some legacy renderer code has an existing unused local; only that warning
is suppressed when compiling the renderer-inclusive client test.

These are asset-free logic/transform tests, not a live map or screenshot test.
Actual `progs.dat`, `player.mdl` and the user's prepared `skate.mdl` are needed to
verify the final visual fit. Apart from the automatic jump kickflip, the
prototype has no other tricks, banking or separate wheel collision; normal
player world collision/gravity still handles motion.
