/*!
 * @file libsm64_integration.cpp
 * Implementation of the libsm64 integration manager.
 */

#include "libsm64_integration.h"

#include "sm64_audio.h"

#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>

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

[[maybe_unused]] static void sm64_debug_print(const char* msg) {
  // libsm64's audio engine (audio/load.c, audio/external.c) fires DEBUG_PRINT
  // from the cubeb worker thread on every audio tick. Routing any of that
  // through lg stalls the audio thread on stdio and torches the main-thread
  // FPS. We keep this stub around for manual re-registration when debugging
  // libsm64 internals, but don't wire it up by default.
  lg::debug("[libsm64] {}", msg);
}

LibSM64Manager& LibSM64Manager::instance() {
  static LibSM64Manager mgr;
  return mgr;
}

LibSM64Manager::~LibSM64Manager() {
  shutdown();
}

// SM64 US ROM is exactly 8 MiB. libsm64 expects the US revision; we use the
// size as a cheap selector when the user drops any .z64 into the search path.
static constexpr std::uintmax_t kExpectedSm64RomSize = 8u * 1024u * 1024u;

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

  // NOTE: intentionally NOT registering sm64_debug_print — libsm64's audio
  // engine spams DEBUG_PRINT on every audio tick from the cubeb worker
  // thread, which stalls on the log and destroys FPS. Re-enable manually if
  // you need to debug libsm64 internals.

  // Allocate texture atlas buffer
  m_texture_data.resize(4 * TEXTURE_WIDTH * TEXTURE_HEIGHT);

  // Initialize the library
  sm64_global_init(rom_data.data(), m_texture_data.data());

  // Boot the N64 audio engine from the same ROM, then kick off the cubeb
  // worker thread so audio starts playing immediately (title-screen jingle,
  // sfx, etc.). Failures are non-fatal — we just run silent.
  sm64_audio_init(rom_data.data());
  m_audio = std::make_unique<SM64AudioPlayer>(m_sm64_lock);
  m_audio->set_volume(m_audio_volume);
  if (!m_audio->start()) {
    lg::warn("[libsm64] Audio stream failed to start; continuing without audio");
  }

  // Pre-allocate tick buffers
  m_tick_position_buf.resize(9 * GEO_MAX_TRIANGLES);
  m_tick_normal_buf.resize(9 * GEO_MAX_TRIANGLES);
  m_tick_color_buf.resize(9 * GEO_MAX_TRIANGLES);
  m_tick_uv_buf.resize(6 * GEO_MAX_TRIANGLES);

  m_initialized = true;
  m_last_rom_path = rom_path;
  lg::info("[libsm64] Initialized successfully");
  return true;
}

bool LibSM64Manager::init_autodetect() {
  if (m_initialized) {
    return true;
  }
  // Uses the project's ghc::filesystem alias from FileUtil.h.

  // Search order: directory next to gk.exe first (so a user drop-in ROM wins),
  // then iso_data/mario/ under the project dir. We pick the first .z64 whose
  // size matches the expected US ROM size.
  std::vector<fs::path> search_dirs;
  try {
    std::string exe_str = file_util::get_current_executable_path();
    if (!exe_str.empty()) {
      fs::path exe(exe_str);
      search_dirs.push_back(exe.parent_path());
    }
  } catch (...) {
    // fall through; we still have iso_data/mario as a fallback
  }
  try {
    fs::path proj = file_util::get_jak_project_dir();
    if (!proj.empty()) {
      search_dirs.push_back(proj / "iso_data" / "mario");
    }
  } catch (...) {
  }

  fs::path picked;
  for (const auto& dir : search_dirs) {
    std::error_code ec;
    if (!fs::exists(dir, ec) || !fs::is_directory(dir, ec)) continue;
    fs::directory_iterator it(dir, ec), end;
    if (ec) continue;
    for (; it != end; it.increment(ec)) {
      if (ec) break;
      const auto& entry = *it;
      if (!entry.is_regular_file(ec)) continue;
      const auto& p = entry.path();
      auto ext = p.extension().string();
      // Case-insensitive .z64 check.
      if (ext.size() != 4) continue;
      if ((ext[0] != '.') || (ext[1] != 'z' && ext[1] != 'Z') ||
          (ext[2] != '6') || (ext[3] != '4')) continue;
      auto sz = fs::file_size(p, ec);
      if (ec) continue;
      if (sz != kExpectedSm64RomSize) {
        lg::info("[libsm64] Skipping {} ({} bytes, expected {})", p.string(),
                 static_cast<size_t>(sz), static_cast<size_t>(kExpectedSm64RomSize));
        continue;
      }
      picked = p;
      break;
    }
    if (!picked.empty()) break;
  }

  if (picked.empty()) {
    lg::warn("[libsm64] Auto-detect: no matching .z64 found next to gk or in iso_data/mario");
    return false;
  }
  lg::info("[libsm64] Auto-detected ROM: {}", picked.string());
  return init(picked.string());
}

