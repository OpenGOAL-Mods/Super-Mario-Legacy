#pragma once

/*!
 * @file sm64_collision_renderer.h
 * Debug OpenGL overlay that draws the static collision triangle set that
 * libsm64 currently holds, so you can compare "what Mario sees" against
 * what Jak's collision system sees.  Built on top of the MARIO_SM64
 * shader (negative UVs → vertex-colour path) to avoid adding a new
 * shader pair just for this.  Gated by `LibSM64Manager::show_collision`;
 * when off, the render() call is essentially free.
 *
 * There's also a one-shot dump function (see sm64_debug_gui.cpp) that
 * writes the same snapshot out to an .obj + .csv pair for inspection
 * in Blender / MeshLab / grep.  That lives in the debug GUI rather
 * than here because it needs file I/O helpers and a dialog, not GPU.
 */

#include <cstdint>

#include "common/common_types.h"

#include "third-party/glad/include/glad/glad.h"

class ShaderLibrary;

namespace sm64 {

class SM64CollisionRenderer {
 public:
  SM64CollisionRenderer();
  ~SM64CollisionRenderer();

  // Create VAO/VBOs.  Must be called on the render thread with a live
  // GL context.  Idempotent.
  void init(ShaderLibrary& shaders);

  // Draw the collision overlay.  No-op if the renderer is disabled,
  // libsm64 isn't initialised, or there are no surfaces.  Assumes the
  // caller has already activated MARIO_SM64 shader and set its standard
  // uniforms; we only set our own per-draw state (GL flags, polygon
  // mode) and restore afterwards.  Takes the Jak camera uniforms so it
  // can re-set them (MarioRenderer leaves them bound, but other renderers
  // might run between the two in the future).
  void render(const float* camera_matrix,
              const float* hvdf_offset,
              const float* camera_pos,
              float fog_constant);

  bool is_initialized() const { return m_initialized; }

 private:
  // Pull a fresh snapshot from LibSM64Manager and push it into the GPU
  // buffers.  Called from render() only when the manager's version
  // counter has advanced, to avoid per-frame copies.
  void rebuild_buffers();

  bool m_initialized = false;

  GLuint m_vao = 0;
  GLuint m_vbo_position = 0;
  GLuint m_vbo_normal = 0;
  GLuint m_vbo_color = 0;
  GLuint m_vbo_uv = 0;  // always {-1,-1} so the MARIO_SM64 shader skips texture lookup.

  int m_vertex_count = 0;           // 3 * tri count
  uint64_t m_uploaded_version = 0;  // last LibSM64Manager::static_surfaces_version() we uploaded
};

}  // namespace sm64
