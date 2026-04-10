/*!
 * @file sm64_debug_gui.cpp
 * ImGui debug window for libsm64 integration.
 */

#include "sm64_debug_gui.h"

#include "game/graphics/opengl_renderer/loader/Loader.h"
#include "game/libsm64/libsm64_integration.h"

#include "third-party/imgui/imgui.h"

namespace sm64 {

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
  ImGui::Separator();

  // Initialization
  if (ImGui::CollapsingHeader("Initialization", ImGuiTreeNodeFlags_DefaultOpen)) {
    ImGui::InputText("ROM Path", m_rom_path, sizeof(m_rom_path));

    if (!mgr.is_initialized()) {
      if (ImGui::Button("Initialize SM64")) {
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
            // Merge collision from all loaded levels
            std::vector<tfrag3::CollisionMesh::Vertex> all_verts;
            all_verts.reserve(total_verts);
            for (auto* lev : levels) {
              if (lev->level) {
                auto& verts = lev->level->collision.vertices;
                all_verts.insert(all_verts.end(), verts.begin(), verts.end());
              }
            }
            mgr.load_level_collision(all_verts);
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
            mgr.create_mario(camera_pos[0], camera_pos[1], camera_pos[2]);
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
            mgr.create_mario(camera_pos[0], camera_pos[1], camera_pos[2]);
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
