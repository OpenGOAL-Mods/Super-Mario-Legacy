/*!
 * @file libsm64_integration.cpp
 * Implementation of the libsm64 integration manager.
 */

#include "libsm64_integration.h"

#include <array>
#include <cmath>
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

  // Drop any tracked actor collision objects before terminating libsm64.
  clear_actor_collision();
  m_type_cache = {};
  m_is_process_drawable_cache.clear();
  m_is_collide_shape_cache.clear();

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

// ========================================================================
// Dynamic actor collision
// ========================================================================
//
// Every "tick" we walk the Jak process tree rooted at *active-pool*, find
// process-drawables whose root is a collide-shape (or subclass) that owns a
// collide-mesh (directly via collide-shape-prim-mesh, or via a nested
// collide-shape-prim-group). For each such mesh we bake its local vertices
// into world space using the actor's current trans+quat and register it with
// libsm64 via sm64_surface_object_create. Subsequent frames re-bake on motion
// and either move the existing object (same signature: skip) or delete+recreate.
//
// GOAL runtime offsets — remember boxed (basic) types subtract 4 from the
// declared offset, see goalc/compiler/compilation/Type.cpp:1632.
//
//   process-drawable.root                :offset 112  -> runtime 108
//   collide-shape.trans (from trs)       :offset 16   -> runtime 12
//   collide-shape.quat  (from trsq)      :offset 32   -> runtime 28
//   collide-shape.root-prim              :offset 160  -> runtime 156
//   collide-shape-prim.prim-core         :offset 16   -> runtime 12 (inline)
//   collide-shape-prim-mesh.mesh         :offset 72   -> runtime 68
//   collide-shape-prim-group.num-prims   :offset 72   -> runtime 68
//   collide-shape-prim-group.prim[0]     :offset 80   -> runtime 76
//   collide-mesh.num-tris                :offset 8    -> runtime 4
//   collide-mesh.num-verts               :offset 12   -> runtime 8
//   collide-mesh.vertex-data             :offset 16   -> runtime 12 (pointer to inline array)
//   collide-mesh.tris (inline array)     :offset 32   -> runtime 28 (in-place, 8 bytes each)

