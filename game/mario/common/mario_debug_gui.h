#pragma once

#include <memory>

class Loader;

namespace sm64 {

class SM64DebugGui {
 public:
  void draw(std::shared_ptr<Loader> loader);
  bool is_visible() const { return m_menu_visible; }
  void set_visible(bool visible) { m_menu_visible = visible; }

 private:
  bool m_menu_visible = false;
};

}  // namespace sm64
