/*!
 * @file sm64_debug_gui.cpp
 * ImGui debug window for libsm64 integration.
 */

#include "sm64_debug_gui.h"

#include "common/log/log.h"
#include "game/graphics/opengl_renderer/loader/Loader.h"
#include "game/libsm64/libsm64_integration.h"

#include "third-party/imgui/imgui.h"

namespace sm64 {

// Walks every Jak level currently held by the loader, concatenates their
// collision vertex buffers, and pushes the result into libsm64 as the level
// collision. Returns the total triangle count actually sent. Returns 0 if no
// levels are loaded (so the caller can report "nothing to reload").
static size_t reload_level_collision_from_loader(LibSM64Manager& mgr,
                                                  std::shared_ptr<Loader>& loader) {
  if (!loader) return 0;
  auto levels = loader->get_in_use_levels();
  if (levels.empty()) return 0;
  size_t total_verts = 0;
  for (auto* lev : levels) {
    if (lev->level) total_verts += lev->level->collision.vertices.size();
  }
  if (total_verts == 0) return 0;
  std::vector<tfrag3::CollisionMesh::Vertex> all_verts;
  all_verts.reserve(total_verts);
  for (auto* lev : levels) {
    if (lev->level) {
      auto& verts = lev->level->collision.vertices;
      all_verts.insert(all_verts.end(), verts.begin(), verts.end());
    }
  }
  mgr.load_level_collision(all_verts);
  return all_verts.size() / 3;
}

// Spawn helper: tries to create Mario, and if the spawn fails (libsm64 returns
// -1 when there's no floor at the spawn position — usually because the level
// collision hasn't been pushed yet for the current level), automatically
// refreshes the level collision from whatever Jak has loaded and retries.
static int32_t spawn_mario_with_collision_fallback(LibSM64Manager& mgr,
                                                    std::shared_ptr<Loader>& loader,
                                                    float x, float y, float z) {
  int32_t id = mgr.create_mario(x, y, z);
  if (id >= 0) return id;
  lg::warn("[libsm64] Mario spawn failed at ({:.1f}, {:.1f}, {:.1f}) — reloading level collision and retrying",
           x, y, z);
  size_t tris = reload_level_collision_from_loader(mgr, loader);
  if (tris == 0) {
    lg::error("[libsm64] No level collision available to reload — spawn will stay failed");
    return -1;
  }
  lg::info("[libsm64] Reloaded {} level collision triangles, retrying spawn", tris);
  return mgr.create_mario(x, y, z);
}

void SM64DebugGui::draw(std::shared_ptr<Loader> loader, const float* camera_pos) {
  if (!m_visible) return;

  auto& mgr = LibSM64Manager::instance();

  ImGui::SetNextWindowSize(ImVec2(400, 500), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("libsm64 - Mario 64", &m_visible)) {
    ImGui::End();
    return;
  }

  // Status
  ImGui::TextColored(mgr.is_initialized() ? ImVec4(0, 1, 0, 1) : ImVec4(1, 0, 0, 1),
                     mgr.is_initialized() ? "SM64 Initialized" : "SM64 Not Initialized");

  ImGui::Checkbox("Enabled", &mgr.enabled);
  ImGui::Checkbox("Follow Mario (lock Jak to Mario)", &mgr.follow_mario);
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Teleports Jak to Mario's position each frame\n"
                     "so the game camera follows Mario.");
  }
  ImGui::Checkbox("Auto-sync Collision", &mgr.auto_sync_collision);
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Automatically reload SM64 collision surfaces\n"
                     "when Jak levels load or unload.");
  }
  bool prev_dynamic_actor_collision = mgr.dynamic_actor_collision;
  ImGui::Checkbox("Dynamic Actor Collision", &mgr.dynamic_actor_collision);
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Walk the Jak process tree each frame and mirror\n"
                     "actor collide-shapes (moving platforms, crates, enemies)\n"
                     "into libsm64 as surface objects so Mario can stand on\n"
                     "and collide with them.");
  }
  if (prev_dynamic_actor_collision && !mgr.dynamic_actor_collision) {
    mgr.clear_actor_collision();
  }
  ImGui::Separator();

  // Initialization
  if (ImGui::CollapsingHeader("Initialization", ImGuiTreeNodeFlags_DefaultOpen)) {
    // Show the auto-detected ROM path (if any) so the user knows what we picked.
    const auto& detected = mgr.last_rom_path();
    if (!detected.empty()) {
      ImGui::TextWrapped("Detected ROM: %s", detected.c_str());
    } else {
      ImGui::TextDisabled("No ROM auto-detected — drop an SM64 US .z64 next to\n"
                          "gk.exe or into iso_data/mario/, or use the override below.");
    }
    ImGui::InputText("ROM Path override", m_rom_path, sizeof(m_rom_path));

    if (!mgr.is_initialized()) {
      if (ImGui::Button("Auto-detect & Init")) {
        mgr.init_autodetect();
      }
      ImGui::SameLine();
      if (ImGui::Button("Init From Override")) {
        mgr.init(m_rom_path);
      }
    } else {
      if (ImGui::Button("Shutdown SM64")) {
        mgr.shutdown();
      }
    }
  }

  // Collision Surfaces
  if (mgr.is_initialized()) {
    if (ImGui::CollapsingHeader("Collision", ImGuiTreeNodeFlags_DefaultOpen)) {
      // Level collision loading
      if (loader) {
        auto levels = loader->get_in_use_levels();
        if (!levels.empty()) {
          size_t total_verts = 0;
          for (auto* lev : levels) {
            if (lev->level) {
              total_verts += lev->level->collision.vertices.size();
            }
          }
          ImGui::Text("Loaded levels: %d (%zu collision triangles)",
                      (int)levels.size(), total_verts / 3);

          if (ImGui::Button("Load Level Collision")) {
            reload_level_collision_from_loader(mgr, loader);
          }
          ImGui::SameLine();
          ImGui::TextDisabled("(?)");
          if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "Loads collision geometry from the currently loaded Jak levels.\n"
                "Mario will be able to walk on the actual game terrain.");
          }
        } else {
          ImGui::TextDisabled("No levels loaded - start a Jak game first");
        }
      }

      if (mgr.get_loaded_surface_count() > 0) {
        ImGui::Text("SM64 surfaces loaded: %d", mgr.get_loaded_surface_count());
      }

      ImGui::Separator();

      // Manual flat ground
      ImGui::DragFloat("Ground Y", &m_ground_y, 0.5f, -1000.0f, 1000.0f);
      ImGui::DragFloat("Ground Extent", &m_ground_extent, 10.0f, 10.0f, 10000.0f);
      if (ImGui::Button("Load Flat Ground")) {
        mgr.load_flat_ground(m_ground_y, m_ground_extent);
      }
      ImGui::SameLine();
      ImGui::TextDisabled("(?)");
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Loads a flat collision plane for Mario to walk on.\n"
                         "Set Y to the height of the ground in Jak coordinates.");
      }
    }
  }

  // Mario Spawning
  if (mgr.is_initialized()) {
    if (ImGui::CollapsingHeader("Mario", ImGuiTreeNodeFlags_DefaultOpen)) {
      ImGui::DragFloat3("Spawn Position", m_spawn_pos, 0.5f);

      if (!mgr.has_mario()) {
        if (ImGui::Button("Spawn Mario")) {
          mgr.create_mario(m_spawn_pos[0], m_spawn_pos[1], m_spawn_pos[2]);
        }
        if (camera_pos) {
          ImGui::SameLine();
          if (ImGui::Button("Spawn Mario at Camera Position")) {
            spawn_mario_with_collision_fallback(mgr, loader, camera_pos[0], camera_pos[1],
                                                 camera_pos[2]);
          }
        }
      } else {
        if (ImGui::Button("Delete Mario")) {
          mgr.delete_mario(mgr.get_mario_id());
        }
        if (camera_pos) {
          ImGui::SameLine();
          if (ImGui::Button("Respawn at Camera Position")) {
            mgr.delete_mario(mgr.get_mario_id());
            spawn_mario_with_collision_fallback(mgr, loader, camera_pos[0], camera_pos[1],
                                                 camera_pos[2]);
          }
        }

        // Show Mario state
        ImGui::Separator();
        auto state = mgr.get_state();
        ImGui::Text("Position: (%.2f, %.2f, %.2f)",
                    state.position.x(), state.position.y(), state.position.z());
        ImGui::Text("Velocity: (%.2f, %.2f, %.2f)",
                    state.velocity.x(), state.velocity.y(), state.velocity.z());
        ImGui::Text("Face Angle: %.2f rad", state.face_angle);
        ImGui::Text("Forward Vel: %.2f", state.forward_velocity);
        ImGui::Text("Health: 0x%04X (%d/8 wedges)", state.health, (state.health >> 8) & 0xF);
        ImGui::Text("Action: 0x%08X", state.action);
        ImGui::Text("Flags: 0x%08X", state.flags);
        ImGui::Text("Anim: %d (frame %d)", state.anim_id, state.anim_frame);

        auto geo = mgr.get_geometry();
        ImGui::Text("Triangles: %d", geo.num_triangles);

        // Ground pound hitbox simulation status.
        ImGui::Separator();
        auto hb = mgr.get_ground_pound_hitbox();
        ImGui::TextColored(hb.active ? ImVec4(1, 0.5f, 0, 1) : ImVec4(0.6f, 0.6f, 0.6f, 1),
                           "Ground Pound: %s%s",
                           hb.active ? "ACTIVE" : "idle",
                           hb.impact_frame ? " (IMPACT)" : "");
        ImGui::Text("  hitbox center: (%.0f, %.0f, %.0f)", hb.center.x(), hb.center.y(),
                    hb.center.z());
        ImGui::Text("  radius: %.0f  y range: [%.0f, %.0f]", hb.radius, hb.bottom_y, hb.top_y);
        ImGui::Text("  frames active: %u  hits this frame: %u  total hits: %u",
                    hb.frames_active, hb.hits_this_frame, hb.total_hits);
      }
    }
  }

  // Controls help
  if (ImGui::CollapsingHeader("Controls")) {
    ImGui::BulletText("Left Stick: Move Mario");
    ImGui::BulletText("Cross (X): Jump (A button)");
    ImGui::BulletText("Square: Punch/Attack (B button)");
    ImGui::BulletText("L2: Crouch/Ground Pound (Z button)");
    ImGui::BulletText("Camera follows Jak's camera direction");
  }

  ImGui::End();
}

}  // namespace sm64