namespace ac {  // "actor collision" — isolated from the rest of the file

constexpr u32 PDRAW_ROOT_OFF = 108;
constexpr u32 CSHAPE_TRANS_OFF = 12;
constexpr u32 CSHAPE_QUAT_OFF = 28;
constexpr u32 CSHAPE_ROOT_PRIM_OFF = 156;
constexpr u32 PRIM_MESH_MESH_OFF = 68;
constexpr u32 PRIM_GROUP_NUM_PRIMS_OFF = 68;
constexpr u32 PRIM_GROUP_PRIM_ARRAY_OFF = 76;
constexpr u32 MESH_NUM_TRIS_OFF = 4;
constexpr u32 MESH_NUM_VERTS_OFF = 8;
constexpr u32 MESH_VERTEX_DATA_OFF = 12;
constexpr u32 MESH_TRIS_OFF = 28;
constexpr u32 MESH_TRI_SIZE = 8;  // collide-mesh-tri: 3 u8 indices + 1 u8 pad + 1 u32 pat

// Process-tree layout (unboxed-adjusted runtime offsets).
// process-tree.child is at GOAL :offset 20 -> runtime 16
// process-tree.brother is at GOAL :offset 16 -> runtime 12
constexpr u32 PTREE_BROTHER_OFF = 12;
constexpr u32 PTREE_CHILD_OFF = 16;

// Limits, tunable
constexpr int MAX_PROCESS_TREE_NODES = 4096;     // safety cap on DFS
constexpr int MAX_ACTOR_SURFACE_OBJECTS = 32;    // cap new objects per frame
constexpr u32 MAX_VERTS_PER_MESH = 256;          // collide-mesh uses u8 indices
constexpr u32 MAX_TRIS_PER_MESH = 512;
constexpr int MAX_PRIM_DEPTH = 8;
constexpr u32 MAX_PRIMS_IN_GROUP = 64;
constexpr int MAX_TYPE_CHAIN_DEPTH = 32;

// Is `addr` a plausible EE-memory pointer that can hold `size` bytes?
// Rejects null/tiny/unaligned-ish pointers and anything past the end.
inline bool valid_ee_addr(u32 addr, u32 size, u32 mem_size) {
  if (addr < 16) return false;                        // no real GOAL object lives this low
  if (size == 0) return false;
  if (addr > mem_size) return false;
  if (size > mem_size) return false;
  if (addr + size > mem_size) return false;            // also catches a+s overflow given a<mem_size
  return true;
}

// Stricter check for a *basic pointer*: must satisfy alignment that real
// jak1 basics use (low 3 bits == 4, since basics are 8-byte aligned at the
// type tag and basic_ptr = type_tag + 4), and must live above the kernel
// scratch / symbol-table region. Walking garbage interpreted as basic ptrs
// was crashing the dynamic-actor-collision walker — see crash log analysis
// in 14:42 trace where node 0x4BD3CC's `brother` field decoded as 0x36837
// (unaligned + below the heap floor) and walking it died.
//
// MIN_HEAP_ADDR is intentionally generous: kernel/symbol-table state lives
// well below 1 MB in jak1, so anything below this is definitely not a real
// heap-allocated basic.
inline bool valid_basic_ptr(u32 addr, u32 mem_size) {
  constexpr u32 MIN_HEAP_ADDR = 0x100000;  // 1 MB
  if (addr < MIN_HEAP_ADDR) return false;
  if ((addr & 0x7u) != 4u) return false;
  // Need at least the type tag at addr-4 plus a small payload.
  return valid_ee_addr(addr, 32, mem_size);
}

// A u32 read that bails on out-of-bounds.
inline bool read_u32(u8* mem, u32 addr, u32 mem_size, u32& out) {
  if (!valid_ee_addr(addr, 4, mem_size)) return false;
  std::memcpy(&out, mem + addr, 4);
  return true;
}

inline bool read_vec4(u8* mem, u32 addr, u32 mem_size, float out[4]) {
  if (!valid_ee_addr(addr, 16, mem_size)) return false;
  std::memcpy(out, mem + addr, 16);
  return true;
}

// Read the type pointer for a basic object (stored at basic_ptr - 4).
inline bool read_basic_type(u8* mem, u32 basic_ptr, u32 mem_size, u32& type_out) {
  if (basic_ptr < 16) return false;                    // same floor as valid_ee_addr
  return read_u32(mem, basic_ptr - 4, mem_size, type_out);
}

// Build a rotation matrix from a unit quaternion (x, y, z, w).
// Result is row-major: out = M * v. Translation is added separately.
inline void quat_to_rot_mat3(const float q[4], float m[9]) {
  float x = q[0], y = q[1], z = q[2], w = q[3];
  float xx = x * x, yy = y * y, zz = z * z;
  float xy = x * y, xz = x * z, yz = y * z;
  float wx = w * x, wy = w * y, wz = w * z;
  m[0] = 1.0f - 2.0f * (yy + zz);  m[1] = 2.0f * (xy - wz);        m[2] = 2.0f * (xz + wy);
  m[3] = 2.0f * (xy + wz);         m[4] = 1.0f - 2.0f * (xx + zz); m[5] = 2.0f * (yz - wx);
  m[6] = 2.0f * (xz - wy);         m[7] = 2.0f * (yz + wx);        m[8] = 1.0f - 2.0f * (xx + yy);
}

inline void transform_point(const float rot[9], const float trans[3], const float local[3],
                             float out[3]) {
  out[0] = rot[0] * local[0] + rot[1] * local[1] + rot[2] * local[2] + trans[0];
  out[1] = rot[3] * local[0] + rot[4] * local[1] + rot[5] * local[2] + trans[1];
  out[2] = rot[6] * local[0] + rot[7] * local[1] + rot[8] * local[2] + trans[2];
}

// Has this actor moved enough to re-bake collision?
// trans delta > 1 Jak unit OR quaternion dot < ~0.9999 (i.e. > ~1° rotation).
inline bool transform_changed(const float a_trans[3], const float a_quat[4],
                               const float b_trans[3], const float b_quat[4]) {
  float dx = a_trans[0] - b_trans[0];
  float dy = a_trans[1] - b_trans[1];
  float dz = a_trans[2] - b_trans[2];
  if (dx * dx + dy * dy + dz * dz > 1.0f) return true;
  float dot = a_quat[0] * b_quat[0] + a_quat[1] * b_quat[1] + a_quat[2] * b_quat[2] +
              a_quat[3] * b_quat[3];
  return std::abs(dot) < 0.9999f;
}

// Walk the type-parent chain of a basic type, looking for `needle`.
// Memoizes the result keyed by the starting type pointer.
bool type_is_descendant(u8* ee_mem, u32 mem_size, u32 type_ptr, u32 needle,
                        std::unordered_map<u32, bool>& cache) {
  if (type_ptr == 0 || needle == 0) return false;
  auto it = cache.find(type_ptr);
  if (it != cache.end()) return it->second;

  u32 cur = type_ptr;
  for (int i = 0; i < MAX_TYPE_CHAIN_DEPTH; i++) {
    if (cur == 0) break;
    if (cur == needle) {
      cache[type_ptr] = true;
      return true;
    }
    // Type struct (basic): symbol @4->0, parent @8->4 after BASIC_OFFSET.
    u32 parent;
    if (!read_u32(ee_mem, cur + 4, mem_size, parent)) break;
    if (parent == cur) break;  // self-loop (object's parent)
    cur = parent;
  }
  cache[type_ptr] = false;
  return false;
}

// Extract a single collide-mesh's local-space triangles as SM64Surfaces,
// transformed by (rot, trans_jak) into world-space SM64 units.
// Returns false on any read/sanity failure; writes triangles into out_surfaces
// and increments out_triangles_considered for each triangle the walker looked at
// (regardless of whether it was emitted).
bool extract_mesh_world(u8* ee_mem, u32 mem_size, u32 mesh_ptr, const float rot[9],
                        const float trans_jak[3], std::vector<SM64Surface>& out_surfaces,
                        int* out_triangles_considered = nullptr) {
  u32 num_tris = 0, num_verts = 0, vertex_data_ptr = 0;
  if (!read_u32(ee_mem, mesh_ptr + MESH_NUM_TRIS_OFF, mem_size, num_tris)) return false;
  if (!read_u32(ee_mem, mesh_ptr + MESH_NUM_VERTS_OFF, mem_size, num_verts)) return false;
  if (!read_u32(ee_mem, mesh_ptr + MESH_VERTEX_DATA_OFF, mem_size, vertex_data_ptr)) return false;

  if (num_tris == 0 || num_tris > MAX_TRIS_PER_MESH) return false;
  if (num_verts == 0 || num_verts > MAX_VERTS_PER_MESH) return false;

  // Range-check the vertex buffer and the tri buffer up-front.
  if (!valid_ee_addr(vertex_data_ptr, num_verts * 16u, mem_size)) return false;
  if (!valid_ee_addr(mesh_ptr + MESH_TRIS_OFF, num_tris * MESH_TRI_SIZE, mem_size)) return false;

  // Pre-read & transform all vertices into world-space SM64 units.
  std::vector<std::array<int32_t, 3>> world_verts(num_verts);
  for (u32 i = 0; i < num_verts; i++) {
    float local_v[4];
    if (!read_vec4(ee_mem, vertex_data_ptr + i * 16u, mem_size, local_v)) return false;
    float world[3];
    transform_point(rot, trans_jak, local_v, world);
    world_verts[i][0] = static_cast<int32_t>(world[0] * JAK_TO_SM64_SCALE);
    world_verts[i][1] = static_cast<int32_t>(world[1] * JAK_TO_SM64_SCALE);
    world_verts[i][2] = static_cast<int32_t>(world[2] * JAK_TO_SM64_SCALE);
  }

  out_surfaces.reserve(out_surfaces.size() + num_tris);
  for (u32 i = 0; i < num_tris; i++) {
    if (out_triangles_considered) (*out_triangles_considered)++;
    u32 tri_addr = mesh_ptr + MESH_TRIS_OFF + i * MESH_TRI_SIZE;
    u8 i0 = ee_mem[tri_addr + 0];
    u8 i1 = ee_mem[tri_addr + 1];
    u8 i2 = ee_mem[tri_addr + 2];
    if (i0 >= num_verts || i1 >= num_verts || i2 >= num_verts) continue;

    SM64Surface s{};
    s.type = 0;      // SURFACE_DEFAULT
    s.force = 0;
    s.terrain = 1;   // TERRAIN_STONE
    const u8 indices[3] = {i0, i1, i2};
    for (int v = 0; v < 3; v++) {
      s.vertices[v][0] = world_verts[indices[v]][0];
      s.vertices[v][1] = world_verts[indices[v]][1];
      s.vertices[v][2] = world_verts[indices[v]][2];
    }
    out_surfaces.push_back(s);
  }
  return true;
}

// Recursively collect mesh pointers from a collide-shape-prim hierarchy.
void collect_mesh_prims(u8* ee_mem, u32 mem_size, u32 prim_ptr, u32 false_val,
                        u32 prim_mesh_type, u32 prim_group_type,
                        std::vector<u32>& out_meshes, int depth = 0) {
  if (prim_ptr == 0 || prim_ptr == false_val || depth > MAX_PRIM_DEPTH) return;
  u32 prim_type;
  if (!read_basic_type(ee_mem, prim_ptr, mem_size, prim_type)) return;

  if (prim_type == prim_mesh_type) {
    u32 mesh_ptr;
    if (!read_u32(ee_mem, prim_ptr + PRIM_MESH_MESH_OFF, mem_size, mesh_ptr)) return;
    if (mesh_ptr != 0 && mesh_ptr != false_val) {
      out_meshes.push_back(mesh_ptr);
    }
  } else if (prim_type == prim_group_type) {
    u32 num_prims;
    if (!read_u32(ee_mem, prim_ptr + PRIM_GROUP_NUM_PRIMS_OFF, mem_size, num_prims)) return;
    if (num_prims == 0 || num_prims > MAX_PRIMS_IN_GROUP) return;
    for (u32 i = 0; i < num_prims; i++) {
      u32 child;
      if (!read_u32(ee_mem, prim_ptr + PRIM_GROUP_PRIM_ARRAY_OFF + i * 4, mem_size, child)) break;
      if (child == 0 || child == false_val) continue;
      collect_mesh_prims(ee_mem, mem_size, child, false_val, prim_mesh_type, prim_group_type,
                         out_meshes, depth + 1);
    }
  }
  // spheres and other prim types are ignored.
}

}  // namespace ac

