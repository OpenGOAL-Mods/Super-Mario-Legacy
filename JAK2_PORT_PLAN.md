# Mario integration → Jak 2 port plan

**Branch:** `Vzero-Jak2` (off `VZero` so all Jak 1 mario work is preserved as base).

This is a living checklist. Tick items as they land; add notes when you
discover Jak 1↔Jak 2 deltas worth remembering.

## Source files (Jak 1 → Jak 2)

| Jak 1 source | Jak 2 destination | Notes |
|---|---|---|
| `goal_src/jak1/engine/mods/mario/mario-settings.gc` | `goal_src/jak2/engine/mods/mario/mario-settings.gc` | Settings struct + JSON load — fields nearly identical. Path under `%APPDATA%/OpenGOAL/jak2/settings/` instead of `jak1/`. |
| `goal_src/jak1/engine/mods/mario/mario.gc` | `goal_src/jak2/engine/mods/mario/mario.gc` | Heaviest port. Jak↔Mario sync, watcher process, longjump trainer, retail-safe debug draws. Uses `*target*` and `(target target ...)` types — Jak 2 has its own target type `target2`. |
| `goal_src/jak1/engine/mods/mario/mario-music.gc` | `goal_src/jak2/engine/mods/mario/mario-music.gc` | Per-level music map — Jak 2 has different level set entirely. |
| `goal_src/jak1/engine/mods/mario/mario-menu-h.gc` | same path under jak2 | Type-only, easy. |
| `goal_src/jak1/engine/mods/mario/mario-menu.gc` | same | Hooks pause-menu & sound-options. Jak 2 has `progress2` not `progress`. |
| `goal_src/jak1/engine/mods/mario/mario-debug-menu.gc` | same | Debug-menu only — Jak 2's debug menu file is `default-menu-pc-jak2.gc` or similar. |

## Cross-cutting Jak 1 files we touched, where Jak 2 equivalents live

| Concern | Jak 1 file | Jak 2 file (existing) |
|---|---|---|
| GOAL→C++ bridge registration | `game/kernel/jak1/kmachine.cpp` | `game/kernel/jak2/kmachine.cpp` |
| Boolean→symbol helper | `bool_to_symbol(bool)` in `game/kernel/common/kmachine.cpp` (works any version) | same |
| Discord RPC | `game/external/discord_jak1.cpp` | `game/external/discord_jak2.cpp` (present) |
| Game text JSON | `game/assets/jak1/text/game_custom_text_*.json` | `game/assets/jak2/text/...` |
| Text-id enum | `goal_src/jak1/engine/ui/text-h.gc` | `goal_src/jak2/engine/ui/text-id-h.gc` |
| Progress menu | `goal_src/jak1/pc/progress-pc.gc` | `goal_src/jak2/pc/progress/progress-pc.gc` |
| Title screen entry | `goal_src/jak1/levels/title/title-obs.gc` | `goal_src/jak2/levels/title/title-obs.gc` |
| DGO build descriptor | `goal_src/jak1/dgos/game.gd` | `goal_src/jak2/dgos/game.gd` |
| Build descriptor (compile order) | `goal_src/jak1/game.gp` | `goal_src/jak2/game.gp` |

## C++ shared file: `game/libsm64/libsm64_integration.{cpp,h}`

This file currently has many Jak 1-only assumptions. The renderer
infrastructure (Mario render bucket, OpenGL state) and libsm64 wrapper
parts are reusable. The "read EE memory for Jak's state" parts need
Jak-version branching.

Specific Jak 1-isms to gate:

- `#include "game/kernel/jak1/kscheme.h"` — needs `#if/#else` block per
  game version.
- `Ptr<TypeJak1>(...)` style accessors — Jak 2 has different layouts for:
  - `target` process type (different state machine, control field offsets)
  - `*game-info*` (Jak 2 has `game-info-jak2`)
  - `*setting-control*`
  - `*pc-settings*` (Jak 2 has `pc-settings-jak2` extending base)
- `read_target_transform` — reads `(-> *target* control trans)`.  Jak 2's
  target type has the same field but offsets may differ; verify in
  decomp `target-h.gc` for Jak 2.
- The yakow grab walks the entity tree looking for `lurkercrab` /
  `yakow` actor types — those don't exist in Jak 2 at all.  Whole
  feature is Jak-1-specific; gate the call out in the Jak 2 build.

## Build configuration

