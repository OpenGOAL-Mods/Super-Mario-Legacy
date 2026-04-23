#pragma once

/*!
 * @file libsm64_integration.h
 * Integration of libsm64 (Super Mario 64's Mario) into the Jak engine.
 * Manages the SM64 library lifecycle, Mario instances, collision surfaces,
 * input translation, and provides geometry data for rendering.
 */

#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "common/common_types.h"
#include "common/custom_data/Tfrag3Data.h"
#include "common/math/Vector.h"

// Forward-declare the libsm64 structs to avoid pulling in the C header everywhere
struct SM64Surface;
struct SM64MarioInputs;
struct SM64MarioState;
struct SM64MarioGeometryBuffers;

namespace sm64 {

class SM64AudioPlayer;

// Maximum triangles output by libsm64 per frame
static constexpr int GEO_MAX_TRIANGLES = 1024;
// Texture atlas dimensions
static constexpr int TEXTURE_WIDTH = 64 * 11;   // 704
static constexpr int TEXTURE_HEIGHT = 64;

// Scale factors between SM64 units and Jak internal units.
// Jak's coordinate system uses 4096 units per meter (PS2 fixed-point heritage).
// Mario is ~160 SM64 units tall (~1.55m real); dividing by a "Mario scale"
// constant (historically 43 in vanilla SM64, currently 50 by default in
// this project) gives us the Jak-meter/SM64-unit conversion ratio.
//
// These used to be constexpr, but the debug GUI now lets the user adjust
// the scale live via sm64_set_mario_scale().  They're defined as inline
// float variables (C++17) so they have a single definition shared across
// TUs and can be mutated at runtime by set_mario_scale below.  All 55+
// existing call sites read them as plain floats — no change needed at the
// use sites.
//
// The libsm64 C decomp also reads the same underlying value via
// `extern float g_libsm64_mario_scale;` in mario_actions_moving.c and
// mario_actions_submerged.c (walk/swim-speed caps), so set_mario_scale
// updates both sides atomically.
inline float SM64_TO_JAK_SCALE = 4096.0f / 50.0f;   // ~81.92 at scale 50
inline float JAK_TO_SM64_SCALE = 50.0f / 4096.0f;   // ~0.0122 at scale 50

// Single-call scale setter — keeps SM64_TO_JAK_SCALE / JAK_TO_SM64_SCALE
// on the Jak side and g_libsm64_mario_scale on the libsm64 side in lock
// step.  Clamped to [1, 500].
void set_mario_scale(float scale);
// Current scale value (read from g_libsm64_mario_scale so it's always the
// same reference point the C decomp uses).
float get_mario_scale();

// Runtime-adjustable bone indices used by teleport_mario_to_jak during
// cutscenes.  Both point into `(-> *target* node-list data N bone
// transform)`.  See engine/data/joint-nodes.gc for the full table:
//   1  = align (root-align bone)
//   26 = hips
//   29 = Lankle
//   33 = Rankle
//
// `g_cutscene_track_bone` supplies **position** — Lankle (29) works
// well because its world position moves with the animation and sits at
// ground level (matches Mario's feet).
//
// `g_cutscene_track_rot_bone` supplies **rotation** — default is 1
// (align) because Lankle's matrix orientation is the foot's forward
// vector, which wobbles / mis-points whenever the leg isn't straight.
// The align bone is the character's root joint and its forward axis
// tracks Jak's body orientation cleanly.
//
// Set either to -1 to disable that axis and fall back to `root.trans`
// (for position) or `root.quat` (for rotation).  The ImGui panel
// exposes both sliders so you can A/B test live.
inline int g_cutscene_track_bone     = 26;  // position source (Lankle)
inline int g_cutscene_track_rot_bone = 26;   // rotation source (align)

// "No slippery Mario" mode.  When off (default), every tri we stream
// into libsm64 is tagged SURFACE_DEFAULT and the vanilla SM64 slope
// thresholds are used — this is the classic behaviour Mario shipped
// with and it means any slope over ~38° launches him into a slide.
// When on, we:
//   1. Classify each collision tri by its Jak `pat-mode` (wall / ground
//      / obstacle) and map to SURFACE_VERY_SLIPPERY / NOT_SLIPPERY /
//      SLIPPERY respectively, and tag walls with TERRAIN_SLIDE.
//   2. Loosen the normY cutoffs in `mario_floor_is_slippery` so Mario
//      stays on his feet on Jak's steeper-than-SM64-intended geometry.
// Read from both C++ (load_level_collision) and C (mario.c via the
// extern `g_libsm64_no_slippery_mario` int, kept in sync by
// `set_no_slippery_mario` below).  Default OFF because it changes Mario
// physics globally.
inline bool g_no_slippery_mario = true;

// Single-call setter — flips `g_no_slippery_mario` (used in
// load_level_collision) AND mirrors it into the libsm64 C-linkage
// `g_libsm64_no_slippery_mario` int (read by mario.c's slope-class
// thresholds) so both sides stay in sync.  The slope-threshold half
// applies immediately; the pat-mode surface classification only
// re-applies on the next collision stream (level transition / the
// streaming window rolling over around Mario).
void set_no_slippery_mario(bool enabled);
bool get_no_slippery_mario();

struct MarioGeometry {
  std::vector<float> position;   // 3 floats per vertex, 3 verts per tri
  std::vector<float> normal;     // 3 floats per vertex
  std::vector<float> color;      // 3 floats per vertex
  std::vector<float> uv;         // 2 floats per vertex
  uint16_t num_triangles = 0;
};

// POD snapshot of a single static collision triangle as it was submitted to
// libsm64 via sm64_static_surfaces_load.  Positions are in SM64 units
// (i.e. already scaled through JAK_TO_SM64_SCALE).  Used by the debug
// collision renderer and the "Dump Surfaces" button; mirrors
// `struct SM64Surface` from libsm64.h but without pulling that C header
// into this C++ header (which would force every TU that includes this
// to also include libsm64's header).
struct CollisionTriSnapshot {
  float verts[3][3];  // 3 vertices, xyz each, in SM64 units
  int16_t type;        // SURFACE_* (surface_terrains.h)
  int16_t force;
  uint16_t terrain;    // TERRAIN_* (surface_terrains.h)
};

struct MarioState {
  math::Vector3f position{0, 0, 0};
  math::Vector3f velocity{0, 0, 0};
  float face_angle = 0;
  float forward_velocity = 0;
  int16_t health = 0;
  uint32_t action = 0;
  uint32_t flags = 0;
  int32_t anim_id = 0;
  int16_t anim_frame = 0;
};

// Koopa shell mesh extracted at runtime from the SM64 ROM's MIO0-compressed
// actor segment.  Vertex positions are in SM64 model-space units (before
// GEO_SCALE and SM64_TO_JAK_SCALE conversion).  Produced once during
// LibSM64Manager::init() by extract_shell_from_rom(); consumed by
// MarioRenderer::build_shell_local_mesh().
struct ShellMeshData {
  struct Vertex {
    float px, py, pz;   // position (SM64 model units)
    float nx, ny, nz;   // normal (unit-length)
    float u, v;          // texture coords (0..1 for dome, <0 = vertex-color-only)
    float cr, cg, cb;   // per-region vertex colour (from SM64 per-DL lighting)
  };
  std::vector<Vertex> vertices;  // 3 per triangle, flat-expanded
  int tri_count = 0;

