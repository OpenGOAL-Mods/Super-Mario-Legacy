#include "mario_debug_gui.h"

#include "third-party/imgui/imgui.h"

namespace sm64 {

bool bool_val = false;
char name_var[128] = "fred";
int volume_amount = 23;


void SM64DebugGui::draw(std::shared_ptr<Loader> loader) {
  if (!m_menu_visible)
    return;

  ImGui::SetNextWindowSize(ImVec2(400, 300), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("Mario Debug", &m_menu_visible)) {
    ImGui::End();
    return;
  }

  ImGui::Text("Hello from the Mario debug window!");
  ImGui::Separator();

  if (ImGui::Button("Test Button")) {
    // do something
  }
  ImGui::Separator();
  //TODO replace 2/3/4 with marios actual postion
  ImGui::Text("Marios position is (%.1f, %.1f, %.1f)", 2.0, 3.0, 4.0 );
  ImGui::Separator();
  ImGui::Checkbox("Example checkbox", &bool_val);
  if (ImGui::IsItemHovered()){

    ImGui::SetTooltip("hover over this if ur fat");
  }
  ImGui::Separator();
  ImGui::InputText("Enter ur name", name_var, sizeof(name_var));
  ImGui::Separator();
  if (ImGui::SliderInt("title", &volume_amount, 0 , 100, "%d%%")) {
    //update volume_amount here

  };

  ImGui::End();
}

}  // namespace sm64