OpenGOAL's gk binary is built per-game-version via CMake presets that
set `-DGAME_VERSION=jak1` (or jak2/jak3).  For the Jak 2 build to
include our libsm64 work, the `game/libsm64/` sources must compile
cleanly under both versions.  The runtime selection of "which game
am I" is via `g_game_version`; Mario init paths gate on
`GameVersion::Jak1` in places — those need to allow Jak 2 too.

Run for Jak 2 build:
```bash
cmake --preset=Release-windows-clang-static-jak2     # if a preset exists
# or
cmake --preset=Release-windows-clang-static -DGAME_VERSION=jak2
cmake --build out/build/Release --target gk_jak2 --parallel 8
```

(Confirm the actual preset / target name from CMakeLists.txt — Jak 2
build target may be `gk` with a flag, not a separate binary.)

## Phase progress

- [x] Branch `Vzero-Jak2` created off VZero (pushed to origin)
- [x] This plan written
- [x] **kmachine.cpp Jak 2 bridge registrations** — all 30 `pc-sm64-*`
      bridges register inside `InitMachine_PCPort` for jak 2.
      `#include "game/libsm64/libsm64_integration.h"` added.
- [x] **libsm64_integration.cpp compiles for Jak 2** — bridge bodies have
      no jak1-specific types in their public C signatures, so the same
      compilation unit serves both versions.  EE-memory readers (e.g.
      `read_target_flags` at line 3303) still call `jak1::intern_from_c`
      which silently no-ops in a jak 2 build — these are documented to
      need degating once the watcher process is ported.
- [x] **Jak 2 mario skeleton GOAL files**
      - `mario-settings.gc` — load + write *mario-settings* singleton.
        On-disk format identical to jak1 so settings transfer between
        builds.
      - `mario-menu-h.gc` — 411-row sound-preview list, byte-identical
        to jak1 (pure SM64 sound IDs).
      - `mario.gc` — bridge externs + music helpers (`sm64-music`,
        `sm64-music-forced`, `sm64-music-id`, `sm64-sound`,
        `sm64-set-music-enabled!`) + state globals
        (`*sm64-mario-pos*` etc).
- [x] **mario sources added to Jak 2 game.gp / game.gd**
      Just `dgos/game.gd` actually — the `cgo-file` macro auto-generates
      goal-src steps from gd entries via goal-src-sequence.  game.gp
      has comment-only block.
- [x] **`(build-game)` succeeds for Jak 2 with mario sources included**
      844 targets all green; mario .o files produced cleanly under
      `out/jak2/obj/`.  No iso assets extracted yet so `(mi)` blocked.
- [x] **jak1 build path still works** — no regressions from jak2 changes
      (`(mi)` for jak1 still produces GAME.CGO clean, 553 targets).

## What still needs porting (depends on runtime testing)

These were intentionally not done overnight because they need
visual / boot verification, which isn't possible without extracted
Jak 2 iso assets.

### Watcher process (sm64-mario-col)
The Jak 1 file has a long `defbehavior sm64-mario-col-init` (~lines
600–1300) plus `sm64-mario-col-start` / `-stop` driver functions.
The Jak 2 port needs:
- `*target*` accessors that match jak2's `target` type (in
  `goal_src/jak2/engine/target/target-h.gc:123`).  `(-> *target*
  control trans)` works the same way (control is overlaid at
  root), so the position read pattern transfers directly.