  std::vector<uint8_t> texture_rgba;  // decoded RGBA8888 texture image
  int tex_width = 0;
  int tex_height = 0;

  bool valid = false;
};

// Mirror of SM64's mario ground-pound attack hitbox, projected into Jak units.
// Modeled after libsm64's `act_ground_pound`/`act_ground_pound_land` and the
// hitbox in object_stuff.c (radius 37, height 160 SM64u, no down-offset). The
// hitbox is a vertical cylinder centered on Mario; in 2D it's a circle, in Y
// it spans [mario_feet_y, mario_feet_y + 160 SM64u] (or the Jak unit equiv).
//
// `active` is set on every frame the SM64 interaction code would also classify
// as INT_GROUND_POUND_OR_TWIRL: ACT_GROUND_POUND with vel.y < 0 (i.e. the
// entire pound) plus the single first frame of ACT_GROUND_POUND_LAND. The
// `impact_frame` flag distinguishes that 1-frame strong window from the rest
// of the fall.
struct GroundPoundHitbox {
  bool active = false;        // hitbox is dealing damage this frame
  bool impact_frame = false;  // true on the single landing-impact frame
  math::Vector3f center{0, 0, 0};  // Mario world position, Jak units
  float radius = 0.0f;        // cylinder radius, Jak units
  float bottom_y = 0.0f;      // bottom of cylinder, Jak units (Mario's feet)
  float top_y = 0.0f;         // top of cylinder, Jak units (Mario's head)
  uint32_t frames_active = 0; // diagnostic: total frames hitbox has been active
  uint32_t total_hits = 0;    // diagnostic: cumulative actor hits across frames
  uint32_t hits_this_frame = 0; // diagnostic: actors hit on the most recent frame
};

// Cylinder-vs-point overlap check, exposed for unit tests. Returns true if the
// 2D distance between hitbox.center and actor_pos is < hitbox.radius + actor_radius
// AND the actor's y is within [hitbox.bottom_y - actor_half_height,
// hitbox.top_y + actor_half_height]. All inputs in Jak units.
bool ground_pound_hitbox_overlaps(const GroundPoundHitbox& hb,
                                   const math::Vector3f& actor_pos,
                                   float actor_radius,
                                   float actor_half_height);

// Cylinder-vs-AABB overlap check, exposed for unit tests. Conservative test:
// the hitbox's vertical [bottom_y, top_y] interval must overlap [aabb_min.y,
// aabb_max.y], and the hitbox's circle of radius `radius` must overlap the
// AABB's xz rectangle (closest-point-to-circle test). Used for ground-pound
// vs collide-mesh hit detection so we don't miss actors whose root pivot is
// below their collide mesh (most Jak actors).
bool ground_pound_hitbox_overlaps_aabb(const GroundPoundHitbox& hb,
                                        const float aabb_min[3],
                                        const float aabb_max[3]);

struct MarioInputState {
  float stick_x = 0;
  float stick_y = 0;
  float cam_look_x = 0;
  float cam_look_z = 1.0f;
  bool button_a = false;
  bool button_b = false;
  bool button_z = false;
};

// Free functions registered via make_function_symbol_from_c so GOAL can call them.
u64 pc_sm64_damage_mario();
u64 pc_sm64_delete_mario();
u64 pc_sm64_heal_mario();
u64 pc_sm64_full_heal_mario();
u64 pc_sm64_star_dance_mario();
u64 pc_sm64_play_sound(u64 sound_bits);
u64 pc_sm64_play_music(u64 seq_id);
u64 pc_sm64_play_music_forced(u64 seq_id);
u64 pc_sm64_stop_music();
// Set the SM64 audio output volume.  Takes a GOAL float (0.0..100.0)
// packed into a u32, matching the existing pc-sm64-teleport-mario ABI.
u64 pc_sm64_set_music_volume(u32 vol_bits);
u64 pc_sm64_teleport_mario(u32 x, u32 y, u32 z);

class LibSM64Manager {
 public:
  static LibSM64Manager& instance();

  // Lifecycle
  bool init(const std::string& rom_path);
  // Auto-detects the SM64 US ROM by scanning the executable directory and
  // <project>/iso_data/mario/ for any .z64 whose size matches the expected
  // US ROM size (8,388,608 bytes). Returns true on successful init. Use this
  // for default-on-launch initialization.
  bool init_autodetect();
  // Returns the detected ROM path if init_autodetect() or a successful init()
  // has run, otherwise empty. Exposed so the debug GUI can show what was
  // picked.
  const std::string& last_rom_path() const { return m_last_rom_path; }
  void shutdown();
  bool is_initialized() const { return m_initialized; }

