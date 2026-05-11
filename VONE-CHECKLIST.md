# Super Mario Legacy V1 — Rewrite Checklist

## 1. Build System & Third-Party Setup
- [ ] Add `third-party/libsm64/` sources and `CMakeLists.txt`
- [ ] Wire libsm64 into root `CMakeLists.txt`
- [ ] Add `game/libsm64/` directory to `game/CMakeLists.txt` (no quotes, match style)
- [ ] Add Mario GLSL shaders (`mario_sm64.vert`, `mario_sm64.frag`) + register in `Shader.cpp`/`Shader.h`
- [ ] Update `.gitignore` for ROM files / build artifacts
- [ ] Update CI workflows (linux-build-clang, windows-build-clang) for libsm64
- [ ] Update `cut-release.yaml` / `release-pipeline.yaml` for VOne release branch

## 2. Core C++ Integration (`game/libsm64/`)

### 2a. Initialization & ROM Loading
- [ ] `LibSM64Manager` singleton with atomic init tracking
- [ ] ROM auto-detection (exe dir, user config dir, `iso_data/mario`)
- [ ] Native file picker fallback (tinyfiledialogs)
- [ ] Process relaunch on ROM update (Windows + Linux)
- [ ] Shell model extraction from ROM compressed actor segment
- [ ] Thread-safe init with acquire/release memory ordering

### 2b. Collision System
- [ ] `load_level_collision()` — stream Jak tfrag3 collision to SM64 surfaces
- [ ] pat-mode to SM64 surface-type mapping (dynamic classification)
- [ ] "No Slippery Mario" physics toggle
- [ ] Configurable wall slope threshold (NY cutoff)
- [ ] Collision triangle snapshots for debug

### 2c. Mario State Management
- [ ] `MarioState` struct (position, velocity, face angle, health, action, anim frame)
- [ ] Ground pound hitbox (cylinder-vs-point/AABB overlap)
- [ ] Auto-respawn logic with configurable cooldown
- [ ] Mario corpse system (snapshot on death, static geometry, separate VAO per corpse)

### 2d. Input & Control Bridge
- [ ] Stick/button forwarding from GOAL (A/B/Z, dual-stick camera)
- [ ] Cutscene bone tracking (hip, ankle, align skeleton bones)
- [ ] Mario-to-Jak position sync during gameplay
- [ ] Jak-to-Mario sync during state transitions (elevator, death)
- [ ] Hover mode request (atomic flag)

### 2e. Health & Damage Bridge
- [ ] Health wedge sync (Mario 8-wedge to Jak 4-hit, halved + clamped)
- [ ] Knockback, burning, drowning, infinite fall, death, stomp-bounce, hover damage types
- [ ] Star dance animation trigger
- [ ] Shove Mario (zero-damage knockback)

### 2f. Coordinate & Scale System
- [ ] Scale slider (1-500, default 50) affecting both C++ and libsm64
- [ ] Coordinate conversion (4096 Jak units = 1m, scale factor ~81.92 at default)
- [ ] Geometry output buffers (position, normal, color, UV — 1024 tri max/frame)
- [ ] Lerped render state (30Hz tick to 60fps smooth)

## 3. Rendering (`MarioRenderer` + `OpenGLRenderer`)
- [ ] `MarioRenderer` class — VAO/VBO management
- [ ] Texture atlas decode from ROM (704x64)
- [ ] Koopa shell mesh (procedural, vertex colors, transform via Mario pos+angle)
- [ ] Corpse rendering (separate GL mesh per death snapshot)
- [ ] Hook into `OpenGLRenderer` to draw Mario in Jak's render bucket
- [ ] Color preset system (10 recolors: red, orange, yellow, lime, green, cyan, blue, purple, magenta, pink)

## 4. Audio System (`sm64_audio`)
- [ ] `SM64AudioPlayer` with cubeb worker thread
- [ ] 32kHz stereo output from SM64 native audio engine
- [ ] Ring buffer (interleaved int16, read/write cursors)
- [ ] Volume control (0-100, atomic/thread-safe)
- [ ] Sequence/music playback from GOAL
- [ ] SFX dispatch from gameplay code