- State name checks updated: jak2 only has `target-title` (not
  jak1's `target-title-play` / `target-title-wait` split).
- `*game-info*` → `*game-info*` (same global name in jak2, but
  field layout changed — jak2 has `game-info-jak2` deftype with
  different fields than jak1 `game-info`).
- Yakow grab feature is jak1-only; gate out via
  `(if (= *game-version* 'jak1) ...)`.

### mario-music.gc
The level→music map has 25 entries hardcoded for jak1 levels
(village1, beach, jungle, …).  Jak 2 has a totally different set
(see `goal_src/jak2/levels/`): atoll, castle, city, consite, dig,
drill, forest, fortress, gungame, hideout, hiphog, intro, mountain,
nest, outro, palace, ruins, sewer, stadium, strip, test-zone, title,
tomb, under.  Map mood-by-mood:
- city → 'inside-castle (hub feel)
- forest → 'bob-omb
- water levels (atoll, sewer, under) → 'water
- volcano / hot levels → 'hot
- spooky / underground → 'spooky / 'underground
- snow → 'snow (if any snow level exists)
- title → 'title

The progress-screen file-select detection needs a different
approach for jak2 — jak2's `progress` type uses `current` /
`next` symbol fields rather than an enum-driven `display-state`.
Reasonable first cut: skip file-select detection entirely on
jak2 and just play the level track.

### mario-menu.gc
Hooks the pause menu and sound-options.  Jak 2 has `progress`
not `progress2` per the field layout above.  Menu integration
points are different — needs scoping with a fresh pass through
`goal_src/jak2/pc/progress/progress-pc.gc`.

### Yakow grab + crab-test + ROM-required dialog
Jak-1-only features.  Yakow / lurkercrab actor types don't
exist in jak 2; gate the entire path off via game-version check.
The ROM-required dialog needs porting to jak2's progress menu
flow — currently fires from `target-title-play` :code in jak1.

### libsm64_integration.cpp degating
Specific lines that need `g_game_version`-aware branching:
- `jak1::intern_from_c` calls (lines 2411, 2423, 2463, 2481,
  2542, 2556, 2579, 3314) — should resolve through a
  `version_intern_from_c` helper.
- Type-specific layouts (target, game-info, setting-control,
  pc-settings) — most read paths gate on
  `g_game_version == GameVersion::Jak1` already; need parallel
  jak 2 readers.

## What did happen overnight (commits on Vzero-Jak2)

1. `87c5b1785` — jak2 mario port: foundation skeleton (Vzero-Jak2)
   - kmachine.cpp registrations
   - mario-settings.gc + mario.gc skeleton
   - game.gd / game.gp wiring
   - This plan
2. `04ce95b86` — jak2 mario: music helpers + sound preview list
   - sm64-music / sm64-music-forced / sm64-music-id / sm64-sound /
     sm64-set-music-enabled! lifted into mario.gc
   - State globals (*sm64-mario-pos* etc) defined
   - mario-menu-h.gc (411-entry sound preview list)

To resume: `git checkout Vzero-Jak2 && git pull`.  Build is
described in `CLAUDE.md` (root + global).  Quick sanity check:
`task set-game-jak2` then `printf '(build-game)\n(e)\n' | task
repl` should give "Successfully built all 844 targets" — that
confirms the foundation is intact.

## Findings during recon (Jak 1 vs Jak 2 build)

### Jak 2 build descriptor structure differs from Jak 1

Jak 1's `game.gp` has 100+ explicit `(goal-src ...)` calls.  Jak 2's
`game.gp` has ONE (`test-zone-obs.gc`) — everything else is driven by
`cgo-file "game.gd"` reading the DGO descriptor and auto-discovering
`.gc` files in convention paths.

To add mario files to the Jak 2 build, we'll need to either:
1. Add `.o` entries to `goal_src/jak2/dgos/game.gd`
2. AND/OR add `(goal-src ...)` lines to `goal_src/jak2/game.gp` for
   anything that isn't auto-discovered

Look at how `pckernel-impl.o` is wired (`game.gd:35` "added") for the
blueprint.

### Jak 2 reuses Jak 1's pckernel-common.gc

`goal_src/jak2/lib/project-lib.gp:68-70` has a special case in
`goal-src-sequence` that maps `pc/pckernel-h.gc`, `pc/pckernel-
common.gc`, `pc/debug/pc-debug-common.gc` to the Jak 1 source tree
via `make-src-sequence-elt-jak1`.  This means our mario-settings.gc
CAN use `with-settings-scope` / `dosettings` macros — they're
defined in Jak 1's pckernel-common.gc and available in the Jak 2
build.

But: `*pc-temp-string-1*` is Jak-1-only.  `*pc-temp-string*` exists
in both.  Mario-settings will need to use `*pc-temp-string*` (or
allocate a local string) when porting.

### Build flakiness

Running `cmake --build` after the system clock jumped forward triggers
ninja's `manifest 'build.ninja' still dirty after 100 tries` error.
Fix per CLAUDE.md is `rm -rf out/build/Release && cmake --preset=… &&
cmake --build`.  Hit this twice already this session.

## Won't-do overnight (need human verification)

- Visual confirmation that Mario actually spawns/renders in Jak 2
- Position sync correctness (Jak 2 coordinate scale may differ — Jak 1
  uses 4096 = 1m, need to verify Jak 2 same)
- Audio integration (Jak 2 has different sound system)
- Yakow grab feature (Jak 1 only — actor types don't exist in Jak 2;
  whole feature should be gated out via `g_game_version == Jak1`)
- Cinematic gates (Jak 2 has different intro flow)
- Progress menu integration — Jak 2 has its own progress-pc.gc with
  different menu structure; ROM-required dialog needs porting
- `target-title` / `target-title-play` state names — Jak 2 has different
  states; mario-music's title-cinematic gate needs Jak-2 state names