void LibSM64Manager::set_audio_volume(int volume) {
  if (volume < 0) volume = 0;
  if (volume > 100) volume = 100;
  m_audio_volume = volume;
  if (m_audio) {
    m_audio->set_volume(volume);
  }
}

int LibSM64Manager::get_audio_volume() const {
  return m_audio_volume;
}

void LibSM64Manager::shutdown() {
  if (!m_initialized) return;

  // Stop the audio worker thread first so it can't race against the global
  // terminate below. Destruct before we drop libsm64 state.
  if (m_audio) {
    m_audio->stop();
    m_audio.reset();
  }

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

  {
    // Serialize against the audio worker thread — libsm64's global state is
    // not reentrant and sm64_audio_tick() runs on cubeb's callback thread.
    std::scoped_lock lock(m_sm64_lock);
    sm64_mario_tick(m_mario_id, &sm64_input, &sm64_state, &sm64_geo);
  }

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

  // ---- Ground-pound hitbox simulation -------------------------------------
  // Replicates SM64's INT_GROUND_POUND_OR_TWIRL classification from
  // libsm64/src/decomp/game/interaction.c::determine_interaction. We don't
  // have access to libsm64's per-object collision pool (Jak actors are surface
  // objects, not behavior objects), so we evaluate the same hitbox geometry
  // here in C++ against our tracked Jak actors.
  //
  // Constants come from libsm64/src/decomp/game/object_stuff.c (Mario hitbox
  // radius = 37, height = 160, downOffset = 0) and the action IDs from
  // libsm64/src/decomp/include/sm64.h.
  constexpr uint32_t kActGroundPound = 0x008008A9;       // ACT_GROUND_POUND
  constexpr uint32_t kActGroundPoundLand = 0x0080023C;   // ACT_GROUND_POUND_LAND
  constexpr float kMarioHitboxRadiusSm64 = 37.0f;
  constexpr float kMarioHitboxHeightSm64 = 160.0f;

  // Reset per-frame flags. We do NOT reset frames_active or total_hits — those
  // accumulate across the lifetime so the debug GUI can show them. hits_this_frame
  // is overwritten by update_actor_collision when it runs the hit pass.
  bool active = false;
  bool impact = false;
  if (sm64_state.action == kActGroundPound) {
    // INT_GROUND_POUND_OR_TWIRL when vel.y < 0. In actionState 0 vel.y is set
    // to -50 every frame; in actionState 1 air_step preserves it. So just check.
    if (sm64_state.velocity[1] < 0.0f) {
      active = true;
    }
  } else if (sm64_state.action == kActGroundPoundLand) {
    // ACT_GROUND_POUND_LAND's handler immediately sets actionState=1 on entry,
    // so the "actionState == 0" window in determine_interaction is exactly the
    // first frame Mario is in this action. We detect that via prev_action edge.
    if (m_prev_action != kActGroundPoundLand) {
      active = true;
      impact = true;
    }
  }

  m_gp_hitbox.active = active;
  m_gp_hitbox.impact_frame = impact;
  m_gp_hitbox.center = m_state.position;  // already in Jak units, Mario's feet
  m_gp_hitbox.radius = kMarioHitboxRadiusSm64 * SM64_TO_JAK_SCALE;
  m_gp_hitbox.bottom_y = m_state.position.y();
  m_gp_hitbox.top_y = m_state.position.y() + kMarioHitboxHeightSm64 * SM64_TO_JAK_SCALE;
  if (active) {
    m_gp_hitbox.frames_active++;
  }
  if (!active) {
    // Once the pound finishes, clear the per-frame hit count so the GUI shows 0.
    m_gp_hitbox.hits_this_frame = 0;
  }

  m_prev_action = sm64_state.action;
}

GroundPoundHitbox LibSM64Manager::get_ground_pound_hitbox() {
  std::lock_guard<std::mutex> lock(m_geo_mutex);
  return m_gp_hitbox;
}