  // Mario instance management
  int32_t create_mario(float x, float y, float z);
  void delete_mario(int32_t mario_id);
  bool has_mario() const { return m_mario_id >= 0; }
  int32_t get_mario_id() const { return m_mario_id; }

  // Per-frame tick
  void tick(const MarioInputState& input);

  // Collision surface loading
  void load_flat_ground(float y_height, float half_extent);
  void load_surfaces(const std::vector<SM64Surface>& surfaces);
  void load_level_collision(const std::vector<tfrag3::CollisionMesh::Vertex>& vertices);
  int get_loaded_surface_count() const { return m_loaded_surface_count; }

  // Grab a POD copy of every static collision triangle currently stored on
  // the C++ side of libsm64 (i.e. everything passed to the last
  // sm64_static_surfaces_load call — mind that with streaming enabled this
  // is the ALL set, not the nearby subset libsm64 is actually using for
  // queries).  Cheap to call but copies the whole list, so don't poll it.
  // Thread-safe — takes m_sm64_lock briefly.  Used by the debug collision
  // renderer and the "Dump Surfaces" button.
  std::vector<CollisionTriSnapshot> snapshot_static_surfaces();

  // Monotonically increasing counter bumped every time the static surface
  // list is replaced (load_level_collision / load_surfaces / shutdown).
  // Used by the collision renderer to skip re-uploading GPU buffers when
  // nothing has changed.
  uint64_t static_surfaces_version() const { return m_static_surfaces_version; }

  // Accessors (threadsafe via mutex)
  MarioGeometry get_geometry();
  MarioState get_state();
  // Returns Mario's state lerped between the last two 30Hz ticks using render_blend.
  // For use by the renderer only — gameplay code should use get_state().
  MarioState get_render_state();
  GroundPoundHitbox get_ground_pound_hitbox();

  // Audio volume (0..100). Applied on the cubeb worker thread, lock-free.
  void set_audio_volume(int volume);
  int get_audio_volume() const;

  // Texture atlas (only valid after init)
  const uint8_t* get_texture_data() const { return m_texture_data.data(); }
  int get_texture_width() const { return TEXTURE_WIDTH; }
  int get_texture_height() const { return TEXTURE_HEIGHT; }

  // Koopa shell mesh extracted from the SM64 ROM (only valid after init).
  const ShellMeshData& get_shell_mesh() const { return m_shell_mesh; }

  // Teleport Jak to Mario's position (makes camera follow Mario).
  // Call resolve_target_symbol() once from the game thread before using sync_jak_to_mario().
  void resolve_target_symbol();
  void sync_jak_to_mario(u8* ee_mem, u32 s7_offset);

  // Reads Jak's current world position (in Jak units) and Y-axis yaw (in
  // radians) from the live *target* process in EE memory. Returns false if
  // *target* isn't resolvable (e.g. no game running). Used by the debug GUI
  // to spawn Mario where Jak currently stands.
  bool read_target_transform(u8* ee_mem, math::Vector3f* out_pos, float* out_yaw_rad);

  // Debug helper: returns the same position teleport_mario_to_jak would
  // pick this frame (bone at g_cutscene_track_bone if available, else
  // *target* root.trans).  Pure-read, no side effects.  `out_used_bone`
  // is set to the bone index we actually read (or -1 if we fell back to
  // root.trans) so the debug GUI can show which source fired.
  bool read_cutscene_track_position(u8* ee_mem,
                                    math::Vector3f* out_pos,
                                    int* out_used_bone = nullptr);

  // Diagnostics: count of times teleport_mario_to_jak has actually been
  // invoked this frame / session.  Bumped from inside the function.
  // Reset nowhere — just a monotonically increasing counter so the ImGui
  // panel can show a rate to confirm the gate is firing.
  uint32_t teleport_call_count() const { return m_teleport_call_count; }

  // Force Mario's yaw (in radians, world Y axis). No-op if no Mario is spawned.
  void set_mario_face_angle(float yaw_rad);

  // Reads Jak's current water state from EE memory and mirrors it into
  // libsm64 via sm64_set_mario_water_level. If the Jak player is submerged
  // (water-control.flags wt09 bit set) the surface Y is pulled from
  // water-control.height and converted to SM64 units; otherwise we set a
  // very low water level so Mario stays dry. No-op if Mario isn't spawned
  // or the water-sync toggle is off.
  void update_mario_water(u8* ee_mem);

  // Check if the Jak game is paused by reading *master-mode* from GOAL memory.
  // Returns true if master-mode is anything except 'game (pause / menu /
  // movie / freeze / …).
  bool is_game_paused(u8* ee_mem);

  // Returns true when Jak's master-mode is specifically 'movie.  Used by
  // the Mario pipeline to distinguish "skip everything" pause states
  // from cutscene movies where we want the pipeline to KEEP RUNNING so
  // teleport_mario_to_jak can glue Mario to the animating skeleton each
  // frame.
  bool is_in_movie(u8* ee_mem);

  // Write Mario's position into a target process's root->trans in EE memory.
  // Returns true if the write succeeded. Exposed for testing.
  static bool write_mario_pos_to_target(u8* ee_mem,
                                        u32 ee_mem_size,
                                        u32 false_val,
                                        u32 target_ptr,
                                        const math::Vector3f& mario_pos);

  // Dynamic actor collision: walks the Jak process tree each tick, finds
  // process-drawables with collide-shape meshes, and bridges them into libsm64
  // as surface objects so Mario can stand on/collide with moving platforms,
  // crates, enemies, etc.
  //
  // Call this from the game-side tick (it walks EE memory via `ee_mem`).
  // Safe to call when disabled — it's a no-op then.
  void update_actor_collision(u8* ee_mem);

  // Tear down all tracked actor surface objects (on level change, disable, etc.)
  void clear_actor_collision();

  // Write Mario's position, face angle, and attacking state to the GOAL-side
  // bridge symbols (*sm64-mario-pos*, *sm64-mario-info*).  Also reads
  // *sm64-mario-hit* and clears it if Jak struck Mario.
  void write_mario_bridge_data(u8* ee_mem);