void LibSM64Manager::clear_actor_collision() {
  if (m_tracked_actors.empty()) {
    m_broken_meshes.clear();
    return;
  }
  for (auto& [mesh_addr, tracked] : m_tracked_actors) {
    if (tracked.has_obj) {
      sm64_surface_object_delete(tracked.sm64_obj_id);
    }
  }
  m_tracked_actors.clear();
  m_broken_meshes.clear();
  lg::info("[libsm64] Cleared all actor surface objects");
}

// --------------------------------------------------------------------------
// The real per-frame entry point and a test-visible variant.
// --------------------------------------------------------------------------
//
// Both share the same walker. The "real" version pulls parameters from the
// live kernel via find_symbol_from_c; the test variant takes everything as
// explicit arguments so unit tests can drive it against a synthetic buffer.

namespace {

// Internal walker state passed around by the sweep routine.
struct WalkCtx {
  u8* ee_mem;
  u32 mem_size;
  u32 false_val;
  u32 active_pool_sym;
  u32 process_drawable_type;
  u32 collide_shape_type;
  u32 prim_mesh_type;
  u32 prim_group_type;
  bool dry_run;
  int& diag_logs_remaining;
  std::unordered_map<u32, bool>& is_process_drawable_cache;
  std::unordered_map<u32, bool>& is_collide_shape_cache;
  std::unordered_map<uint64_t, LibSM64Manager::TrackedActor>& tracked_actors;
  std::unordered_set<u32>& broken_meshes;
  LibSM64Manager::TestSweepResult& result;
};

// Compose a tracking key from (process-drawable address, mesh address). Two
// different actor instances can share the same collide-mesh template, so we
// can't key by mesh alone.
inline uint64_t make_actor_key(u32 pd_node, u32 mesh_ptr) {
  return (static_cast<uint64_t>(pd_node) << 32) | static_cast<uint64_t>(mesh_ptr);
}

// Perform one actor-collision sweep. Returns normally on any expected failure
// (unreadable memory, missing types, etc.) — the walker never throws or crashes.
void do_sweep(WalkCtx& c) {
  using namespace ac;

  if (c.ee_mem == nullptr) return;
  if (c.mem_size < 1024) return;
  if (c.false_val == 0) return;
  if (c.active_pool_sym == 0 || c.process_drawable_type == 0 || c.collide_shape_type == 0 ||
      c.prim_mesh_type == 0 || c.prim_group_type == 0) {
    return;
  }

  // Read the *active-pool* symbol value (= pointer to the active pool process-tree).
  u32 root_process;
  if (!read_u32(c.ee_mem, c.active_pool_sym, c.mem_size, root_process)) return;
  if (root_process == 0 || root_process == c.false_val) return;

  // process-tree.child / .brother are (pointer process-tree) — ppointers, not
  // direct pointers. The kernel sets each to the address of some other
  // process's `self` slot so it can rewrite the slot if the process moves.
  // To get the actual process-tree basic ptr we deref once. See
  // gkernel.gc / ppointer->process.
  auto deref_ppointer = [&c](u32 pp, u32& out_node) -> bool {
    if (pp == 0 || pp == c.false_val) return false;
    if (!valid_ee_addr(pp, 4, c.mem_size)) return false;
    u32 actual = 0;
    if (!read_u32(c.ee_mem, pp, c.mem_size, actual)) return false;
    if (actual == 0 || actual == c.false_val) return false;
    if (!valid_basic_ptr(actual, c.mem_size)) return false;
    out_node = actual;
    return true;
  };

  std::vector<u32> stack;
  stack.reserve(256);
  {
    u32 child_pp;
    if (read_u32(c.ee_mem, root_process + PTREE_CHILD_OFF, c.mem_size, child_pp)) {
      u32 first_child = 0;
      if (deref_ppointer(child_pp, first_child)) {
        stack.push_back(first_child);
      }
    }
  }

  // Reset per-frame seen flags on previously tracked actors.
  for (auto& [k, v] : c.tracked_actors) v.seen_this_frame = false;

  std::unordered_set<u32> visited_set;  // cycle guard: never re-enqueue a node
  visited_set.reserve(512);

  int created_this_frame = 0;

  while (!stack.empty() && c.result.process_tree_nodes_visited < MAX_PROCESS_TREE_NODES) {
    u32 node = stack.back();
    stack.pop_back();

    if (node == 0 || node == c.false_val) continue;
    if (!visited_set.insert(node).second) continue;   // already walked
    c.result.process_tree_nodes_visited++;

    // The popped node itself must look like a basic pointer.
    if (!valid_basic_ptr(node, c.mem_size)) continue;

    // Read brother/child ppointers and deref to actual process-tree basic ptrs.
    u32 brother_pp = 0, child_pp = 0;
    read_u32(c.ee_mem, node + PTREE_BROTHER_OFF, c.mem_size, brother_pp);
    read_u32(c.ee_mem, node + PTREE_CHILD_OFF, c.mem_size, child_pp);
    u32 brother = 0, child = 0;
    if (deref_ppointer(brother_pp, brother) &&
        visited_set.find(brother) == visited_set.end()) {
      stack.push_back(brother);
    }
    if (deref_ppointer(child_pp, child) &&
        visited_set.find(child) == visited_set.end()) {
      stack.push_back(child);
    }

    // Only process-drawable nodes have a useful root; type-check via parent chain.
    u32 node_type;
    if (!read_basic_type(c.ee_mem, node, c.mem_size, node_type)) continue;
    // Self-referential type tag (node_type == node) is garbage; same for
    // unaligned / too-low type ptrs.
    if (node_type == node || !valid_basic_ptr(node_type, c.mem_size)) continue;
    if (!type_is_descendant(c.ee_mem, c.mem_size, node_type, c.process_drawable_type,
                             c.is_process_drawable_cache)) {
      continue;
    }
    c.result.process_drawables_seen++;

    // Read root (collide-shape or subclass) from process-drawable @108.
    u32 root;
    if (!read_u32(c.ee_mem, node + PDRAW_ROOT_OFF, c.mem_size, root)) continue;
    if (root == 0 || root == c.false_val) continue;
    if (!valid_basic_ptr(root, c.mem_size)) continue;

    u32 root_type;
    if (!read_basic_type(c.ee_mem, root, c.mem_size, root_type)) continue;
    if (!valid_basic_ptr(root_type, c.mem_size)) continue;
    if (!type_is_descendant(c.ee_mem, c.mem_size, root_type, c.collide_shape_type,
                             c.is_collide_shape_cache)) {
      continue;
    }

    // Read root-prim and walk the prim tree to find any meshes.
    u32 root_prim;
    if (!read_u32(c.ee_mem, root + CSHAPE_ROOT_PRIM_OFF, c.mem_size, root_prim)) continue;
    if (root_prim == 0 || root_prim == c.false_val) continue;

    std::vector<u32> mesh_ptrs;
    collect_mesh_prims(c.ee_mem, c.mem_size, root_prim, c.false_val, c.prim_mesh_type,
                       c.prim_group_type, mesh_ptrs);
    if (mesh_ptrs.empty()) continue;

    // Read the actor's world transform.
    float trans[4], quat[4];
    if (!read_vec4(c.ee_mem, root + CSHAPE_TRANS_OFF, c.mem_size, trans)) continue;
    if (!read_vec4(c.ee_mem, root + CSHAPE_QUAT_OFF, c.mem_size, quat)) continue;

    // Sanity: finite trans, unit-ish quaternion. Replace garbage rotations with identity.
    if (!std::isfinite(trans[0]) || !std::isfinite(trans[1]) || !std::isfinite(trans[2])) {
      continue;
    }
    float qlen2 = quat[0] * quat[0] + quat[1] * quat[1] + quat[2] * quat[2] + quat[3] * quat[3];
    if (!std::isfinite(qlen2) || qlen2 < 0.5f || qlen2 > 1.5f) {
      quat[0] = quat[1] = quat[2] = 0.0f;
      quat[3] = 1.0f;
    }

    float rot[9];
    quat_to_rot_mat3(quat, rot);

    for (u32 mesh_ptr : mesh_ptrs) {
      // Skip meshes we've already decided are broken.
      if (c.broken_meshes.count(mesh_ptr)) continue;

      // Key by (process-drawable, mesh) so two actor instances that share the
      // same collide-mesh template each get their own libsm64 surface object.
      uint64_t key = make_actor_key(node, mesh_ptr);
      auto& tracked = c.tracked_actors[key];
      tracked.seen_this_frame = true;

      // Fast-path: already have an object and transform hasn't changed.
      if (tracked.has_obj && !transform_changed(trans, quat, tracked.last_trans, tracked.last_quat)) {
        continue;
      }

      if (created_this_frame >= MAX_ACTOR_SURFACE_OBJECTS) break;

      std::vector<SM64Surface> surfaces;
      if (!extract_mesh_world(c.ee_mem, c.mem_size, mesh_ptr, rot, trans, surfaces, nullptr)) {
        c.broken_meshes.insert(mesh_ptr);
        c.result.errors++;
        continue;
      }
      if (surfaces.empty()) {
        c.broken_meshes.insert(mesh_ptr);
        continue;
      }

      c.result.meshes_found++;
      c.result.triangles_extracted += (int)surfaces.size();

      if (!c.dry_run) {
        // If we already had an object for this (actor, mesh) pair, destroy it
        // (world-space vertices changed so a pure move() isn't enough).
        if (tracked.has_obj) {
          sm64_surface_object_delete(tracked.sm64_obj_id);
          tracked.sm64_obj_id = 0;
          tracked.has_obj = false;
        }

        SM64SurfaceObject obj{};
        obj.transform.position[0] = 0.0f;
        obj.transform.position[1] = 0.0f;
        obj.transform.position[2] = 0.0f;
        obj.transform.eulerRotation[0] = 0.0f;
        obj.transform.eulerRotation[1] = 0.0f;
        obj.transform.eulerRotation[2] = 0.0f;
        obj.surfaceCount = static_cast<uint32_t>(surfaces.size());
        obj.surfaces = surfaces.data();
        tracked.sm64_obj_id = sm64_surface_object_create(&obj);
        tracked.has_obj = true;
      }
      std::memcpy(tracked.last_trans, trans, 12);
      std::memcpy(tracked.last_quat, quat, 16);
      created_this_frame++;
    }
  }

  // Reap actors that disappeared this frame.
  for (auto it = c.tracked_actors.begin(); it != c.tracked_actors.end();) {
    if (!it->second.seen_this_frame) {
      if (it->second.has_obj && !c.dry_run) {
        sm64_surface_object_delete(it->second.sm64_obj_id);
      }
      it = c.tracked_actors.erase(it);
    } else {
      ++it;
    }
  }
}

}  // namespace