## 5. Debug Systems
- [ ] ImGui debug panel (`SM64DebugGui`) — ROM path, spawn pos, ground params, surface dump
- [ ] Collision debug renderer (`SM64CollisionRenderer`) — overlay of Mario's collision view
- [ ] Surface snapshot export (OBJ + CSV)
- [ ] Per-tick profiling (microsecond timing)
- [ ] Live runtime config (scale, no-slippery, audio volume)

## 6. GOAL to C++ Bridge (`kmachine.cpp`)
Register 20+ bridge function symbols via `make_func_symbol_func`:
- [ ] Mario spawn/delete/teleport
- [ ] Damage/heal/shove/star-dance
- [ ] Corpse capture/finalize
- [ ] Color/render toggles
- [ ] Health wedge sync
- [ ] ROM detection query
- [ ] Input relay
- [ ] Audio commands (play/stop/volume music, play SFX)
- [ ] Face angle set
- [ ] Hover request

## 7. GOAL Mod Sources (`goal_src/jak1/engine/mods/mario/`)

### 7a. `mario.gc` — Core Mario Process
- [ ] Mario collision process (auto-spawn, safe multi-spawn guards)
- [ ] Frame-by-frame position/health/damage state sync
- [ ] Attack trackers:
  - [ ] Punch (1.5m radius, 15 frames, fist +1.5m forward)
  - [ ] Ground-pound (3m radius, 10 frames, at feet)
  - [ ] Dive (2m radius, 30 frames, follows slide trajectory)
  - [ ] Slide-kick, jump-kick, butt-slide
- [ ] Enemy kill attribution (5% "so long-a Bowser!" quip)
- [ ] Attack window (0.5s post-swing detection)
- [ ] Shell riding:
  - [ ] `sm64-crab-shell` visual follows Mario when mounted
  - [ ] Shell scale 65% x 45%z
  - [ ] Transparent to collision (render-only)
  - [ ] Enemies see Jak as racer-mode
  - [ ] Auto-despawn on dismount or level teleport
- [ ] Jak hiding when Mario active
- [ ] Mario hide during Jak death animations
- [ ] Debug draws (body=red, punch=yellow, ground-pound=blue)

### 7b. `mario-music.gc` — Music System
- [ ] 34+ SM64 tracks mapped to Jak levels
- [ ] Per-level music routing (forced vs pause-responsive modes)
- [ ] Auto-mute when Mario disabled
- [ ] File-select music overlay detection
- [ ] No restart if same track already playing

### 7c. `mario-menu-h.gc` + `mario-menu.gc` — Pause Menu
- [ ] "MARIO OPTIONS" submenu in pause menu
- [ ] Play SM64 Music toggle
- [ ] SM64 Music volume slider
- [ ] SM64 SFX volume slider
- [ ] Render Corpses toggle
- [ ] Clear Corpses button

### 7d. `mario-settings.gc` — Persistent Settings
- [ ] Settings file (same folder as pc-settings)
- [ ] Save/load: music toggle, music volume, SFX volume, corpse render, mario color
- [ ] Lazy-load on first access

### 7e. `mario-debug-menu.gc` — Debug Menu
- [ ] SM64 Sound Previewer (R1=next, L1=prev, Triangle=replay, Square=exit)
- [ ] 411+ SM64 sound effects browsable
- [ ] Color preset selector (10 colors, persisted)

### 7f. Sound Integration
- [ ] 25+ Mario voice clips (yahoo, mama-mia, dying, game-over, etc.)
- [ ] Pause menu sound + coin SFX
- [ ] Camera zoom SFX on goggles/periscope
- [ ] Menu camera zoom in/out SFX
- [ ] Sound volume slider in base sound menu

## 8. Jak Engine Modifications