  // Called from GOAL (via make_function_symbol_from_c "pc-sm64-damage-mario")
  // when a GOAL enemy hits Mario's body sphere.  Thread-safe — grabs m_sm64_lock.
  void damage_mario_from_goal();
  void heal_mario_from_goal();
  void full_heal_mario_from_goal();
  void star_dance_mario_from_goal();
  void play_sound_from_goal(int32_t sound_bits);
  void play_music_from_goal(uint8_t seq_id);
  // Same as play_music_from_goal but flips the "force unpaused" flag so the
  // track keeps playing even while Jak's game is paused (title screen,
  // menus, save-select, etc.).  Calling the non-forced variant (or
  // stop_music_from_goal) clears the flag and restores pause-responsive
  // behavior for subsequent pause transitions.
  void play_music_forced_from_goal(uint8_t seq_id);
  void stop_music_from_goal();

  // Per-frame music pause sync — called from the renderer BEFORE the
  // pause-gate early return so we can stop libsm64's BG music the moment
  // Jak's game pauses and resume the remembered track on unpause.
  void update_music_pause_state(bool is_paused);
  void teleport_mario_from_goal(float x, float y, float z);

  // Teleport Mario to Jak's current position/rotation (used during cutscenes).
  void teleport_mario_to_jak(u8* ee_mem);
  // Debug: glue Mario to Jak's position, zeroing velocity and forcing freefall
  // so SM64 physics don't snap him back to the ground.
  void debug_glue_mario_to_jak(u8* ee_mem);
  // Read *sm64-target-flags* bridge vector and update public flags below.
  void read_target_flags(u8* ee_mem);

  // Flags set by read_target_flags(), used by OpenGLRenderer for render/input control.
  bool target_grabbed = false;    // cutscene, clone-anim, or periscope
  bool target_periscope = false;  // periscope specifically
  bool target_clone_anim = false; // blue eco doors/bridges — grabbed but don't teleport
  bool target_in_movie = false;   // (movie?) — scene-player / cutscene movies active

  // Yakow grab: walks the Jak process tree each tick, finds yakow actors,
  // and lets Mario pick one up with the grab button (punch) when standing
  // close to it. While held, the yakow's trans is overwritten each frame to
  // glue it to Mario's hand position (computed from mario pos + face angle).
  // On natural release (throw, drop, damage), the yakow's trans is left
  // wherever Mario put it and its normal AI resumes.
  //
  // This uses the libsm64 fake-held-object API (sm64_mario_begin_fake_hold /
  // sm64_mario_is_holding_fake) so Mario's stock pickup/hold/throw actions
  // drive the interaction without needing a real SM64 Object to back it.
  //
  // Call this from the game-side tick AFTER tick() and after
  // sync_jak_to_mario, so Mario's position for this frame is current.
  // Safe to call when disabled — it's a no-op then.
  void update_yakow_grab(u8* ee_mem);

  // Release any held yakow and reset grab state. Called on level change,
  // Mario delete, yakow_grab toggle off, or shutdown.
  void clear_yakow_grab();

  // Zoomer→shell: each tick, maintain the GOAL-side handshake that keeps Jak
  // out of the target-racing-* states and puts Mario into ACT_RIDING_SHELL_GROUND
  // when the player tries to hop on a zoomer.
  //
  // Two GOAL symbols drive this (defined in target-handler.gc):
  //   *sm64-skip-zoomer*      — C++ sets this to #t while Mario exists and
  //                             zoomer_shell is enabled. When #t, target's
  //                             'racing change-mode event handler swallows
  //                             the event instead of going into the racer
  //                             (avoiding the racer's bad camera and loud
  //                             engine SFX). When #f, Jak races normally.
  //   *sm64-zoomer-requested* — GOAL sets this to #t whenever a 'racing event
  //                             was swallowed (i.e. the player is hitting
  //                             attack near a zoomer). C++ polls it each
  //                             tick; on #t it puts Mario into the ground
  //                             shell action and clears it back to #f.
  //
  // Since Mario inherits the riding-shell action flag while he's in the shell
  // action, the native check_lava_boost no-ops for lava floors
  // (interaction.c:902) — we also skip the host-side lava kick in
  // update_mario_water for the same reason, so lava rocks in Fire Canyon and
  // Lava Tube don't hurt Mario while he's on the shell.
  //
  // No-op if libsm64 isn't initialized or the feature toggle is off. Safe to
  // call unconditionally each tick — gracefully no-ops if the GOAL symbols
  // aren't present yet (e.g. kernel loaded but target-handler not linked).
  void update_zoomer_shell(u8* ee_mem);

  // Edge-detect Jak's target-clone-anim state (fuel-cell pickup / blue
  // eco door / bridge cutscenes) and snapshot Mario's full sim state at
  // the rising edge.  When the cutscene ends, restore position + velocity
  // + face angle + forward velocity + action so Mario resumes exactly as
  // he was before the interaction — most importantly, still on the shell
  // if he was on a shell when the cell was grabbed.  Call once per tick.
  void update_shell_preserve_across_cell_grab();

  // Launcher glue: detects when Jak is launched by a spring pad (rapid
  // upward Y velocity) and glues Mario to Jak's trajectory instead of
  // the normal Mario→Jak sync. Called after tick() but before
  // sync_jak_to_mario. Returns true if Mario is currently glued to a
  // launcher trajectory (caller should skip sync_jak_to_mario).
  bool update_launcher_glue(u8* ee_mem);

  // Safety floor: create-or-move the pseudo-floor quad so it's glued just
  // below Mario's current XYZ. See `safety_floor` comment for the rationale.
  // Called from tick() before sm64_mario_tick each frame. No-op if the
  // toggle is off or Mario isn't spawned. Must be called with m_sm64_lock
  // held — it calls sm64_surface_object_create/move which touch libsm64
  // global state.
  void update_safety_floor(float mario_x_sm64, float mario_y_sm64, float mario_z_sm64);
  // Delete the safety floor surface object if one exists. Called on Mario
  // respawn, delete, and shutdown.
  void clear_safety_floor();