// Pure geometry test, exposed for unit tests. Both inputs in Jak units. The
// 2D check is `dx² + dz² < (hb.radius + actor_radius)²`. The Y check inflates
// the cylinder by the actor's half-height on both ends to model the actor as a
// vertical capsule rather than a point.
bool ground_pound_hitbox_overlaps(const GroundPoundHitbox& hb,
                                  const math::Vector3f& actor_pos,
                                  float actor_radius,
                                  float actor_half_height) {
  if (!hb.active) return false;
  float dx = actor_pos.x() - hb.center.x();
  float dz = actor_pos.z() - hb.center.z();
  float r = hb.radius + actor_radius;
  if (dx * dx + dz * dz > r * r) return false;
  float ay = actor_pos.y();
  if (ay + actor_half_height < hb.bottom_y) return false;
  if (ay - actor_half_height > hb.top_y) return false;
  return true;
}

bool ground_pound_hitbox_overlaps_aabb(const GroundPoundHitbox& hb,
                                        const float aabb_min[3],
                                        const float aabb_max[3]) {
  if (!hb.active) return false;
  // Y interval test.
  if (aabb_max[1] < hb.bottom_y) return false;
  if (aabb_min[1] > hb.top_y) return false;
  // XZ: closest point on the AABB rectangle to the hitbox center.
  float cx = hb.center.x();
  float cz = hb.center.z();
  float closest_x = cx < aabb_min[0] ? aabb_min[0] : (cx > aabb_max[0] ? aabb_max[0] : cx);
  float closest_z = cz < aabb_min[2] ? aabb_min[2] : (cz > aabb_max[2] ? aabb_max[2] : cz);
  float dx = closest_x - cx;
  float dz = closest_z - cz;
  return dx * dx + dz * dz <= hb.radius * hb.radius;
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
constexpr u32 PDRAW_NODE_LIST_OFF = 112;        // process-drawable.node-list (basic), declared 116 - 4
constexpr u32 CSHAPE_TRANS_OFF = 12;
constexpr u32 CSHAPE_QUAT_OFF = 28;
constexpr u32 CSHAPE_ROOT_PRIM_OFF = 156;
constexpr u32 PRIM_TRANSFORM_INDEX_OFF = 8;     // collide-shape-prim.transform-index (int8)
constexpr u32 PRIM_MESH_MESH_OFF = 68;
constexpr u32 PRIM_GROUP_NUM_PRIMS_OFF = 68;
constexpr u32 PRIM_GROUP_PRIM_ARRAY_OFF = 76;
constexpr u32 MESH_NUM_TRIS_OFF = 4;
constexpr u32 MESH_NUM_VERTS_OFF = 8;
constexpr u32 MESH_VERTEX_DATA_OFF = 12;
constexpr u32 MESH_TRIS_OFF = 28;
constexpr u32 MESH_TRI_SIZE = 8;  // collide-mesh-tri: 3 u8 indices + 1 u8 pad + 1 u32 pat
// cspace-array (process-drawable.node-list) layout:
//   inline-array-class is a basic. `data` is declared at offset 16; runtime
//   offset = 12 (declared - 4). Each cspace is a 32-byte structure (cspace-array
//   heap-base = 32). cspace itself is a `structure` (no type tag), so its field
//   offsets are NOT -4 adjusted: parent@0, joint@4, joint-num@8, geo@12, bone@16, ...
constexpr u32 CSPACE_ARRAY_DATA_OFF = 12;       // declared 16 - 4
constexpr u32 CSPACE_SIZE = 32;
constexpr u32 CSPACE_BONE_OFF = 16;             // structure field, no -4 adjust
// bone.transform is a 4x4 matrix at offset 0 of `bone`. Column-major:
//   col0 (offset 0..15)  = X basis
//   col1 (offset 16..31) = Y basis
//   col2 (offset 32..47) = Z basis
//   col3 (offset 48..63) = world translation (xyz, w)

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

// Decompose a row-major 3x3 rotation matrix into the (pitch, yaw, roll) Euler
// angles in degrees that libsm64's `mtxf_rotate_zxy_and_translate` expects in
// SM64ObjectTransform::eulerRotation. libsm64 internally negates degrees via
// `CONVERT_ANGLE`, so we precompute -extracted_angle here.
//
// Matrix indices: R[row*3+col]. libsm64 builds:
//   R[1][2] = -sin(pitch)            -> pitch = asin(-R[1][2])
//   R[1][0] / R[1][1] = sz/cz        -> roll  = atan2(R[1][0], R[1][1])
//   R[0][2] / R[2][2] = sy/cy        -> yaw   = atan2(R[0][2], R[2][2])
inline void rot_mat3_to_zxy_euler_degrees(const float rot[9], float out_deg[3]) {
  float r12 = rot[1 * 3 + 2];
  float r10 = rot[1 * 3 + 0];
  float r11 = rot[1 * 3 + 1];
  float r02 = rot[0 * 3 + 2];
  float r22 = rot[2 * 3 + 2];

  float sx = -r12;
  if (sx > 1.0f) sx = 1.0f;
  if (sx < -1.0f) sx = -1.0f;

  float pitch_rad = std::asin(sx);
  float yaw_rad, roll_rad;
  // Gimbal lock guard: when |sx| ~ 1, cos(pitch) ~ 0 and r10/r11 are tiny —
  // fall back to extracting roll from the (0,0)/(0,1) cell.
  if (std::abs(sx) > 0.9999f) {
    roll_rad = 0.0f;
    yaw_rad = std::atan2(-rot[2 * 3 + 0], rot[0 * 3 + 0]);
  } else {
    roll_rad = std::atan2(r10, r11);
    yaw_rad = std::atan2(r02, r22);
  }

  constexpr float RAD_TO_DEG = 57.29577951308232f;
  // Negate to cancel libsm64's CONVERT_ANGLE negation.
  out_deg[0] = -pitch_rad * RAD_TO_DEG;
  out_deg[1] = -yaw_rad * RAD_TO_DEG;
  out_deg[2] = -roll_rad * RAD_TO_DEG;
}

// Quaternion variant — go via the row-major 3x3 helper above.
inline void quat_to_zxy_euler_degrees(const float q[4], float out_deg[3]) {
  float rot[9];
  quat_to_rot_mat3(q, rot);
  rot_mat3_to_zxy_euler_degrees(rot, out_deg);
}

// Extract a single collide-mesh's local-space triangles as SM64Surfaces,
// in actor-local SM64 units (no rotation/translation applied). The caller
// passes the world transform separately to libsm64 via SM64ObjectTransform —
// this is what allows `sm64_surface_object_move` to compute platform velocity
// each frame and carry Mario along with moving platforms.
bool extract_mesh_local(u8* ee_mem, u32 mem_size, u32 mesh_ptr,
                        std::vector<SM64Surface>& out_surfaces,
                        float* out_local_aabb_min_jak = nullptr,
                        float* out_local_aabb_max_jak = nullptr) {
  u32 num_tris = 0, num_verts = 0, vertex_data_ptr = 0;
  if (!read_u32(ee_mem, mesh_ptr + MESH_NUM_TRIS_OFF, mem_size, num_tris)) return false;
  if (!read_u32(ee_mem, mesh_ptr + MESH_NUM_VERTS_OFF, mem_size, num_verts)) return false;
  if (!read_u32(ee_mem, mesh_ptr + MESH_VERTEX_DATA_OFF, mem_size, vertex_data_ptr)) return false;

  if (num_tris == 0 || num_tris > MAX_TRIS_PER_MESH) return false;
  if (num_verts == 0 || num_verts > MAX_VERTS_PER_MESH) return false;
  if (!valid_ee_addr(vertex_data_ptr, num_verts * 16u, mem_size)) return false;
  if (!valid_ee_addr(mesh_ptr + MESH_TRIS_OFF, num_tris * MESH_TRI_SIZE, mem_size)) return false;

  float lmin[3] = {std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                    std::numeric_limits<float>::max()};
  float lmax[3] = {-std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(),
                    -std::numeric_limits<float>::max()};
  std::vector<std::array<int32_t, 3>> local_verts(num_verts);
  for (u32 i = 0; i < num_verts; i++) {
    float local_v[4];
    if (!read_vec4(ee_mem, vertex_data_ptr + i * 16u, mem_size, local_v)) return false;
    for (int k = 0; k < 3; k++) {
      if (local_v[k] < lmin[k]) lmin[k] = local_v[k];
      if (local_v[k] > lmax[k]) lmax[k] = local_v[k];
    }
    local_verts[i][0] = static_cast<int32_t>(local_v[0] * JAK_TO_SM64_SCALE);
    local_verts[i][1] = static_cast<int32_t>(local_v[1] * JAK_TO_SM64_SCALE);
    local_verts[i][2] = static_cast<int32_t>(local_v[2] * JAK_TO_SM64_SCALE);
  }
  if (out_local_aabb_min_jak) std::memcpy(out_local_aabb_min_jak, lmin, sizeof(lmin));
  if (out_local_aabb_max_jak) std::memcpy(out_local_aabb_max_jak, lmax, sizeof(lmax));

  out_surfaces.reserve(out_surfaces.size() + num_tris);
  for (u32 i = 0; i < num_tris; i++) {
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
      s.vertices[v][0] = local_verts[indices[v]][0];
      s.vertices[v][1] = local_verts[indices[v]][1];
      s.vertices[v][2] = local_verts[indices[v]][2];
    }
    out_surfaces.push_back(s);
  }
  return true;
}

