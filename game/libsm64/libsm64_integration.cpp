/*!
 * @file libsm64_integration.cpp
 * Implementation of the libsm64 integration manager.
 */

#include "libsm64_integration.h"

#include <cstring>
#include <fstream>

#include "common/goal_constants.h"
#include "common/log/log.h"
#include "common/util/FileUtil.h"
#include "game/kernel/common/Ptr.h"
#include "game/kernel/common/Symbol4.h"
#include "game/kernel/common/kscheme.h"
#include "game/kernel/jak1/kscheme.h"

extern "C" {
#include "libsm64.h"
}

namespace sm64 {

static void sm64_debug_print(const char* msg) {
  lg::info("[libsm64] {}", msg);
}

LibSM64Manager& LibSM64Manager::instance() {
  static LibSM64Manager mgr;
  return mgr;
}

LibSM64Manager::~LibSM64Manager() {
  shutdown();
}

bool LibSM64Manager::init(const std::string& rom_path) {
  if (m_initialized) {
    lg::warn("[libsm64] Already initialized");
    return true;
  }

  // Read the SM64 ROM file
  std::ifstream rom_file(rom_path, std::ios::binary | std::ios::ate);
  if (!rom_file.is_open()) {
    lg::error("[libsm64] Failed to open ROM file: {}", rom_path);
    return false;
  }

  auto rom_size = rom_file.tellg();
  rom_file.seekg(0, std::ios::beg);
  std::vector<uint8_t> rom_data(rom_size);
  if (!rom_file.read(reinterpret_cast<char*>(rom_data.data()), rom_size)) {
    lg::error("[libsm64] Failed to read ROM file");
    return false;
  }
  rom_file.close();

  lg::info("[libsm64] ROM loaded: {} bytes", static_cast<size_t>(rom_size));

  // Register debug print callback
  sm64_register_debug_print_function(sm64_debug_print);

  // Allocate texture atlas buffer
  m_texture_data.resize(4 * TEXTURE_WIDTH * TEXTURE_HEIGHT);

  // Initialize the library
  sm64_global_init(rom_data.data(), m_texture_data.data());

  // Pre-allocate tick buffers
  m_tick_position_buf.resize(9 * GEO_MAX_TRIANGLES);
  m_tick_normal_buf.resize(9 * GEO_MAX_TRIANGLES);
  m_tick_color_buf.resize(9 * GEO_MAX_TRIANGLES);
  m_tick_uv_buf.resize(6 * GEO_MAX_TRIANGLES);

  m_initialized = true;
  lg::info("[libsm64] Initialized successfully");
  return true;
}

void LibSM64Manager::shutdown() {
  if (!m_initialized) return;

  if (m_mario_id >= 0) {
    sm64_mario_delete(m_mario_id);
    m_mario_id = -1;
  }

  sm64_global_terminate();
  m_initialized = false;
  lg::info("[libsm64] Shutdown complete");
}

int32_t LibSM64Manager::create_mario(float x, float y, float z) {
  if (!m_initialized) {
    lg::error("[libsm64] Cannot create Mario: not initialized");
    return -1;
  }

  if (m_mario_id >= 0) {
    sm64_mario_delete(m_mario_id);
  }

  // Convert Jak coordinates to SM64 coordinates
  // Jak: Y-up, same as SM64, but different scale
  float sm64_x = x * JAK_TO_SM64_SCALE;
  float sm64_y = y * JAK_TO_SM64_SCALE;
  float sm64_z = z * JAK_TO_SM64_SCALE;

  m_mario_id = sm64_mario_create(sm64_x, sm64_y, sm64_z);
  if (m_mario_id < 0) {
    lg::error("[libsm64] Failed to create Mario");
    return -1;
  }

  lg::info("[libsm64] Mario created at ({}, {}, {}) [SM64: ({}, {}, {})]",
           x, y, z, sm64_x, sm64_y, sm64_z);
  return m_mario_id;
}

void LibSM64Manager::delete_mario(int32_t mario_id) {
  if (m_mario_id == mario_id && m_mario_id >= 0) {
    sm64_mario_delete(m_mario_id);
    m_mario_id = -1;
  }
}

void LibSM64Manager::tick(const MarioInputState& input) {
  if (!m_initialized || m_mario_id < 0) return;

  SM64MarioInputs sm64_input{};
  sm64_input.camLookX = input.cam_look_x;
  sm64_input.camLookZ = input.cam_look_z;
  sm64_input.stickX = input.stick_x;
  sm64_input.stickY = input.stick_y;
  sm64_input.buttonA = input.button_a ? 1 : 0;
  sm64_input.buttonB = input.button_b ? 1 : 0;
  sm64_input.buttonZ = input.button_z ? 1 : 0;

  SM64MarioState sm64_state{};
  SM64MarioGeometryBuffers sm64_geo{};
  sm64_geo.position = m_tick_position_buf.data();
  sm64_geo.normal = m_tick_normal_buf.data();
  sm64_geo.color = m_tick_color_buf.data();
  sm64_geo.uv = m_tick_uv_buf.data();

  sm64_mario_tick(m_mario_id, &sm64_input, &sm64_state, &sm64_geo);

  // Copy results into our managed buffers (threadsafe)
  std::lock_guard<std::mutex> lock(m_geo_mutex);

  m_geometry.num_triangles = sm64_geo.numTrianglesUsed;
  int num_verts = sm64_geo.numTrianglesUsed * 3;

  m_geometry.position.resize(num_verts * 3);
  m_geometry.normal.resize(num_verts * 3);
  m_geometry.color.resize(num_verts * 3);
  m_geometry.uv.resize(num_verts * 2);

  // Copy and scale positions from SM64 to Jak coordinate space
  for (int i = 0; i < num_verts * 3; i++) {
    m_geometry.position[i] = m_tick_position_buf[i] * SM64_TO_JAK_SCALE;
  }
  std::memcpy(m_geometry.normal.data(), m_tick_normal_buf.data(), num_verts * 3 * sizeof(float));
  std::memcpy(m_geometry.color.data(), m_tick_color_buf.data(), num_verts * 3 * sizeof(float));
  std::memcpy(m_geometry.uv.data(), m_tick_uv_buf.data(), num_verts * 2 * sizeof(float));

  // Update state with Jak-scale coordinates
  m_state.position = math::Vector3f(
      sm64_state.position[0] * SM64_TO_JAK_SCALE,
      sm64_state.position[1] * SM64_TO_JAK_SCALE,
      sm64_state.position[2] * SM64_TO_JAK_SCALE);
  m_state.velocity = math::Vector3f(
      sm64_state.velocity[0] * SM64_TO_JAK_SCALE,
      sm64_state.velocity[1] * SM64_TO_JAK_SCALE,
      sm64_state.velocity[2] * SM64_TO_JAK_SCALE);
  m_state.face_angle = sm64_state.faceAngle;
  m_state.forward_velocity = sm64_state.forwardVelocity * SM64_TO_JAK_SCALE;
  m_state.health = sm64_state.health;
  m_state.action = sm64_state.action;
  m_state.flags = sm64_state.flags;
  m_state.anim_id = sm64_state.animID;
  m_state.anim_frame = sm64_state.animFrame;
}

void LibSM64Manager::load_flat_ground(float y_height, float half_extent) {
  if (!m_initialized) return;

  float e = half_extent * JAK_TO_SM64_SCALE;
  int32_t y = static_cast<int32_t>(y_height * JAK_TO_SM64_SCALE);

  // Two triangles forming a flat ground plane
  SM64Surface surfaces[2];
  std::memset(surfaces, 0, sizeof(surfaces));

  // Triangle 1
  surfaces[0].type = 0x0000;  // SURFACE_DEFAULT
  surfaces[0].force = 0;
  surfaces[0].terrain = 0x0001;  // TERRAIN_STONE
  surfaces[0].vertices[0][0] = static_cast<int32_t>(-e);
  surfaces[0].vertices[0][1] = y;
  surfaces[0].vertices[0][2] = static_cast<int32_t>(-e);
  surfaces[0].vertices[1][0] = static_cast<int32_t>(e);
  surfaces[0].vertices[1][1] = y;
  surfaces[0].vertices[1][2] = static_cast<int32_t>(-e);
  surfaces[0].vertices[2][0] = static_cast<int32_t>(-e);
  surfaces[0].vertices[2][1] = y;
  surfaces[0].vertices[2][2] = static_cast<int32_t>(e);

  // Triangle 2
  surfaces[1].type = 0x0000;
  surfaces[1].force = 0;
  surfaces[1].terrain = 0x0001;
  surfaces[1].vertices[0][0] = static_cast<int32_t>(e);
  surfaces[1].vertices[0][1] = y;
  surfaces[1].vertices[0][2] = static_cast<int32_t>(e);
  surfaces[1].vertices[1][0] = static_cast<int32_t>(-e);
  surfaces[1].vertices[1][1] = y;
  surfaces[1].vertices[1][2] = static_cast<int32_t>(e);
  surfaces[1].vertices[2][0] = static_cast<int32_t>(e);
  surfaces[1].vertices[2][1] = y;
  surfaces[1].vertices[2][2] = static_cast<int32_t>(-e);

  sm64_static_surfaces_load(surfaces, 2);
  lg::info("[libsm64] Loaded flat ground at y={} extent={}", y_height, half_extent);
}

void LibSM64Manager::load_surfaces(const std::vector<SM64Surface>& surfaces) {
  if (!m_initialized || surfaces.empty()) return;
  sm64_static_surfaces_load(surfaces.data(), static_cast<uint32_t>(surfaces.size()));
  m_loaded_surface_count = static_cast<int>(surfaces.size());
  lg::info("[libsm64] Loaded {} collision surfaces", surfaces.size());
}

void LibSM64Manager::load_level_collision(
    const std::vector<tfrag3::CollisionMesh::Vertex>& vertices) {
  if (!m_initialized) return;
  if (vertices.size() < 3) {
    lg::warn("[libsm64] No collision vertices to load");
    return;
  }

  size_t num_tris = vertices.size() / 3;
  std::vector<SM64Surface> surfaces(num_tris);

  for (size_t i = 0; i < num_tris; i++) {
    auto& surf = surfaces[i];
    surf.type = 0x0000;    // SURFACE_DEFAULT
    surf.force = 0;
    surf.terrain = 0x0001;  // TERRAIN_STONE

    for (int v = 0; v < 3; v++) {
      const auto& vert = vertices[i * 3 + v];
      // Jak positions are in meters, SM64 expects its own units (43x scale)
      surf.vertices[v][0] = static_cast<int32_t>(vert.x * JAK_TO_SM64_SCALE);
      surf.vertices[v][1] = static_cast<int32_t>(vert.y * JAK_TO_SM64_SCALE);
      surf.vertices[v][2] = static_cast<int32_t>(vert.z * JAK_TO_SM64_SCALE);
    }
  }

  sm64_static_surfaces_load(surfaces.data(), static_cast<uint32_t>(surfaces.size()));
  m_loaded_surface_count = static_cast<int>(surfaces.size());
  lg::info("[libsm64] Loaded {} collision surfaces from level geometry", surfaces.size());
}

bool LibSM64Manager::write_mario_pos_to_target(u8* ee_mem,
                                                u32 ee_mem_size,
                                                u32 false_val,
                                                u32 target_ptr,
                                                const math::Vector3f& mario_pos) {
  if (!ee_mem || target_ptr == 0 || target_ptr == false_val) return false;

  // process-drawable.root is declared at GOAL :offset 112, but for boxed (basic) types
  // the runtime offset is (declared - 4) — see goalc/compiler/compilation/Type.cpp:1632.
  // So root lives at target_ptr + 108. Reading 4 bytes there → need target_ptr + 112 in bounds.
  constexpr u32 ROOT_RUNTIME_OFF = 108;
  if (target_ptr + ROOT_RUNTIME_OFF + 4 > ee_mem_size) {
    lg::warn("[libsm64] write: target_ptr 0x{:X} + {} > mem_size 0x{:X}",
             target_ptr, ROOT_RUNTIME_OFF + 4, ee_mem_size);
    return false;
  }

  u32 root_ptr;
  std::memcpy(&root_ptr, ee_mem + target_ptr + ROOT_RUNTIME_OFF, 4);
  if (root_ptr == 0 || root_ptr == false_val) {
    lg::warn("[libsm64] write: root_ptr 0x{:X} is null or #f", root_ptr);
    return false;
  }

  // trs.trans is declared at GOAL :offset 16 → runtime offset 12 (boxed adjustment).
  // The vector is 16 bytes (x,y,z,w floats).
  constexpr u32 TRANS_RUNTIME_OFF = 12;
  if (root_ptr + TRANS_RUNTIME_OFF + 16 > ee_mem_size) {
    lg::warn("[libsm64] write: root_ptr 0x{:X} + {} > mem_size 0x{:X}",
             root_ptr, TRANS_RUNTIME_OFF + 16, ee_mem_size);
    return false;
  }

  float trans[4];
  trans[0] = mario_pos.x();
  trans[1] = mario_pos.y();
  trans[2] = mario_pos.z();
  trans[3] = 1.0f;

  // Log the first few writes for debugging
  static int write_count = 0;
  if (write_count < 3) {
    float existing[4];
    std::memcpy(existing, ee_mem + root_ptr + TRANS_RUNTIME_OFF, 16);
    lg::info("[libsm64] write #{}: root=0x{:X}, existing=({}, {}, {}, {}), new=({}, {}, {})",
             write_count, root_ptr,
             existing[0], existing[1], existing[2], existing[3],
             trans[0], trans[1], trans[2]);
    write_count++;
  }

  std::memcpy(ee_mem + root_ptr + TRANS_RUNTIME_OFF, trans, 16);
  return true;
}

void LibSM64Manager::resolve_target_symbol() {
  // Resolves and caches the *target* symbol offset.
  // Safe to call from any thread — intern_from_c only reads for existing symbols.
  if (m_cached_target_sym_offset != 0) return;

  u32 false_val = s7.offset;
  if (false_val == 0) {
    lg::warn("[libsm64] resolve_target_symbol: s7 not set yet");
    return;
  }

  auto target_sym = jak1::intern_from_c("*target*");
  if (target_sym.offset != 0) {
    m_cached_target_sym_offset = target_sym.offset;
    lg::info("[libsm64] Cached *target* symbol at offset 0x{:X}", m_cached_target_sym_offset);
  } else {
    lg::warn("[libsm64] resolve_target_symbol: could not find *target*");
  }
}

void LibSM64Manager::sync_jak_to_mario(u8* ee_mem, u32 s7_offset) {
  if (!m_initialized || m_mario_id < 0 || !ee_mem) return;

  u32 false_val = s7.offset;
  if (false_val == 0) return;

  // Look up *target* directly via kernel (called every tick, but only a read)
  auto target_sym = jak1::intern_from_c("*target*");
  if (target_sym.offset == 0) return;
  u32 target_ptr = target_sym->value;
  if (target_ptr == 0 || target_ptr == false_val) return;

  auto mario_pos = get_state().position;
  write_mario_pos_to_target(ee_mem, EE_MAIN_MEM_SIZE, false_val, target_ptr, mario_pos);
}

MarioGeometry LibSM64Manager::get_geometry() {
  std::lock_guard<std::mutex> lock(m_geo_mutex);
  return m_geometry;
}

MarioState LibSM64Manager::get_state() {
  std::lock_guard<std::mutex> lock(m_geo_mutex);
  return m_state;
}

}  // namespace sm64
