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

  // Settings
  bool enabled = false;
  bool follow_mario = false;      // Lock Jak's position to Mario's
  bool auto_sync_collision = false; // Auto-reload collision when levels change

 private:
  LibSM64Manager() = default;
  ~LibSM64Manager();

  LibSM64Manager(const LibSM64Manager&) = delete;
  LibSM64Manager& operator=(const LibSM64Manager&) = delete;

  bool m_initialized = false;
  int32_t m_mario_id = -1;
  int m_loaded_surface_count = 0;
  u32 m_cached_target_sym_offset = 0;  // Cached *target* symbol offset (0 = not yet resolved)

  std::vector<uint8_t> m_texture_data;  // RGBA texture atlas

  // Double-buffered geometry for thread safety
  std::mutex m_geo_mutex;
  MarioGeometry m_geometry;
  MarioState m_state;

  // Pre-allocated buffers for sm64_mario_tick
  std::vector<float> m_tick_position_buf;
  std::vector<float> m_tick_normal_buf;
  std::vector<float> m_tick_color_buf;
  std::vector<float> m_tick_uv_buf;
};

}  // namespace sm64