  // --- Testing hooks ---------------------------------------------------------
  // Run one actor-collision sweep against a custom EE memory buffer (with a
  // custom "false" value and EE memory size). Used by unit tests to validate
  // the DFS/extraction logic without needing a real game. Returns the number
  // of meshes that were successfully extracted (and in dry_run mode, the
  // number that *would* have been submitted to libsm64).
  struct TestSweepResult {
    int meshes_found = 0;
    int triangles_extracted = 0;
    int process_tree_nodes_visited = 0;
    int process_drawables_seen = 0;
    int errors = 0;
    // Bone-attached prim diagnostics: whenever a prim has transform_index >= 0,
    // we increment bone_lookups_attempted. If compute_prim_world_transform
    // successfully reads the bone matrix it bumps bone_lookups_succeeded;
    // otherwise it bumps bone_lookups_fell_back (use_root path taken).
    int bone_lookups_attempted = 0;
    int bone_lookups_succeeded = 0;
    int bone_lookups_fell_back = 0;
    // Captured per-prim transforms (only filled in tests; one entry per prim
    // processed). Each entry is (jak_x, jak_y, jak_z, transform_index).
    struct CapturedPrim {
      float pos[3];
      int transform_index;
      bool used_bone;
    };
    std::vector<CapturedPrim> captured_prims;
  };
  TestSweepResult test_sweep(u8* ee_mem,
                             u32 ee_mem_size,
                             u32 false_val,
                             u32 active_pool_sym,
                             u32 process_drawable_type,
                             u32 collide_shape_type,
                             u32 prim_mesh_type,
                             u32 prim_group_type,
                             bool dry_run = true,
                             // Optional camera-type filter pointers. Tests that
                             // don't care about these can leave them at 0, which
                             // disables the filter (matches the pre-filter
                             // behavior, so existing tests pass unchanged).
                             u32 pov_camera_type = 0,
                             u32 citadelcam_type = 0,
                             // Optional prim-sphere type. 0 disables sphere
                             // handling (tests that don't care about spheres
                             // leave this at its default, matching the
                             // pre-sphere behavior — sphere prims are silently
                             // skipped by the walker).
                             u32 prim_sphere_type = 0);

  // Per-actor tracking record. Public so the internal sweep walker (which lives
  // in an anonymous namespace in the .cpp) can refer to it from a context struct.
  // m_tracked_actors itself is still private.
  struct TrackedActor {
    uint32_t sm64_obj_id = 0;
    bool has_obj = false;   // sm64_obj_id=0 is a valid libsm64 id, so track separately
    float last_trans[3] = {0, 0, 0};
    float last_quat[4] = {0, 0, 0, 1};
    bool seen_this_frame = false;
    // Local-space AABB of the collide mesh in Jak units, captured once at
    // extraction time. Used for hit testing — last_trans is the prim anchor
    // (often at the actor's base), so testing against the prim point misses
    // anything stacked on top of the mesh.
    float local_aabb_min[3] = {0, 0, 0};
    float local_aabb_max[3] = {0, 0, 0};
    bool has_aabb = false;
    // World-space AABB of the collide mesh in Jak units, recomputed each
    // frame from local_aabb_{min,max} + the prim's world transform.
    float world_aabb_min[3] = {0, 0, 0};
    float world_aabb_max[3] = {0, 0, 0};
  };

  // Settings
  bool enabled = true;
  bool follow_mario = true;           // Lock Jak's position to Mario's
  bool auto_sync_collision = true;    // Auto-reload static collision when levels change
  bool dynamic_actor_collision = true; // Walk process tree and mirror collide-meshes
  bool hide_jak_model = true;         // Skip drawing eichar-lod0 while Mario is active
  bool water_sync = true;             // Mirror Jak's water volume into libsm64 each tick
  // When set, SM64CollisionRenderer draws the loaded static collision
  // triangles as a translucent colored overlay (one colour per SURFACE_*
  // type) plus a wireframe pass on top, so you can eyeball where Mario's
  // view of the world differs from Jak's.  Off by default.
  bool show_collision = false;

  // Experimental collision-loader supplement.  libsm64 classifies surfaces
  // purely by normal.y — `|ny| > 0.01` is a floor, `|ny| <= 0.01` is a
  // wall (surface_collision.c:104 and 180).  Jak's pat-mode=WALL tris
  // aren't always geometrically vertical, so many of them fall on the
  // "floor" side of that cutoff and are invisible to
  // find_wall_collisions.  At speed, Mario tunnels straight through
  // them because each sub-step's XZ jumps past the tri's footprint
  // entirely — no wall hit, no find_floor hit either.
  //
  // When this toggle is on:
  //   - Degenerate (zero-area) tris are dropped so they don't NaN
  //     libsm64's find_floor.
  //   - For every pat-mode=WALL tri with `0.01 < |ny| <= wall_extrusion_ny_max`
  //     we ADDITIONALLY emit a pair of perfectly-vertical triangles
  //     forming a wall quad across the source tri's longest XZ edge and
  //     Y range.  These quads have ny=0 exactly, so find_wall_collisions
  //     picks them up and fast Mario bumps off them cleanly.  Winding
  //     matches the source tri's outward XZ normal.
  //   - Tris past the `wall_extrusion_ny_max` cutoff (geometrically more
  //     like slopes than walls) are LEFT ALONE — extruding a 30-degree
  //     ramp into a vertical quad creates a tall false wall that blocks
  //     Mario from climbing slopes Jak considers walkable.
  //   - The ORIGINAL tilted tri is always emitted with its normal
  //     VERY_SLIPPERY + TERRAIN_SLIDE tagging, so find_floor continues
  //     to pick it up and Mario slides on its surface exactly as he did
  //     before.
  //
  // Auto-reload picks up the new value on flip / slider release, and a
  // fresh stream also runs on every level transition / streaming window
  // roll.  Default ON — the wall-extrusion supplement fixes the
  // "tunnel through tilted walls at speed" case that's present in the
  // legacy loader, and has been validated against the sandover wall
  // geometry that first surfaced the bug.
  bool test_new_collide_toggle = true;

