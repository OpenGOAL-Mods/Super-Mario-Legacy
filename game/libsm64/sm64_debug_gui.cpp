/*!
 * @file sm64_debug_gui.cpp
 * ImGui debug window for libsm64 integration.
 */

#include "sm64_debug_gui.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>

#include "common/log/log.h"
#include "common/util/FileUtil.h"
#include "game/graphics/opengl_renderer/loader/Loader.h"
#include "game/libsm64/libsm64_integration.h"
#include "game/runtime.h"

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

// Snapshot every loaded static surface and write it out as two files — a
// .obj (loadable in Blender / MeshLab) and a .csv (grep-friendly sidecar
// with per-triangle type/force/terrain).  Returns the absolute path of
// the .obj that was written, or empty string if nothing was loaded.
// Writes into the per-game misc dir (`%APPDATA%/OpenGOAL/jak1/misc/
// sm64_collision_dumps/` on Windows) so multiple dumps accumulate and
// survive build-tree wipes.
static std::string dump_surfaces_to_files() {
  auto tris = LibSM64Manager::instance().snapshot_static_surfaces();
  if (tris.empty()) return {};

  // Timestamp the filename so each press produces a unique pair.
  std::time_t t = std::time(nullptr);
  std::tm local{};
#ifdef _WIN32
  localtime_s(&local, &t);
#else
  localtime_r(&t, &local);
#endif
  char stamp[32];
  std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &local);

  // `file_util::get_user_misc_dir` returns a `ghc::filesystem::path`
  // (third-party polyfill) which doesn't implicitly convert to
  // `std::filesystem::path` on MSVC.  Convert through .string() so we
  // stay in std::filesystem-land here — cheaper than pulling in the
  // ghc polyfill header just for this.
  namespace fs = std::filesystem;
  fs::path dir = fs::path(file_util::get_user_misc_dir(g_game_version).string()) /
                  "sm64_collision_dumps";
  std::error_code ec;
  fs::create_directories(dir, ec);
  if (ec) {
    lg::error("[libsm64] Failed to create dump directory {}: {}", dir.string(), ec.message());
    return {};
  }

  fs::path obj_path = dir / (std::string("sm64_collision_") + stamp + ".obj");
  fs::path csv_path = dir / (std::string("sm64_collision_") + stamp + ".csv");

  // ---- OBJ (geometry, loadable in Blender) -----------------------------
  // Positions are in SM64 units — Blender's default is meters, so imported
  // meshes will look huge.  Multiply by SM64_TO_JAK_SCALE (~81.92 at scale
  // 50) to get Jak units, or just apply the Transform scale in the
  // importer.  Faces are 3-indexed (OBJ is 1-based).  A comment above
  // each `f` line carries the surface type/force/terrain so you can
  // read the dump without the sidecar CSV.
  {
    std::ofstream obj(obj_path);
    if (!obj) {
      lg::error("[libsm64] Failed to open {} for writing", obj_path.string());
      return {};
    }
    obj << "# SM64 Legacy collision dump\n";
    obj << "# Generated: " << stamp << "\n";
    obj << "# Triangle count: " << tris.size() << "\n";
    obj << "# Units: SM64 (multiply positions by SM64_TO_JAK_SCALE="
        << SM64_TO_JAK_SCALE << " for Jak units)\n";
    obj << "o sm64_collision\n";
    for (const auto& tri : tris) {
      for (int v = 0; v < 3; v++) {
        obj << "v " << tri.verts[v][0] << ' ' << tri.verts[v][1] << ' ' << tri.verts[v][2] << '\n';
      }
    }
    size_t base = 1;  // OBJ indices are 1-based
    for (size_t i = 0; i < tris.size(); i++, base += 3) {
      char buf[96];
      std::snprintf(buf, sizeof(buf), "# tri %zu type=0x%04X force=%d terrain=0x%04X\n",
                    i, (unsigned)(uint16_t)tris[i].type, (int)tris[i].force,
                    (unsigned)tris[i].terrain);
      obj << buf;
      obj << "f " << base << ' ' << (base + 1) << ' ' << (base + 2) << '\n';
    }
  }

  // ---- CSV (grep-friendly metadata) -----------------------------------
  {
    std::ofstream csv(csv_path);
    if (!csv) {
      lg::error("[libsm64] Failed to open {} for writing", csv_path.string());
      return obj_path.string();  // still return the OBJ; it wrote OK
    }
    csv << "tri,type,force,terrain,v0x,v0y,v0z,v1x,v1y,v1z,v2x,v2y,v2z\n";
    for (size_t i = 0; i < tris.size(); i++) {
      const auto& tri = tris[i];
      csv << i << ','
          << (int)tri.type << ',' << (int)tri.force << ',' << (unsigned)tri.terrain;
      for (int v = 0; v < 3; v++) {
        csv << ',' << tri.verts[v][0] << ',' << tri.verts[v][1] << ',' << tri.verts[v][2];
      }
      csv << '\n';
    }
  }

  lg::info("[libsm64] Dumped {} collision triangles to {}", tris.size(), obj_path.string());
  return obj_path.string();
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