void LibSM64Manager::update_actor_collision(u8* ee_mem) {
  if (!m_initialized || !dynamic_actor_collision || !ee_mem) return;

  u32 false_val = s7.offset;
  if (false_val == 0) return;

  // Lazily populate the type cache via find_symbol_from_c (read-only — does NOT
  // allocate new symbol slots). Any missing symbol → silently bail; we'll retry
  // next frame.
  if (!m_type_cache.ready) {
    auto pd = jak1::find_symbol_from_c("process-drawable");
    auto cs = jak1::find_symbol_from_c("collide-shape");
    auto pm = jak1::find_symbol_from_c("collide-shape-prim-mesh");
    auto pg = jak1::find_symbol_from_c("collide-shape-prim-group");
    auto ap = jak1::find_symbol_from_c("*active-pool*");
    if (pd.offset == 0 || cs.offset == 0 || pm.offset == 0 || pg.offset == 0 ||
        ap.offset == 0) {
      return;  // symbol table doesn't have one of our keys yet; retry next frame
    }
    u32 pd_val = pd->value;
    u32 cs_val = cs->value;
    u32 pm_val = pm->value;
    u32 pg_val = pg->value;
    if (pd_val == 0 || pd_val == false_val || cs_val == 0 || cs_val == false_val ||
        pm_val == 0 || pm_val == false_val || pg_val == 0 || pg_val == false_val) {
      return;  // types symbols exist but haven't been bound to Type structs yet
    }
    m_type_cache.process_drawable = pd_val;
    m_type_cache.collide_shape = cs_val;
    m_type_cache.prim_mesh = pm_val;
    m_type_cache.prim_group = pg_val;
    m_type_cache.active_pool_sym = ap.offset;
    m_type_cache.ready = true;
    lg::info("[libsm64] Actor collision type cache ready: pd=0x{:X} cs=0x{:X} pm=0x{:X} pg=0x{:X} ap_sym=0x{:X} false=0x{:X}",
             pd_val, cs_val, pm_val, pg_val, ap.offset, false_val);
  }

  TestSweepResult result;
  WalkCtx ctx{
      ee_mem,
      EE_MAIN_MEM_SIZE,
      false_val,
      m_type_cache.active_pool_sym,
      m_type_cache.process_drawable,
      m_type_cache.collide_shape,
      m_type_cache.prim_mesh,
      m_type_cache.prim_group,
      dynamic_actor_collision_dry_run,
      m_actor_diag_logs_remaining,
      m_is_process_drawable_cache,
      m_is_collide_shape_cache,
      m_tracked_actors,
      m_broken_meshes,
      result,
  };
  do_sweep(ctx);

  m_actor_sync_frame++;
  if (m_actor_sync_frame == 1 || m_actor_sync_frame == 60 || m_actor_sync_frame == 600) {
    lg::info(
        "[libsm64] actor-collision frame {}: visited {} nodes, pd_seen {}, meshes {}, tris {}, errors {}, tracking {}",
        m_actor_sync_frame, result.process_tree_nodes_visited, result.process_drawables_seen,
        result.meshes_found, result.triangles_extracted, result.errors, m_tracked_actors.size());
  }
}

LibSM64Manager::TestSweepResult LibSM64Manager::test_sweep(u8* ee_mem,
                                                           u32 ee_mem_size,
                                                           u32 false_val,
                                                           u32 active_pool_sym,
                                                           u32 process_drawable_type,
                                                           u32 collide_shape_type,
                                                           u32 prim_mesh_type,
                                                           u32 prim_group_type,
                                                           bool dry_run) {
  TestSweepResult result;
  int dummy_diag_remaining = 0;  // tests shouldn't spam lg::info
  std::unordered_map<u32, bool> pd_cache, cs_cache;
  std::unordered_map<uint64_t, TrackedActor> tracked;
  std::unordered_set<u32> broken;

  WalkCtx ctx{
      ee_mem,
      ee_mem_size,
      false_val,
      active_pool_sym,
      process_drawable_type,
      collide_shape_type,
      prim_mesh_type,
      prim_group_type,
      dry_run,
      dummy_diag_remaining,
      pd_cache,
      cs_cache,
      tracked,
      broken,
      result,
  };
  do_sweep(ctx);
  return result;
}

}  // namespace sm64