  // Upper bound on |normal.y| for the wall-extrusion supplement above.
  // Tris with `|ny| > wall_extrusion_ny_max` are considered slopes, not
  // walls, and skipped by the extruder.  Lower values = fewer extrusions
  // (only the most vertical walls); higher = more (risk of false walls
  // on mildly sloped surfaces).  Exposed as an ImGui slider so you can
  // dial it live between collision reloads.  Default 0.30 ≈ 72° from
  // horizontal: captures near-vertical walls while leaving genuine
  // ramps alone.
  float wall_extrusion_ny_max = 0.30f;
  // Maximum height (Jak units) of each extruded vertical wall quad.
  // The quad is Y-centered on the source tri's centroid and clamped to
  // this height, so large sloped tris don't produce invisible wall slabs
  // taller than one or two Mario heights.  Default 120 ≈ ~1.5 Mario
  // heights at scale 50.  Tunable live via ImGui; takes effect on the
  // next collision stream.
  float wall_extrusion_height_cap = 3000.0f;
  // When set, update_actor_collision walks the tree and logs what it finds
  // but never calls into libsm64. Used to validate the walker in isolation
  // without risking crashes in libsm64 itself.
  bool dynamic_actor_collision_dry_run = false;

  // Yakow grab feature — default on. When enabled, update_yakow_grab walks
  // the process tree looking for yakow actors and uses the libsm64 fake-hold
  // API to let Mario pick them up on B-press when close.
  bool yakow_grab = true;
  // Zoomer→shell feature — default on. When enabled, update_zoomer_shell
  // watches Jak's state each tick and forces Mario into ACT_RIDING_SHELL_GROUND
  // while Jak is in any target-racing-* state (i.e. while he's on the zoomer).
  // Also gives Mario lava immunity in that state so the lava rocks in
  // Fire Canyon / Lava Tube don't burn him while the zoomer is in use.
  bool zoomer_shell = true;
  // Pseudo safety-floor: a small flat quad glued 200 SM64 units below
  // Mario each tick so libsm64's find_floor query always returns a valid
  // surface. Just meant to stop the "Mario walks off the edge into a
  // NULL floor" failure mode.
  bool safety_floor = true;
  // How far below Mario (in SM64 units) the safety quad sits.
  float safety_floor_drop_sm64 = 200.0f;
  // Collision streaming: instead of feeding all level triangles to libsm64 at
  // once, only feed those within a radius of Mario. Re-filter when Mario moves
  // far enough from the last reload center. Dramatically reduces the O(n) cost
  // of libsm64's brute-force find_floor / find_ceil / find_wall_collisions.
  bool collision_streaming = true;
  // Radius (SM64 units) around Mario within which triangles are loaded.
  // 6000 SM64u ≈ ~570k Jak units ≈ ~140m — covers a generous play area.
  float collision_stream_radius = 6000.0f;
  // Mario must move this far (SM64 units) from the last reload center before
  // we re-filter. Set to ~40% of the radius so there's always a comfortable
  // buffer of loaded collision ahead of Mario.
  float collision_stream_reload_threshold = 2500.0f;
  // Proximity threshold (SM64 units) for initiating a grab when B is pressed.
  // 160 SM64u ≈ Mario's height. At 43 SM64u/Jak meter that's ~3.7m.
  float yakow_grab_radius_sm64 = 160.0f;
  // Forward offset (SM64 units) from Mario's feet toward his face angle,
  // where the held yakow is glued. Roughly matches the light-object HOLP.
  float yakow_hold_forward_sm64 = 60.0f;
  // Up offset (SM64 units) from Mario's feet for the held yakow.
  float yakow_hold_up_sm64 = 80.0f;

  // Visual interpolation blend factor for the renderer.
  // Set each frame by tick_mario_sm64() so the renderer can smoothly
  // lerp between the previous and current 30Hz tick positions at 60fps.
  // 0.0 = show previous tick, 1.0 = show current tick.
  // NOT guarded by m_geo_mutex — only written/read from the render thread.
  float render_blend = 1.0f;

 private:
  LibSM64Manager() = default;
  ~LibSM64Manager();

  // Parse the koopa shell model + texture from the ROM bytes stored in
  // m_rom_data.  Called once during init(); frees m_rom_data when done.
  bool extract_shell_from_rom();

  LibSM64Manager(const LibSM64Manager&) = delete;
  LibSM64Manager& operator=(const LibSM64Manager&) = delete;

  bool m_initialized = false;
  std::string m_last_rom_path;  // path of the ROM passed to the last successful init
  int32_t m_mario_id = -1;
  int m_loaded_surface_count = 0;
  int m_audio_volume = 100;  // latched value, also mirrored into m_audio on start
  u32 m_cached_target_sym_offset = 0;  // Cached *target* symbol offset (0 = not yet resolved)

