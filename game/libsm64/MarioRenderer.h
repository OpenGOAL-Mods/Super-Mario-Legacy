#pragma once

/*!
 * @file MarioRenderer.h
 * OpenGL renderer for the libsm64 Mario model.
 * Renders Mario's dynamic mesh using the texture atlas and geometry
 * output from sm64_mario_tick().
 * Also renders a procedural Koopa shell under Mario when he's in
 * ACT_RIDING_SHELL_GROUND (the zoomer-shell feature).
 */

#include <vector>

#include "common/common_types.h"

#include "game/graphics/opengl_renderer/Shader.h"
#include "game/libsm64/libsm64_integration.h"

#include "third-party/glad/include/glad/glad.h"

namespace sm64 {

class MarioRenderer {
 public:
  MarioRenderer();
  ~MarioRenderer();

  void init(ShaderLibrary& shaders);
  void render(const float* camera_matrix,
              const float* hvdf_offset,
              const float* camera_pos,
              float fog_constant);

  bool is_initialized() const { return m_initialized; }

 private:
  void upload_texture();
  void update_geometry(const MarioGeometry& geo);

  // Shell rendering helpers.
  void build_shell_local_mesh();
  void render_shell(const MarioState& state);

  bool m_initialized = false;
  bool m_texture_uploaded = false;

  // Mario mesh GL objects
  GLuint m_vao = 0;
  GLuint m_vbo_position = 0;
  GLuint m_vbo_normal = 0;
  GLuint m_vbo_color = 0;
  GLuint m_vbo_uv = 0;
  GLuint m_texture = 0;

  uint16_t m_num_triangles = 0;

  // Shell mesh GL objects (separate VAO so we can draw independently)
  GLuint m_shell_vao = 0;
  GLuint m_shell_vbo_position = 0;
  GLuint m_shell_vbo_normal = 0;
  GLuint m_shell_vbo_color = 0;
  GLuint m_shell_vbo_uv = 0;

  GLuint m_shell_texture = 0;

  // Pre-built shell mesh in local space (origin = shell center, Y up).
  // Positions and normals are in Jak units; color is RGB vertex color.
  // Transform to world each frame via Mario's position + face_angle.
  struct ShellVertex {
    float px, py, pz;   // local position
    float nx, ny, nz;   // normal
    float cr, cg, cb;   // color
    float u, v;          // texture coords (positive = textured dome, negative = vertex colour)
  };
  std::vector<ShellVertex> m_shell_local;   // 3 verts per tri
  int m_shell_tri_count = 0;
};

}  // namespace sm64