// Per-prim collection result: identifies the prim, its collide-mesh, and
// which bone (if any) the prim is attached to via transform-index. We need
// the prim ptr separately because two prim-meshes in the same group can
// share the same collide-mesh template — keying tracked actors by mesh_ptr
// alone collapses them onto each other.
struct CollectedPrim {
  u32 prim_ptr;
  u32 mesh_ptr;
  int8_t transform_index;
};

// Recursively collect prim-mesh entries from a collide-shape-prim hierarchy.
void collect_mesh_prims(u8* ee_mem, u32 mem_size, u32 prim_ptr, u32 false_val,
                        u32 prim_mesh_type, u32 prim_group_type,
                        std::vector<CollectedPrim>& out_prims, int depth = 0) {
  if (prim_ptr == 0 || prim_ptr == false_val || depth > MAX_PRIM_DEPTH) return;
  u32 prim_type;
  if (!read_basic_type(ee_mem, prim_ptr, mem_size, prim_type)) return;

  if (prim_type == prim_mesh_type) {
    u32 mesh_ptr;
    if (!read_u32(ee_mem, prim_ptr + PRIM_MESH_MESH_OFF, mem_size, mesh_ptr)) return;
    if (mesh_ptr != 0 && mesh_ptr != false_val) {
      // transform-index is an int8 in collide-shape-prim. -2 = no joint
      // attachment, >=0 = bone index into process-drawable.node-list.
      int8_t xform_idx = -2;
      if (valid_ee_addr(prim_ptr + PRIM_TRANSFORM_INDEX_OFF, 1, mem_size)) {
        xform_idx = static_cast<int8_t>(ee_mem[prim_ptr + PRIM_TRANSFORM_INDEX_OFF]);
      }
      out_prims.push_back({prim_ptr, mesh_ptr, xform_idx});
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
                         out_prims, depth + 1);
    }
  }
  // spheres and other prim types are ignored.
}