void SM64DebugGui::draw(std::shared_ptr<Loader> loader) {
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
  ImGui::Checkbox("Hide Jak when Mario is active", &mgr.hide_jak_model);
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Skips rendering Jak's player model (eichar-lod0)\n"
                     "while a Mario instance exists, so Mario isn't clipped\n"
                     "inside Jak.");
  }
  ImGui::Checkbox("Water Sync (Mario swims where Jak swims)", &mgr.water_sync);
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Reads Jak's water-control each frame and forwards\n"
                     "the water surface Y into libsm64 so Mario enters and\n"
                     "exits swim state along with Jak.");
  }
  bool prev_yakow_grab = mgr.yakow_grab;
  ImGui::Checkbox("Yakow Grab (punch near cow to pick up)", &mgr.yakow_grab);
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Walks the Jak process tree each frame looking for\n"
                     "yakow actors. When Mario is within grab range and\n"
                     "presses Square (B), the closest yakow is glued to\n"
                     "Mario's hand via the libsm64 fake-held-object API.\n"
                     "Press Square again to throw the yakow (Mario runs the\n"
                     "native throw action). Toggling off releases any held\n"
                     "yakow and restores its normal AI.");
  }
  if (prev_yakow_grab && !mgr.yakow_grab) {
    mgr.clear_yakow_grab();
  }
  bool prev_safety_floor = mgr.safety_floor;
  ImGui::Checkbox("Safety Floor (pseudo-floor under Mario)", &mgr.safety_floor);
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Spawns a huge invisible quad beneath Mario each frame\n"
                     "so libsm64's floor query always returns something, even\n"
                     "when Mario is out over the void. Real level floors stay\n"
                     "the nearest floor below Mario — the safety quad only\n"
                     "kicks in when he's walked off the edge. Prevents the\n"
                     "NULL-floor crash mode when jumping off map edges.");
  }
  if (prev_safety_floor && !mgr.safety_floor) {
    mgr.clear_safety_floor();
  }
  ImGui::Checkbox("Zoomer Shell (Mario rides shell on zoomer)", &mgr.zoomer_shell);
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Watches Jak's state each frame. When Jak is on the\n"
                     "zoomer (target-racing-* states in Fire Canyon /\n"
                     "Lava Tube / Misty / Rolling / Ogre), forces Mario\n"
                     "into ACT_RIDING_SHELL_GROUND so he surfs alongside.\n"
                     "Native shell-riding immunity also blocks lava-rock\n"
                     "damage in Fire Canyon / Lava Tube.");
  }

  int volume = mgr.get_audio_volume();
  if (ImGui::SliderInt("Mario Volume", &volume, 0, 100, "%d%%")) {
    mgr.set_audio_volume(volume);
  }
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Volume of the SM64 audio stream (music + sfx).");
  }

  // Live Mario scale slider.  Updates three things in lock step:
  //   1. Jak-side SM64_TO_JAK_SCALE / JAK_TO_SM64_SCALE (inline-vars in
  //      libsm64_integration.h) — affects rendered mesh size + every
  //      Jak↔SM64 unit conversion in position sync, collision, etc.
  //   2. libsm64 C-side g_libsm64_mario_scale — feeds the walk/swim
  //      speed caps in mario_actions_moving.c / _submerged.c so Mario's
  //      top speed scales proportionally with his size.
  //   3. (Indirect) Everything downstream that reads the two above.
  // Range [10, 150]: 10 makes Mario huge (feels like giant mode), 150
  // makes him tiny.  Default 50 matches the mario-scale-testing branch.
  float scale = get_mario_scale();
  if (ImGui::SliderFloat("Mario Scale", &scale, 10.0f, 150.0f, "%.1f")) {
    set_mario_scale(scale);
  }
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("SM64-units-per-Jak-meter.  Lower = bigger Mario,\n"
                     "higher = smaller.  Vanilla SM64 uses 43; this project\n"
                     "ships with 50 as the default.  Live-updates everything\n"
                     "(rendered size, walk speed cap, collision scale).");
  }

  // "No slippery Mario" toggle — off by default so Mario behaves like
  // vanilla SM64.  Flipping it does two things atomically:
  //   1. libsm64 C-side: swaps the slope-class normY cutoffs in
  //      mario_floor_is_slippery to looser values so Mario stays on his
  //      feet on Jak's steeper-than-SM64-intended geometry.
  //   2. Jak-side: tells load_level_collision to classify each tri by
  //      pat-mode (wall/ground/obstacle) instead of tagging everything
  //      SURFACE_DEFAULT.  Only takes effect on the NEXT collision
  //      stream (usually a level transition); flipping it at runtime in
  //      the middle of a level leaves the existing tris unreclassified
  //      until the streaming window around Mario rolls over.
  {
    bool no_slip = get_no_slippery_mario();
    if (ImGui::Checkbox("No Slippery Mario", &no_slip)) {
      set_no_slippery_mario(no_slip);
    }
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("OFF (default): vanilla SM64 slope thresholds — any\n"
                       "slope over ~38 deg flips Mario into a slide.\n"
                       "ON: looser thresholds + pat-mode surface mapping so\n"
                       "walls slide (VERY_SLIPPERY) and ground sticks\n"
                       "(NOT_SLIPPERY).  Surface-type change requires a\n"
                       "collision reload (level transition) to apply;\n"
                       "slope-threshold change is immediate.");
    }
  }

  // Experimental normal-aware collision classification.  See libsm64_integration.h
  // for full rationale — tl;dr ~95 % of Jak-labelled walls aren't vertical enough
  // for libsm64 to treat as walls, so tagging them VERY_SLIPPERY (the legacy
  // no-slip behaviour) force-slides Mario constantly.  When this toggle is on
  // we gate the VERY_SLIPPERY tag on the actual triangle normal and also
  // drop degenerate zero-area tris.  Only applies to the next collision
  // stream — reload by crossing a level transition or waiting for the
  // streaming window around Mario to roll over.
  if (ImGui::Checkbox("Test New Collide Toggle", &mgr.test_new_collide_toggle)) {
    // Reload immediately so the new classification shows up without
    // waiting for a level transition.
    size_t tris = reload_level_collision_from_loader(mgr, loader);
    if (tris > 0) {
      lg::info("[libsm64] Test-new-collide toggle={} — reloaded {} triangles",
               mgr.test_new_collide_toggle ? 1 : 0, tris);
    }
  }
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip(
        "EXPERIMENTAL collision-loader supplement.\n"
        "For every pat-mode=WALL tri whose normal is in the configured\n"
        "wall-extrusion window (0.01 < |ny| <= slider below), ADDITIONALLY\n"
        "emits a pair of perfectly vertical triangles forming a wall\n"
        "quad over the source tri's longest XZ edge and Y range.  Fixes\n"
        "the tunneling-at-speed case (Mario's sub-step jumps past the\n"
        "original tilted tri's footprint) while leaving the source tri\n"
        "intact so find_floor still picks it up and Mario slides on its\n"
        "surface as before.  Also drops degenerate (zero-area) tris that\n"
        "can NaN libsm64's find_floor.\n"
        "Flips auto-reload the collision so the change is immediate.\n"
        "Pairs with 'No Slippery Mario' — no effect unless that's also on.");
  }
  ImGui::SliderFloat("Wall Extrusion Max |ny|", &mgr.wall_extrusion_ny_max, 0.01f, 1.0f,
                     "%.3f");
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip(
        "Upper bound on |normal.y| for the wall-extrusion supplement.\n"
        "Tris with `0.01 < |ny| <= value` get replaced with vertical\n"
        "wall quads; tris past this cutoff are treated as slopes and\n"
        "left alone.\n"
        "  0.05  — only walls within ~3° of perfectly vertical.\n"
        "  0.30  — near-vertical walls up to ~72° from horizontal (default).\n"
        "  1.00  — extrude everything Jak calls a wall (risks turning\n"
        "          gentle slopes into tall invisible walls).\n"
        "Release the slider to auto-reload collision with the new value.");
  }
  // Auto-reload when the user lets go of the slider so the new threshold
  // actually shows up in-game without forcing them to cross a level
  // boundary.  IsItemDeactivatedAfterEdit fires exactly once on mouse-
  // release if the value changed during the drag.
  if (ImGui::IsItemDeactivatedAfterEdit()) {
    size_t tris = reload_level_collision_from_loader(mgr, loader);
    if (tris > 0) {
      lg::info("[libsm64] Wall-extrusion cap changed to {:.3f} — reloaded {} triangles",
               mgr.wall_extrusion_ny_max, tris);
    }
  }

  // Cutscene bone tracker.  teleport_mario_to_jak reads
  // `(-> *target* node-list data N bone transform)` when this is >= 0,
  // else falls back to root.trans / root.quat.  Useful eichar indices:
  //   -1 = disabled (root.trans fallback)
  //    1 = align (root-align bone)
  //   26 = hips
  //   29 = Lankle  (default)
  //   33 = Rankle
  // See engine/data/joint-nodes.gc for the full table.
  ImGui::SliderInt("Track Bone (pos)", &g_cutscene_track_bone, -1, 80);
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Bone whose WORLD POSITION Mario is teleported to\n"
                     "during cutscenes.  -1 = *target* root.trans\n"
                     "fallback.  Default 29 (Lankle on eichar).\n"
                     "Handy: 1 = align, 26 = hips, 33 = Rankle.");
  }
  ImGui::SliderInt("Track Bone (rot)", &g_cutscene_track_rot_bone, -1, 80);
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Bone whose ROTATION MATRIX Mario's pitch/yaw/roll\n"
                     "are copied from.  -1 = *target* root.quat fallback.\n"
                     "Default 1 (align) — the character root-align joint\n"
                     "whose forward axis tracks Jak's body facing.\n"
                     "Lankle is a poor rotation source (foot wobbles).");
  }

  // Live readout of the tracked bone's world position vs. Mario's live
  // position.  During gameplay these should roughly coincide with
  // whatever the teleport does; during a cutscene, watching the bone
  // position update (or not) tells you whether *target*'s skeleton is
  // actually being driven by the movie animation or whether it's frozen
  // along with the rest of the process.  If "Bone pos" stops changing
  // when you expect Jak to be walking around, the bone isn't updating
  // and no amount of bone-index tweaking will help — we'd need a
  // different data source entirely (e.g. pov-camera).
  {
    math::Vector3f bone_pos;
    int used_bone = -2;
    const bool have = mgr.read_cutscene_track_position(g_ee_main_mem, &bone_pos, &used_bone);
    if (have) {
      ImGui::Text("Bone pos (idx=%d): (%.1f, %.1f, %.1f)",
                 used_bone, bone_pos.x(), bone_pos.y(), bone_pos.z());
    } else {
      ImGui::TextDisabled("Bone pos: (unavailable — *target* not ready)");
    }
    auto mstate = mgr.get_state();
    ImGui::Text("Mario pos:       (%.1f, %.1f, %.1f)",
               mstate.position.x(), mstate.position.y(), mstate.position.z());
    // Delta helps eyeball whether the teleport is landing Mario where
    // the bone is, or whether something else is immediately pulling him
    // away (e.g. SM64 physics gravity on a non-solid surface).
    if (have) {
      const float dx = bone_pos.x() - mstate.position.x();
      const float dy = bone_pos.y() - mstate.position.y();
      const float dz = bone_pos.z() - mstate.position.z();
      const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
      ImGui::Text("Delta |bone - mario|: %.1f Jak units", d);
    }

    // Gate state + teleport rate.  Pipeline runs the teleport only when
    // (target_grabbed && !target_periscope && !target_clone_anim) AND
    // (!paused || in_movie).  The Calls/sec counter tells you whether
    // the function is actually being invoked each frame — if it's 0
    // during a cutscene, the gate is blocking despite appearances.
    ImGui::Text("Gate: grabbed=%d periscope=%d clone-anim=%d in_movie=%d",
                mgr.target_grabbed ? 1 : 0,
                mgr.target_periscope ? 1 : 0,
                mgr.target_clone_anim ? 1 : 0,
                mgr.target_in_movie ? 1 : 0);
    ImGui::Text("Mode: paused=%d  (movie override: %d)",
                mgr.is_game_paused(g_ee_main_mem) ? 1 : 0,
                mgr.target_in_movie ? 1 : 0);
    // Compute calls-per-second by diffing the monotonic counter against
    // its value one second ago.  Rough sample — at 30 Hz sim rate a
    // continuously-teleporting cutscene should read 30.
    static uint32_t s_last_count = 0;
    static double s_last_time = ImGui::GetTime();
    const double now = ImGui::GetTime();
    static float s_cached_rate = 0.0f;
    if (now - s_last_time >= 0.5) {
      const uint32_t cur = mgr.teleport_call_count();
      s_cached_rate = static_cast<float>((cur - s_last_count) / (now - s_last_time));
      s_last_count = cur;
      s_last_time = now;
    }
    ImGui::Text("teleport_mario_to_jak: %u total, %.1f calls/sec",
                mgr.teleport_call_count(), s_cached_rate);
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

      // Wireframe overlay + OBJ/CSV dump — both hang off the same snapshot of
      // m_all_static_surfaces in the manager, so toggling the overlay and
      // pressing Dump work even when Mario isn't spawned (as long as a level
      // has been loaded and streamed once).
      ImGui::Checkbox("Show Collision Overlay", &mgr.show_collision);
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Draw every static collision triangle libsm64 currently knows about\n"
            "as a coloured wireframe (one colour per SURFACE_* type).  Useful for\n"
            "spotting mismatches between what Mario walks on and what Jak's\n"
            "collision system exposes — if you see holes in the wireframe where\n"
            "Jak has solid ground, that's the cause of 'Mario falls through' bugs.\n"
            "No-op cost when unchecked.");
      }
      if (ImGui::Button("Dump Surfaces to File")) {
        m_last_dump_path = dump_surfaces_to_files();
      }
      ImGui::SameLine();
      ImGui::TextDisabled("(?)");
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Write the same triangle set shown by 'Show Collision Overlay' to\n"
            "a timestamped .obj + .csv pair under the user misc dir (on Windows:\n"
            "%%APPDATA%%\\OpenGOAL\\jak1\\misc\\sm64_collision_dumps\\).\n"
            "The .obj loads straight into Blender/MeshLab for eyeballing; the\n"
            "sidecar .csv has one row per triangle with type/force/terrain for\n"
            "grepping.  Positions are in SM64 units — multiply by "
            "SM64_TO_JAK_SCALE\nfor Jak units.");
      }
      if (!m_last_dump_path.empty()) {
        ImGui::TextWrapped("Last dump: %s", m_last_dump_path.c_str());
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
        ImGui::SameLine();
        if (ImGui::Button("Spawn Mario at Target Position")) {
          math::Vector3f tpos;
          float tyaw = 0.f;
          if (mgr.read_target_transform(g_ee_main_mem, &tpos, &tyaw)) {
            int32_t id = spawn_mario_with_collision_fallback(mgr, loader,
                                                              tpos.x(), tpos.y(), tpos.z());
            if (id >= 0) {
              mgr.set_mario_face_angle(tyaw);
            }
          } else {
            lg::warn("[libsm64] Spawn at target: *target* not found");
          }
        }
      } else {
        if (ImGui::Button("Delete Mario")) {
          mgr.delete_mario(mgr.get_mario_id());
        }
        ImGui::SameLine();
        if (ImGui::Button("Respawn at Target Position")) {
          math::Vector3f tpos;
          float tyaw = 0.f;
          if (mgr.read_target_transform(g_ee_main_mem, &tpos, &tyaw)) {
            mgr.delete_mario(mgr.get_mario_id());
            int32_t id = spawn_mario_with_collision_fallback(mgr, loader,
                                                              tpos.x(), tpos.y(), tpos.z());
            if (id >= 0) {
              mgr.set_mario_face_angle(tyaw);
            }
          } else {
            lg::warn("[libsm64] Respawn at target: *target* not found");
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
