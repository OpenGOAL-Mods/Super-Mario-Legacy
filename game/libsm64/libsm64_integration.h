#pragma once

/*!
 * @file libsm64_integration.h
 * Integration of libsm64 (Super Mario 64's Mario) into the Jak engine.
 * Manages the SM64 library lifecycle, Mario instances, collision surfaces,
 * input translation, and provides geometry data for rendering.
 */

#include <array>
#include <cstdint>
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

// Maximum triangles output by libsm64 per frame
static constexpr int GEO_MAX_TRIANGLES = 1024;
// Texture atlas dimensions
static constexpr int TEXTURE_WIDTH = 64 * 11;   // 704
static constexpr int TEXTURE_HEIGHT = 64;

// Scale factors between SM64 units and Jak internal units.
// Jak's coordinate system uses 4096 units per meter (PS2 fixed-point heritage).
// Mario is ~160 SM64 units tall (~1.55m real), so 1 SM64 unit ~ 0.0097m ~ 39.7 Jak units.
// Original estimate: 1 Jak meter = 43 SM64 units. Combined with 4096 units/meter:
//   1 Jak unit = 43/4096 SM64 units, 1 SM64 unit = 4096/43 Jak units.
static constexpr float SM64_TO_JAK_SCALE = 4096.0f / 43.0f;   // ~95.26
static constexpr float JAK_TO_SM64_SCALE = 43.0f / 4096.0f;   // ~0.0105

struct MarioGeometry {
  std::vector<float> position;   // 3 floats per vertex, 3 verts per tri
  std::vector<float> normal;     // 3 floats per vertex
  std::vector<float> color;      // 3 floats per vertex
  std::vector<float> uv;         // 2 floats per vertex
  uint16_t num_triangles = 0;
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

class LibSM64Manager {
 public:
  static LibSM64Manager& instance();

  // Lifecycle
  bool init(const std::string& rom_path);
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

  // Accessors (threadsafe via mutex)
  MarioGeometry get_geometry();
  MarioState get_state();
  GroundPoundHitbox get_ground_pound_hitbox();

  // Texture atlas (only valid after init)
  const uint8_t* get_texture_data() const { return m_texture_data.data(); }
  int get_texture_width() const { return TEXTURE_WIDTH; }
  int get_texture_height() const { return TEXTURE_HEIGHT; }

  // Teleport Jak to Mario's position (makes camera follow Mario).
  // Call resolve_target_symbol() once from the game thread before using sync_jak_to_mario().
  void resolve_target_symbol();
  void sync_jak_to_mario(u8* ee_mem, u32 s7_offset);

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
                             bool dry_run = true);

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
  bool enabled = false;
  bool follow_mario = false;          // Lock Jak's position to Mario's
  bool auto_sync_collision = false;   // Auto-reload static collision when levels change
  bool dynamic_actor_collision = false; // Walk process tree and mirror collide-meshes
  // When set, update_actor_collision walks the tree and logs what it finds
  // but never calls into libsm64. Used to validate the walker in isolation
  // without risking crashes in libsm64 itself.
  bool dynamic_actor_collision_dry_run = false;

 private:
  LibSM64Manager() = default;
  ~LibSM64Manager();

  LibSM64Manager(const LibSM64Manager&) = delete;
  LibSM64Manager& operator=(const LibSM64Manager&) = delete;

  bool m_initialized = false;
  int32_t m_mario_id = -1;
  int m_loaded_surface_count = 0;
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
  } m_type_cache;

  // Memoized type-ancestry tests: (type_ptr) -> is-a-descendant
  std::unordered_map<u32, bool> m_is_process_drawable_cache;
  std::unordered_map<u32, bool> m_is_collide_shape_cache;

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

  // Throttling and diagnostics. The log budget is intentionally large so that
  // when the feature crashes, the most recent lines in the log file pinpoint
  // which mesh / which step died. Reset on shutdown().
  int m_actor_sync_frame = 0;
  int m_actor_diag_logs_remaining = 500;

  std::vector<uint8_t> m_texture_data;  // RGBA texture atlas

  // Double-buffered geometry for thread safety
  std::mutex m_geo_mutex;
  MarioGeometry m_geometry;
  MarioState m_state;
  GroundPoundHitbox m_gp_hitbox;
  uint32_t m_prev_action = 0;        // last frame's mario action, for impact-frame edge detect

  // Pre-allocated buffers for sm64_mario_tick
  std::vector<float> m_tick_position_buf;
  std::vector<float> m_tick_normal_buf;
  std::vector<float> m_tick_color_buf;
  std::vector<float> m_tick_uv_buf;
};

}  // namespace sm64
