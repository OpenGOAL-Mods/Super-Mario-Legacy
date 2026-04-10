/*!
 * @file MarioRenderer.cpp
 * OpenGL renderer for Mario from libsm64.
 */

#include "MarioRenderer.h"

#include "common/log/log.h"

#include "game/graphics/pipelines/opengl.h"

namespace sm64 {

MarioRenderer::MarioRenderer() = default;

MarioRenderer::~MarioRenderer() {
  if (m_vao) glDeleteVertexArrays(1, &m_vao);
  if (m_vbo_position) glDeleteBuffers(1, &m_vbo_position);
  if (m_vbo_normal) glDeleteBuffers(1, &m_vbo_normal);
  if (m_vbo_color) glDeleteBuffers(1, &m_vbo_color);
  if (m_vbo_uv) glDeleteBuffers(1, &m_vbo_uv);
  if (m_texture) glDeleteTextures(1, &m_texture);
}

void MarioRenderer::init(ShaderLibrary& shaders) {
  if (m_initialized) return;

  // Create VAO and VBOs
  glGenVertexArrays(1, &m_vao);
  glGenBuffers(1, &m_vbo_position);
  glGenBuffers(1, &m_vbo_normal);
  glGenBuffers(1, &m_vbo_color);
  glGenBuffers(1, &m_vbo_uv);
  glGenTextures(1, &m_texture);

  glBindVertexArray(m_vao);

  // Position (location 0)
  glBindBuffer(GL_ARRAY_BUFFER, m_vbo_position);
  glBufferData(GL_ARRAY_BUFFER, 9 * GEO_MAX_TRIANGLES * sizeof(float), nullptr, GL_DYNAMIC_DRAW);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, nullptr);

  // Normal (location 1)
  glBindBuffer(GL_ARRAY_BUFFER, m_vbo_normal);
  glBufferData(GL_ARRAY_BUFFER, 9 * GEO_MAX_TRIANGLES * sizeof(float), nullptr, GL_DYNAMIC_DRAW);
  glEnableVertexAttribArray(1);
  glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 0, nullptr);

  // Color (location 2)
  glBindBuffer(GL_ARRAY_BUFFER, m_vbo_color);
  glBufferData(GL_ARRAY_BUFFER, 9 * GEO_MAX_TRIANGLES * sizeof(float), nullptr, GL_DYNAMIC_DRAW);
  glEnableVertexAttribArray(2);
  glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, 0, nullptr);

  // UV (location 3)
  glBindBuffer(GL_ARRAY_BUFFER, m_vbo_uv);
  glBufferData(GL_ARRAY_BUFFER, 6 * GEO_MAX_TRIANGLES * sizeof(float), nullptr, GL_DYNAMIC_DRAW);
  glEnableVertexAttribArray(3);
  glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, 0, nullptr);

  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glBindVertexArray(0);

  m_initialized = true;
  lg::info("[MarioRenderer] Initialized");
}

void MarioRenderer::upload_texture() {
  auto& mgr = LibSM64Manager::instance();
  if (!mgr.is_initialized()) return;

  glBindTexture(GL_TEXTURE_2D, m_texture);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, mgr.get_texture_width(), mgr.get_texture_height(), 0,
               GL_RGBA, GL_UNSIGNED_BYTE, mgr.get_texture_data());
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glBindTexture(GL_TEXTURE_2D, 0);

  m_texture_uploaded = true;
  lg::info("[MarioRenderer] Texture atlas uploaded ({}x{})",
           mgr.get_texture_width(), mgr.get_texture_height());
}

void MarioRenderer::update_geometry(const MarioGeometry& geo) {
  m_num_triangles = geo.num_triangles;
  if (m_num_triangles == 0) return;

  int num_verts = m_num_triangles * 3;

  glBindBuffer(GL_ARRAY_BUFFER, m_vbo_position);
  glBufferSubData(GL_ARRAY_BUFFER, 0, num_verts * 3 * sizeof(float), geo.position.data());

  glBindBuffer(GL_ARRAY_BUFFER, m_vbo_normal);
  glBufferSubData(GL_ARRAY_BUFFER, 0, num_verts * 3 * sizeof(float), geo.normal.data());

  glBindBuffer(GL_ARRAY_BUFFER, m_vbo_color);
  glBufferSubData(GL_ARRAY_BUFFER, 0, num_verts * 3 * sizeof(float), geo.color.data());

  glBindBuffer(GL_ARRAY_BUFFER, m_vbo_uv);
  glBufferSubData(GL_ARRAY_BUFFER, 0, num_verts * 2 * sizeof(float), geo.uv.data());

  glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void MarioRenderer::render(const float* camera_matrix,
                            const float* hvdf_offset,
                            const float* camera_pos,
                            float fog_constant) {
  auto& mgr = LibSM64Manager::instance();
  if (!mgr.is_initialized() || !mgr.has_mario() || !m_initialized) return;

  // Upload texture on first render
  if (!m_texture_uploaded) {
    upload_texture();
  }

  // Get latest geometry from the SM64 tick
  auto geo = mgr.get_geometry();
  if (geo.num_triangles == 0) return;

  update_geometry(geo);

  // Set up GL state
  glEnable(GL_DEPTH_TEST);
  glDepthFunc(GL_GEQUAL);  // Jak uses reversed-Z
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

  // Bind texture
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, m_texture);

  // Get current shader program (MARIO_SM64 set by caller)
  GLint program;
  glGetIntegerv(GL_CURRENT_PROGRAM, &program);

  // Set uniforms
  glUniformMatrix4fv(glGetUniformLocation(program, "camera"), 1, GL_FALSE, camera_matrix);
  glUniform4fv(glGetUniformLocation(program, "hvdf_offset"), 1, hvdf_offset);
  glUniform1f(glGetUniformLocation(program, "fog_constant"), fog_constant);
  glUniform1i(glGetUniformLocation(program, "tex_T0"), 0);

  // Light direction (simple sun-like, normalized)
  float light_dir[3] = {0.5f, 0.8f, 0.3f};
  float len = sqrtf(light_dir[0]*light_dir[0] + light_dir[1]*light_dir[1] + light_dir[2]*light_dir[2]);
  light_dir[0] /= len; light_dir[1] /= len; light_dir[2] /= len;
  glUniform3fv(glGetUniformLocation(program, "light_dir"), 1, light_dir);

  // Draw
  glBindVertexArray(m_vao);
  glDrawArrays(GL_TRIANGLES, 0, m_num_triangles * 3);
  glBindVertexArray(0);

  glBindTexture(GL_TEXTURE_2D, 0);
}

}  // namespace sm64