  // ---- Dynamic actor collision state ------------------------------------------------
  // Cached type pointers (populated lazily on first actor-collision tick).
  // These are the .value of the type symbols, i.e. addresses of Type structs.
  struct TypeCache {
    bool ready = false;
    u32 active_pool_sym = 0;   // symbol address of *active-pool*
    u32 process_drawable = 0;  // type ptr
    u32 collide_shape = 0;     // type ptr (includes collide-shape-moving + control-info)
    u32 prim_mesh = 0;         // type ptr for collide-shape-prim-mesh
    u32 prim_group = 0;        // type ptr for collide-shape-prim-group
    // Sphere prims need their own path through the walker. scarecrow-a /
    // scarecrow-b (and a bunch of other sphere-only actors) use prim-sphere
    // under a prim-group root, so if we skip them Mario falls through them.
    // Looked up from the same symbol bundle as prim_mesh/prim_group (ENGINE).
    u32 prim_sphere = 0;       // type ptr for collide-shape-prim-sphere
    // Camera-related process-drawable types we never want to feed into Mario's
    // world collision. `pov-camera` is in ENGINE so it's always available.
    // `citadelcam` is in the citadel level DGO and stays 0 until that level
    // is loaded — a 0 here is fine, it just disables that specific filter.
    u32 pov_camera = 0;        // type ptr for pov-camera (cutscenes)
    u32 citadelcam = 0;        // type ptr for citadelcam (citadel level)
    // Bouncy trampoline types — level-specific, resolved lazily. 0 means the
    // level with that type isn't loaded (fine, disables the filter).
    u32 springbox = 0;         // type ptr for springbox (jungle bouncer)
    u32 spiderwebs = 0;        // type ptr for spiderwebs (maincave bouncer)
    u32 teetertotter = 0;      // type ptr for teetertotter (misty seesaw)
    u32 sm64_mario_col = 0;    // type ptr for sm64-mario-col (our GOAL hitbox process)
    u32 touch_tracker = 0;     // type ptr for touch-tracker (attack/eco hitboxes, not solid)
    u32 projectile = 0;        // type ptr for projectile (temporary attack effects, not solid)
  } m_type_cache;

  // Memoized type-ancestry tests: (type_ptr) -> is-a-descendant
  std::unordered_map<u32, bool> m_is_process_drawable_cache;
  std::unordered_map<u32, bool> m_is_collide_shape_cache;
  std::unordered_map<u32, bool> m_is_pov_camera_cache;
  std::unordered_map<u32, bool> m_is_citadelcam_cache;
  std::unordered_map<u32, bool> m_is_projectile_cache;

  // Tracked actor collision objects, keyed by ((process-drawable EE addr) << 32 |
  // collide-mesh EE addr). Different actor instances can SHARE the same
  // collide-mesh template (e.g., crates), so keying by mesh alone caused two
  // actors to stomp on each other's libsm64 surface object every frame.
  // Value: the libsm64 surface object id and a "signature" of the last transform
  // we submitted, so we can detect movement and only update when needed.
  // (TrackedActor struct itself is declared in the public section above.)
  std::unordered_map<uint64_t, TrackedActor> m_tracked_actors;

  // Set of mesh pointers that have failed extraction — we blacklist them to
  // avoid re-trying every frame and spamming logs.
  std::unordered_set<u32> m_broken_meshes;

  // ---- Yakow grab state -------------------------------------------------
  // Cached yakow type ptr (resolved lazily on first tick via find_symbol_from_c).
  // 0 means "not yet resolved this session".
  u32 m_yakow_type = 0;
  // Memoized "is this type a yakow descendant" cache (usually yakow itself,
  // but we go through type_is_descendant for consistency with the existing
  // walker helpers so yakow subtypes would also match if they ever exist).
  std::unordered_map<u32, bool> m_is_yakow_cache;
  // Process-tree EE address of the currently held yakow, or 0 if not holding.
  // While non-zero, update_yakow_grab rewrites this process's trans each
  // frame to glue it to Mario's hand.
  u32 m_grabbed_yakow_ee = 0;
  // Edge-detect state for the grab/throw button (button_b, which the input
  // layer maps to Square). Updated from MarioInputState each tick() so
  // update_yakow_grab can detect a single-frame press vs a hold.
  bool m_prev_button_b = false;
  bool m_cur_button_b = false;

  // ---- Zoomer→shell state ----------------------------------------------
  // No persistent state — update_zoomer_shell does fresh find_symbol_from_c
  // lookups each tick on *sm64-skip-zoomer* / *sm64-zoomer-requested*. The
  // lookups are hash-table reads and consistent with how sync_jak_to_mario
  // resolves *target* every frame. Also no local "in shell" latch — we
  // simply set the shell action whenever the GOAL-side flag fires and let
  // Mario run his own physics from there.

  // Throttling and diagnostics. The log budget is intentionally large so that
  // when the feature crashes, the most recent lines in the log file pinpoint
  // which mesh / which step died. Reset on shutdown().
  int m_actor_sync_frame = 0;
  int m_actor_diag_logs_remaining = 500;

  // ---- Collision streaming state -----------------------------------------
  // All processed SM64Surface structs from the last load_level_collision call.
  // We keep these on our side and only feed a spatial subset to libsm64.
  std::vector<SM64Surface> m_all_static_surfaces;
  // Version counter bumped every time m_all_static_surfaces is replaced.
  // Read via static_surfaces_version(); lets SM64CollisionRenderer skip
  // the GPU re-upload when nothing has changed.
  uint64_t m_static_surfaces_version = 0;
  // Per-triangle XZ centroid in SM64 coords, parallel to m_all_static_surfaces.
  // Pre-computed once at load time to avoid recomputing every streaming pass.
  struct SurfaceCentroid { float x, z; };
  std::vector<SurfaceCentroid> m_surface_centroids;
  // The SM64-unit position where we last reloaded the streaming subset.
  float m_stream_center_x = 0.0f, m_stream_center_z = 0.0f;
  // True once we've done at least one streaming load (so we can detect the
  // initial case where Mario spawns but no subset has been sent yet).
  bool m_stream_loaded = false;
  // Number of triangles in the current streaming subset (for debug display).
  int m_stream_loaded_count = 0;

  // Called inside tick() (under sm64_lock) to reload the nearby subset when
  // Mario moves far enough from the last center. No-op if streaming is off.
  void update_streaming_collision(float mario_x_sm64, float mario_z_sm64);

  // ---- Safety floor state ----------------------------------------------
  // The libsm64 surface-object id we allocated for the safety quad, valid
  // only when `m_safety_floor_created` is true. Created lazily on the first
  // tick after Mario is spawned; destroyed on delete_mario/shutdown/
  // respawn so the next spawn starts fresh. The quad's Y is recomputed from
  // Mario's current Y each frame in update_safety_floor — there is no
  // cached Y because the whole point is for it to follow him.
  uint32_t m_safety_floor_id = 0;
  bool m_safety_floor_created = false;