### 8a. `target-handler.gc` — Combat & Damage
- [ ] Jak can't punch/spin/duck (Mario handles combat)
- [ ] Yellow eco shots still enabled
- [ ] Enemy bounce routing through Mario
- [ ] Ground-pound stomp bounce sync
- [ ] Invincibility frames sync

### 8b. `target.gc` / `target2.gc` — Movement & State
- [ ] Tube-slide detection — force Mario slide-track
- [ ] Ice-skating floor class (`*sm64-on-ice*` to VERY_SLIPPERY)
- [ ] Disable swingpole and ledge-grab
- [ ] Cutscene movement copy
- [ ] Mario speed fix + hover with R2

### 8c. `target-death.gc`
- [ ] Mario hide during death
- [ ] Damage type propagation to Mario

### 8d. Level-Specific Fixes
- [ ] `fisher.gc` — Mario fish minigame, swimming suppression
- [ ] `jungle-mirrors.gc` — plant boss/periscope fixes
- [ ] `bonelurker.gc` — touch tracker fix
- [ ] `snow-bumper.gc` — bumpers + ice support
- [ ] `snow-flutflut-obs.gc` — flutflut course easier
- [ ] `ogre-obs.gc` — fire boulder collision patch
- [ ] `racer.gc` — zoomer/shell handshake
- [ ] `sunken-elevator.gc` — skip sync during ascent
- [ ] `village2-obs.gc` — swamp/tar floor
- [ ] `village3-obs.gc` — fixes
- [ ] `title-obs.gc` — title screen integration
- [ ] `billy.gc` — billy boss fixes
- [ ] `collectables.gc` — orb/cell collection hooks
- [ ] `generic-obs.gc` — actor warping, teetertotters, trampolines, spiderwebs, launchers
- [ ] `orb-cache.gc` — fix getting stuck
- [ ] `game-info.gc` — game info hooks
- [ ] `collide-shape.gc` — ignore collide for projectiles (blue eco fix)
- [ ] `cam-states.gc` — fast-follow camera mode for shell
- [ ] `sage-finalboss.gc` — final boss hooks
- [ ] `flutflut.gc` — flutflut disabled/death

## 9. DGO / Build Descriptors
- [ ] Add all new `.gc` files to `game.gd` (correct load order — after pckernel)
- [ ] Add compile entries to `game.gp`
- [ ] Custom text entries in `game_custom_text_en-US.json`

## 10. Misc / Polish
- [ ] Discord RPC — switch Jak1 to Mario Legacy app ID
- [ ] In-game ROM-required prompt with auto-restart
- [ ] Delay Mario music until title screen
- [ ] HUD health display shows Mario health (div 2) when active
- [ ] `progress-pc.gc` — extended sound options with Mario sliders
- [ ] `progress-h.gc` — new menu index constants
- [ ] `text-h.gc` — new text IDs
- [ ] `pckernel.gc` — integration hooks
- [ ] `gsound.gc` — sound system extensions

## 11. Testing & Verification
- [ ] Builds on Windows (clang)
- [ ] Builds on Linux (clang)
- [ ] ROM detection works (auto + manual picker)
- [ ] Mario spawns and moves in Sandover Village
- [ ] Collision works on all terrain types
- [ ] All 6 attack types register on enemies
- [ ] Shell riding works (mount, ride, dismount)
- [ ] Health syncs both directions (Mario to Jak)
- [ ] Death triggers corpse snapshot
- [ ] Music plays correct track per level
- [ ] Music/SFX volume sliders work
- [ ] Pause menu shows all Mario options
- [ ] Settings persist across restarts
- [ ] Fish minigame completable
- [ ] Sunken elevator doesn't desync
- [ ] Tube slides work
- [ ] Ice surfaces slippery
- [ ] Trampolines, spiderwebs, launchers work
- [ ] Fire boulders don't break collision
- [ ] Blue eco projectiles work
- [ ] Final boss completable
- [ ] Title screen to gameplay transition clean
- [ ] 60fps smooth (lerped rendering)
- [ ] No crashes on level transitions
- [ ] Debug menu functional (sound previewer, color picker, collision overlay)
