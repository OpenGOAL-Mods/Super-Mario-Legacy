#pragma once

/*!
 * @file sm64_debug_gui.h
 * ImGui debug window for controlling the libsm64 integration.
 */

#include <memory>
#include <string>

class Loader;

namespace sm64 {

class SM64DebugGui {
 public:
  void draw(std::shared_ptr<Loader> loader);
  bool is_visible() const { return m_visible; }
  void set_visible(bool visible) { m_visible = visible; }

 private:
  bool m_visible = false;
  char m_rom_path[512] = "";  // override; leave empty to use the auto-detected ROM
  float m_spawn_pos[3] = {0.0f, 10.0f, 0.0f};
  float m_ground_y = 0.0f;
  float m_ground_extent = 500.0f;
  // Last "Dump Surfaces" target, shown under the button so users can
  // find the file without tailing the log.  Empty = not dumped yet (or
  // the last attempt failed).
  std::string m_last_dump_path;
};

}  // namespace sm64