  // ---- Shell-over-water state --------------------------------------------
  // Set each frame by update_mario_water (runs before tick). When true, the
  // post-tick correction in tick() keeps Mario at the water surface while
  // shell-riding, preventing any brief SHELL_FALL bounce.
  bool m_in_water_volume = false;
  float m_water_level_sm64 = 0.0f;

  // ---- Launcher glue state ------------------------------------------------
  // When Jak uses a launcher (spring pad), GOAL controls Jak's trajectory.
  // We detect this by reading Jak's GOAL state name and comparing against
  // the launcher states (target-launch, target-high-jump, etc.). While
  // active, we skip the normal Mario→Jak sync and glue Mario to Jak's
  // position until the launch state ends.
  //
  // Detection happens in update_launcher_glue (runs before tick).
  // Actual position override happens inside tick() after sm64_mario_tick,
  // within the existing sm64_lock scope, avoiding external sm64 API calls.
  bool m_in_launcher = false;
  math::Vector3f m_launcher_target_jak{0, 0, 0};  // Jak-unit position to glue to
  // Post-glue settle: when a glue state (launcher, warp, continue) ends, keep
  // Mario pinned to Jak's position for a few extra frames so that
  // auto_sync_collision has time to detect the new level and reload collision
  // surfaces. Without this, Mario falls through the ground on level changes
  // triggered by continue points / warp gates because the collision reload
  // hasn't happened yet when the glue releases.
  int m_post_glue_settle_frames = 0;
  static constexpr int POST_GLUE_SETTLE_DURATION = 30;  // ~1 second at 30Hz tick rate

  // ---- Music pause/resume state ----------------------------------------
  // The actual audio gating happens in SM64AudioPlayer::m_paused — when
  // true, its cubeb callback emits silence without ticking the N64 audio
  // engine, so the active sequence cursor stays frozen and unpause resumes
  // in-place.  The two fields below drive whether we should be in that
  // state:
  //   `m_game_paused` mirrors update_music_pause_state's last input —
  //     i.e. "is Jak's game currently in a non-game master-mode (paused /
  //     menu / freeze / progress / etc.)?".
  //   `m_force_audio_unpaused` is set by play_music_forced_from_goal and
  //     cleared by play_music_from_goal / stop_music_from_goal.  When true,
  //     audio never pauses regardless of `m_game_paused` — used for title
  //     screen / menu music that should keep playing while master-mode
  //     sits at something other than 'game.
  //   Desired audio-paused = m_game_paused && !m_force_audio_unpaused.
  bool m_game_paused = false;
  bool m_force_audio_unpaused = false;

  std::vector<uint8_t> m_texture_data;  // RGBA texture atlas

  // ROM bytes — kept briefly during init() for shell extraction, then freed.
  std::vector<uint8_t> m_rom_data;
  // Shell mesh + texture extracted from the ROM (persists for renderer use).
  ShellMeshData m_shell_mesh;

  // Serializes calls into libsm64 global state. sm64_mario_tick() and
  // sm64_audio_tick() both touch shared internal state, so the audio worker
  // thread holds this while pulling PCM, and the main thread holds it during
  // tick(). Separate from m_geo_mutex which only guards the Mario geometry
  // read/write snapshots.
  std::mutex m_sm64_lock;
  std::unique_ptr<SM64AudioPlayer> m_audio;

  // Double-buffered geometry for thread safety
  std::mutex m_geo_mutex;
  MarioGeometry m_geometry;
  MarioState m_state;
  // Previous tick's geometry and state, used for render-side interpolation.
  MarioGeometry m_prev_geometry;
  MarioState m_prev_state;
  GroundPoundHitbox m_gp_hitbox;
  uint32_t m_prev_action = 0;        // last frame's mario action, for impact-frame edge detect
  // Last frame's "is Mario submerged in a lava water-vol" flag. Used by
  // update_mario_water to detect the dry->lava entry edge, so each time
  // Mario falls back into the lava plane during an ACT_LAVA_BOOST arc we
  // can re-fire the bounce even though he's still in a fire action. Without
  // this edge detection Mario would launch once on first contact, then
  // pass through the lava surface without any reaction on the second drop.
  bool m_prev_in_lava = false;
  int m_star_dance_timer = -1;  // -1 = inactive, >= 0 = frames since star dance started
  // Diagnostic counter bumped every time teleport_mario_to_jak fires.
  // The ImGui panel displays the delta-per-frame to confirm the cutscene
  // gate is actually reaching the teleport function.
  uint32_t m_teleport_call_count = 0;

  // Cell-pickup state preservation.  Snapshotted at the rising edge of
  // target_clone_anim (cutscene start) and restored at the falling edge
  // so Mario picks back up exactly where he was — same position, same
  // velocity, same action.  Without this Mario would drift during the
  // ~1-2s cutscene (gravity, action timeouts, etc.) and most visibly
  // fall off a shell he was riding when the cell was grabbed.
  bool m_prev_target_clone_anim = false;
  bool m_clone_anim_snapshot_valid = false;
  MarioState m_clone_anim_snapshot;

  // Post-cutscene freeze.  On the cutscene's falling edge we restore the
  // libsm64 state, then flag `pending_settle`.  The next tick() runs
  // normally (so sm64_mario_tick propagates the restored pose into
  // m_state/m_geometry and the renderer sees it), and on its way out
  // converts the flag into `freeze_ticks = 90` which makes subsequent
  // ticks skip sm64_mario_tick for 3 s.  That holds Mario still long
  // enough for Jak's camera to catch up to his new position before
  // physics resumes and he zips off on the shell.
  bool m_post_restore_pending_settle = false;
  int  m_post_restore_freeze_ticks = 0;
  static constexpr int kPostCloneAnimFreezeTicks = 60;  // @ 30 Hz

  // Pre-allocated buffers for sm64_mario_tick
  std::vector<float> m_tick_position_buf;
  std::vector<float> m_tick_normal_buf;
  std::vector<float> m_tick_color_buf;
  std::vector<float> m_tick_uv_buf;
};

}  // namespace sm64