// Resolve a prim's per-prim world transform. For prims with transform_index >= 0
// and a valid node-list, looks up `process-drawable.node-list[index].bone.transform`
// and decomposes it into translation + rotation. For everything else falls back
// to the actor's root cshape trans/quat (the existing behavior).
//
// out_pos is in Jak units; out_rot is row-major 3x3 (orthonormal rotation
// extracted from the bone matrix, with scale stripped).
bool compute_prim_world_transform(u8* ee_mem, u32 mem_size, u32 false_val, u32 pd_node,
                                   int8_t transform_index, const float root_trans[3],
                                   const float root_rot[9], float out_pos[3], float out_rot[9],
                                   bool* out_used_bone = nullptr) {
  // Default: actor root.
  auto use_root = [&]() {
    out_pos[0] = root_trans[0];
    out_pos[1] = root_trans[1];
    out_pos[2] = root_trans[2];
    std::memcpy(out_rot, root_rot, 9 * sizeof(float));
    if (out_used_bone) *out_used_bone = false;
    return true;
  };

  if (transform_index < 0) return use_root();

  // process-drawable.node-list — basic ptr to a cspace-array.
  u32 node_list = 0;
  if (!read_u32(ee_mem, pd_node + PDRAW_NODE_LIST_OFF, mem_size, node_list)) return false;
  if (node_list == 0 || node_list == false_val) return use_root();
  if (!valid_basic_ptr(node_list, mem_size)) return use_root();

  // Bounds-check transform_index against cspace-array.length (offset 0).
  // Skeleton may not be initialized yet (length == 0) or the index might
  // be bogus. Either way, fall back to the actor root rather than reading
  // garbage from beyond the end of the array.
  u32 cspace_len_raw = 0;
  if (!read_u32(ee_mem, node_list, mem_size, cspace_len_raw)) return use_root();
  int32_t cspace_len = static_cast<int32_t>(cspace_len_raw);
  if (cspace_len <= 0 || cspace_len > 1024) return use_root();  // sanity bound
  if (static_cast<int32_t>(transform_index) >= cspace_len) return use_root();

  // cspace[i] starts at node_list + 16 + i*32. cspace.bone is a basic ptr
  // at +16 within the cspace struct.
  u32 cspace_addr =
      node_list + CSPACE_ARRAY_DATA_OFF + static_cast<u32>(transform_index) * CSPACE_SIZE;
  u32 bone_ptr = 0;
  if (!read_u32(ee_mem, cspace_addr + CSPACE_BONE_OFF, mem_size, bone_ptr)) return use_root();
  if (bone_ptr == 0 || bone_ptr == false_val) return use_root();
  // bone is a structure (not a basic), so no -4 type tag — just check the
  // raw matrix range is in-bounds. Require 16-byte alignment so the matrix
  // load is well-defined.
  if ((bone_ptr & 0xF) != 0) return use_root();
  if (!valid_ee_addr(bone_ptr, 64, mem_size)) return use_root();

  // bone.transform: 4x4 column-major, 16 floats. Sanity-check finiteness
  // AND that translation is within a reasonable Jak-world range, so we
  // don't push libsm64 absurd transforms when the bone is uninitialized.
  float m[16];
  std::memcpy(m, ee_mem + bone_ptr, 64);
  for (int i = 0; i < 16; i++) {
    if (!std::isfinite(m[i])) return use_root();
  }
  // Translation is column 3. Jak world coords are in the millions max
  // (4096 units/meter). Anything beyond ±1e8 is garbage.
  for (int i = 0; i < 3; i++) {
    if (std::abs(m[3 * 4 + i]) > 1.0e8f) return use_root();
  }

  // Translation = column 3 (xyz). m[col*4 + row].
  out_pos[0] = m[3 * 4 + 0];
  out_pos[1] = m[3 * 4 + 1];
  out_pos[2] = m[3 * 4 + 2];

  // Build row-major 3x3 rotation from columns 0/1/2 of the bone matrix.
  // out_rot[row*3 + col] = m[col*4 + row]
  for (int r = 0; r < 3; r++) {
    for (int c = 0; c < 3; c++) {
      out_rot[r * 3 + c] = m[c * 4 + r];
    }
  }

  // Strip scale by normalizing each column (each column is a basis vector).
  // Bone matrices in jak1 are typically scale=1 anyway, but be safe.
  for (int c = 0; c < 3; c++) {
    float lx = out_rot[0 * 3 + c];
    float ly = out_rot[1 * 3 + c];
    float lz = out_rot[2 * 3 + c];
    float len2 = lx * lx + ly * ly + lz * lz;
    if (len2 < 1e-12f || !std::isfinite(len2)) return use_root();
    float inv = 1.0f / std::sqrt(len2);
    out_rot[0 * 3 + c] = lx * inv;
    out_rot[1 * 3 + c] = ly * inv;
    out_rot[2 * 3 + c] = lz * inv;
  }
  if (out_used_bone) *out_used_bone = true;
  return true;
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

// Compose a tracking key from (process-drawable address, collide-shape-prim
// address). We key on the prim — not the mesh — because two prims inside the
// same prim-group can share a single collide-mesh template (e.g. mirrored
// sub-pieces) yet need their own libsm64 surface object because their
// `transform-index` (and therefore world transform) differs.
inline uint64_t make_actor_key(u32 pd_node, u32 prim_ptr) {
  return (static_cast<uint64_t>(pd_node) << 32) | static_cast<uint64_t>(prim_ptr);
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

    std::vector<CollectedPrim> prims;
    collect_mesh_prims(c.ee_mem, c.mem_size, root_prim, c.false_val, c.prim_mesh_type,
                       c.prim_group_type, prims);
    if (prims.empty()) continue;

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

    // Pre-compute the actor root rotation matrix once — used as the fallback
    // for prims that aren't bone-attached.
    float root_rot[9];
    quat_to_rot_mat3(quat, root_rot);

    for (const CollectedPrim& cp : prims) {
      u32 prim_ptr = cp.prim_ptr;
      u32 mesh_ptr = cp.mesh_ptr;

      // Skip meshes we've already decided are broken.
      if (c.broken_meshes.count(mesh_ptr)) continue;

      // Key by (process-drawable, prim) so multiple prims inside one
      // prim-group that share a mesh template each get their own libsm64
      // surface object. Each bone-attached prim shows up as a distinct
      // entry — effectively `<actor>-1`, `<actor>-2`, etc.
      uint64_t key = make_actor_key(node, prim_ptr);
      auto& tracked = c.tracked_actors[key];
      tracked.seen_this_frame = true;

      // Resolve this prim's per-prim world transform. For prims attached to
      // a bone (transform_index >= 0) this walks the actor's node-list and
      // pulls the bone matrix; otherwise it falls back to the actor root.
      float prim_pos[3];
      float prim_rot[9];
      bool used_bone = false;
      if (!compute_prim_world_transform(c.ee_mem, c.mem_size, c.false_val, node,
                                         cp.transform_index, trans, root_rot, prim_pos,
                                         prim_rot, &used_bone)) {
        continue;
      }
      if (cp.transform_index >= 0) {
        c.result.bone_lookups_attempted++;
        if (used_bone) {
          c.result.bone_lookups_succeeded++;
        } else {
          c.result.bone_lookups_fell_back++;
        }
      }
      // Capture for tests/diagnostics. Bounded to keep production overhead low.
      if (c.result.captured_prims.size() < 256) {
        LibSM64Manager::TestSweepResult::CapturedPrim cap{};
        cap.pos[0] = prim_pos[0];
        cap.pos[1] = prim_pos[1];
        cap.pos[2] = prim_pos[2];
        cap.transform_index = cp.transform_index;
        cap.used_bone = used_bone;
        c.result.captured_prims.push_back(cap);
      }

      // Build the SM64ObjectTransform for this prim. For root-relative prims
      // this is the actor pose; for bone-attached prims it's the bone's world
      // pose pulled from the cspace-array. In either case we use the
      // sm64_surface_object_move flow (NOT destroy/recreate) so libsm64 can
      // compute platform velocity from the per-frame delta and carry Mario.
      SM64ObjectTransform xform{};
      xform.position[0] = prim_pos[0] * JAK_TO_SM64_SCALE;
      xform.position[1] = prim_pos[1] * JAK_TO_SM64_SCALE;
      xform.position[2] = prim_pos[2] * JAK_TO_SM64_SCALE;
      rot_mat3_to_zxy_euler_degrees(prim_rot, xform.eulerRotation);

      // Helper: refresh the world-space AABB on the tracked actor from its
      // local AABB and the prim's current world transform. Conservative — we
      // transform all 8 corners of the local AABB and take per-axis min/max,
      // which gives a tight fit for axis-aligned meshes and a loose-but-safe
      // fit for rotated ones.
      auto refresh_world_aabb = [&]() {
        if (!tracked.has_aabb) return;
        const float* lmin = tracked.local_aabb_min;
        const float* lmax = tracked.local_aabb_max;
        float wmin[3] = {std::numeric_limits<float>::max(),
                         std::numeric_limits<float>::max(),
                         std::numeric_limits<float>::max()};
        float wmax[3] = {-std::numeric_limits<float>::max(),
                         -std::numeric_limits<float>::max(),
                         -std::numeric_limits<float>::max()};
        for (int corner = 0; corner < 8; corner++) {
          float lv[3] = {(corner & 1) ? lmax[0] : lmin[0],
                         (corner & 2) ? lmax[1] : lmin[1],
                         (corner & 4) ? lmax[2] : lmin[2]};
          float wv[3];
          // wv = prim_rot * lv + prim_pos. prim_rot is row-major 3x3.
          for (int r = 0; r < 3; r++) {
            wv[r] = prim_rot[r * 3 + 0] * lv[0] + prim_rot[r * 3 + 1] * lv[1] +
                    prim_rot[r * 3 + 2] * lv[2] + prim_pos[r];
          }
          for (int k = 0; k < 3; k++) {
            if (wv[k] < wmin[k]) wmin[k] = wv[k];
            if (wv[k] > wmax[k]) wmax[k] = wv[k];
          }
        }
        std::memcpy(tracked.world_aabb_min, wmin, sizeof(wmin));
        std::memcpy(tracked.world_aabb_max, wmax, sizeof(wmax));
      };

      if (tracked.has_obj) {
        if (!c.dry_run) {
          sm64_surface_object_move(tracked.sm64_obj_id, &xform);
        }
        std::memcpy(tracked.last_trans, prim_pos, 12);
        refresh_world_aabb();
        continue;
      }

      if (created_this_frame >= MAX_ACTOR_SURFACE_OBJECTS) break;

      std::vector<SM64Surface> surfaces;
      float local_aabb_min[3];
      float local_aabb_max[3];
      if (!extract_mesh_local(c.ee_mem, c.mem_size, mesh_ptr, surfaces, local_aabb_min,
                              local_aabb_max)) {
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
        SM64SurfaceObject obj{};
        obj.transform = xform;
        obj.surfaceCount = static_cast<uint32_t>(surfaces.size());
        obj.surfaces = surfaces.data();
        tracked.sm64_obj_id = sm64_surface_object_create(&obj);
        tracked.has_obj = true;
      }
      std::memcpy(tracked.last_trans, prim_pos, 12);
      std::memcpy(tracked.local_aabb_min, local_aabb_min, sizeof(local_aabb_min));
      std::memcpy(tracked.local_aabb_max, local_aabb_max, sizeof(local_aabb_max));
      tracked.has_aabb = true;
      refresh_world_aabb();
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

  // ---- Ground-pound hit pass ----------------------------------------------
  // After the walker has refreshed every tracked actor's last_trans for this
  // frame, intersect each visible actor against Mario's ground-pound hitbox.
  // We just count hits for now (visualization in the debug GUI); applying the
  // attack to GOAL processes is the next step. Done OUTSIDE the walker so the
  // testable test_sweep path stays focused on collision-mesh extraction.
  {
    GroundPoundHitbox hb_snapshot;
    {
      std::lock_guard<std::mutex> lock(m_geo_mutex);
      hb_snapshot = m_gp_hitbox;
    }
    uint32_t hits = 0;
    if (hb_snapshot.active) {
      for (auto& [k, t] : m_tracked_actors) {
        if (!t.seen_this_frame) continue;
        // Prefer the world-space collide-mesh AABB if available — last_trans
        // is the prim anchor (often at the actor's base), so testing against
        // the prim point alone misses anything stacked above the pivot.
        bool hit;
        if (t.has_aabb) {
          hit = ground_pound_hitbox_overlaps_aabb(hb_snapshot, t.world_aabb_min, t.world_aabb_max);
        } else {
          constexpr float kActorPadRadius = 4096.0f;
          constexpr float kActorHalfHeight = 4096.0f;
          math::Vector3f actor_pos(t.last_trans[0], t.last_trans[1], t.last_trans[2]);
          hit = ground_pound_hitbox_overlaps(hb_snapshot, actor_pos, kActorPadRadius,
                                              kActorHalfHeight);
        }
        if (hit) {
          hits++;
          if (m_actor_diag_logs_remaining > 0) {
            m_actor_diag_logs_remaining--;
            lg::info(
                "[libsm64] ground-pound HIT actor pd=0x{:X} prim=0x{:X} aabb=({:.0f},{:.0f},{:.0f})-({:.0f},{:.0f},{:.0f})"
                " mario=({:.0f},{:.0f},{:.0f}) impact={}",
                static_cast<u32>(k >> 32), static_cast<u32>(k & 0xFFFFFFFFu),
                t.world_aabb_min[0], t.world_aabb_min[1], t.world_aabb_min[2],
                t.world_aabb_max[0], t.world_aabb_max[1], t.world_aabb_max[2],
                hb_snapshot.center.x(), hb_snapshot.center.y(), hb_snapshot.center.z(),
                hb_snapshot.impact_frame ? "YES" : "no");
          }
        }
      }
    }
    if (hits > 0 || hb_snapshot.active) {
      std::lock_guard<std::mutex> lock(m_geo_mutex);
      m_gp_hitbox.hits_this_frame = hits;
      m_gp_hitbox.total_hits += hits;
    }
  }

  m_actor_sync_frame++;
  if (m_actor_sync_frame == 1 || m_actor_sync_frame == 60 || m_actor_sync_frame == 600) {
    lg::info(
        "[libsm64] actor-collision frame {}: visited {} nodes, pd_seen {}, meshes {}, tris {}, errors {}, tracking {}, bone_attempts {}, bone_ok {}, bone_fellback {}",
        m_actor_sync_frame, result.process_tree_nodes_visited, result.process_drawables_seen,
        result.meshes_found, result.triangles_extracted, result.errors, m_tracked_actors.size(),
        result.bone_lookups_attempted, result.bone_lookups_succeeded,
        result.bone_lookups_fell_back);
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
