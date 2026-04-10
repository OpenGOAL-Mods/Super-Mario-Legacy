#pragma once

/*!
 * @file MarioRenderer.h
 * OpenGL renderer for the libsm64 Mario model.
 * Renders Mario's dynamic mesh using the texture atlas and geometry
 * output from sm64_mario_tick().
 */

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

  bool m_initialized = false;
  bool m_texture_uploaded = false;

  GLuint m_vao = 0;
  GLuint m_vbo_position = 0;
  GLuint m_vbo_normal = 0;
  GLuint m_vbo_color = 0;
  GLuint m_vbo_uv = 0;
  GLuint m_texture = 0;

  uint16_t m_num_triangles = 0;
};

}  // namespace sm64
