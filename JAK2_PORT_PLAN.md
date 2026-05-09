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

- [x] Branch `Vzero-Jak2` created off VZero
- [x] This plan written
- [x] **kmachine.cpp Jak 2 bridge registrations** (commit pending) — all 30
      `pc-sm64-*` bridges now register inside `InitMachine_PCPort` for jak 2.
      `#include "game/libsm64/libsm64_integration.h"` added.
- [ ] libsm64_integration.cpp compiles for Jak 2 — **expected to compile
      already** since the bridge function bodies don't directly use
      jak1-specific kscheme.h types in their public C signatures.  Build
      verification in progress.
- [ ] Jak 2 mario skeleton GOAL files
- [ ] mario sources added to Jak 2 game.gp / game.gd
- [ ] `(mi)` succeeds for Jak 2 with mario sources included

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
