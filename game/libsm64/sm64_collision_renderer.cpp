/*!
 * @file sm64_collision_renderer.cpp
 * See sm64_collision_renderer.h for design notes.
 */

#include "sm64_collision_renderer.h"

#include <cmath>
#include <vector>

#include "common/log/log.h"

#include "game/graphics/opengl_renderer/Shader.h"
#include "game/graphics/pipelines/opengl.h"
#include "game/libsm64/libsm64_integration.h"

namespace sm64 {

// SM64 -> Jak units.  libsm64 stores static surface positions in SM64
// integer units (see load_level_collision), but our Jak camera transform
// in MARIO_SM64.vert consumes Jak units (the same space Mario's mesh is
// already in).  Multiplying by SM64_TO_JAK_SCALE converts.
static inline float sm64_to_jak(float v) {
  return v * SM64_TO_JAK_SCALE;
}

// Colour palette indexed by SURFACE_* type.  The values here are the
// common Jak-side classifications plus a few SM64-specific ones we
// actually stream in.  Anything not listed falls through to the
// default (mid-grey) below.
static void surface_color(int16_t type, float out[3]) {
  // RGB, roughly desaturated so wireframe-over-fill stays readable.
  switch (type) {
    case 0x0000: out[0] = 0.80f; out[1] = 0.80f; out[2] = 0.80f; return;  // DEFAULT — grey
    case 0x0001: out[0] = 1.00f; out[1] = 0.30f; out[2] = 0.10f; return;  // BURNING — orange-red
    case 0x0005: out[0] = 0.70f; out[1] = 0.40f; out[2] = 0.90f; return;  // HANGABLE
    case 0x000A: out[0] = 0.20f; out[1] = 0.00f; out[2] = 0.00f; return;  // DEATH_PLANE — dark red
    case 0x000D: out[0] = 0.20f; out[1] = 0.50f; out[2] = 0.95f; return;  // WATER — blue
    case 0x000E: out[0] = 0.30f; out[1] = 0.60f; out[2] = 1.00f; return;  // FLOWING_WATER — bright blue
    case 0x0013: out[0] = 1.00f; out[1] = 0.10f; out[2] = 0.10f; return;  // VERY_SLIPPERY — red (usually walls)
    case 0x0014: out[0] = 1.00f; out[1] = 0.80f; out[2] = 0.10f; return;  // SLIPPERY — yellow
    case 0x0015: out[0] = 0.20f; out[1] = 0.90f; out[2] = 0.20f; return;  // NOT_SLIPPERY — green (ground)
    case 0x002E: out[0] = 0.70f; out[1] = 0.95f; out[2] = 1.00f; return;  // ICE — cyan
    case 0x0030: out[0] = 0.55f; out[1] = 0.55f; out[2] = 0.55f; return;  // HARD — dark grey
    default:
      // Unknown — bright magenta so misclassified geometry is obvious.
      out[0] = 1.00f; out[1] = 0.10f; out[2] = 1.00f; return;
  }
}

SM64CollisionRenderer::SM64CollisionRenderer() = default;

SM64CollisionRenderer::~SM64CollisionRenderer() {
  if (m_vao) glDeleteVertexArrays(1, &m_vao);
  if (m_vbo_position) glDeleteBuffers(1, &m_vbo_position);
  if (m_vbo_normal) glDeleteBuffers(1, &m_vbo_normal);
  if (m_vbo_color) glDeleteBuffers(1, &m_vbo_color);
  if (m_vbo_uv) glDeleteBuffers(1, &m_vbo_uv);
}

void SM64CollisionRenderer::init(ShaderLibrary& /*shaders*/) {
  if (m_initialized) return;

  glGenVertexArrays(1, &m_vao);
  glGenBuffers(1, &m_vbo_position);
  glGenBuffers(1, &m_vbo_normal);
  glGenBuffers(1, &m_vbo_color);
  glGenBuffers(1, &m_vbo_uv);

  // Layout matches MARIO_SM64.vert: 0=pos, 1=normal, 2=color, 3=uv.
  glBindVertexArray(m_vao);

  glBindBuffer(GL_ARRAY_BUFFER, m_vbo_position);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, nullptr);

  glBindBuffer(GL_ARRAY_BUFFER, m_vbo_normal);
  glEnableVertexAttribArray(1);
  glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 0, nullptr);

  glBindBuffer(GL_ARRAY_BUFFER, m_vbo_color);
  glEnableVertexAttribArray(2);
  glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, 0, nullptr);

  glBindBuffer(GL_ARRAY_BUFFER, m_vbo_uv);
  glEnableVertexAttribArray(3);
  glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, 0, nullptr);

  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glBindVertexArray(0);

  m_initialized = true;
  lg::info("[SM64CollisionRenderer] Initialized");
}

void SM64CollisionRenderer::rebuild_buffers() {
  auto& mgr = LibSM64Manager::instance();
  auto tris = mgr.snapshot_static_surfaces();
  m_vertex_count = static_cast<int>(tris.size()) * 3;
  if (m_vertex_count == 0) {
    // Still orphan-upload an empty buffer so the draw call is safe.
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo_position);
    glBufferData(GL_ARRAY_BUFFER, 0, nullptr, GL_STATIC_DRAW);
    m_uploaded_version = mgr.static_surfaces_version();
    return;
  }

  // Pack: 3 verts per tri, per-tri colour (from type) replicated across
  // all three verts, per-tri normal (cross product of edges), per-vert
  // UV set to (-1,-1) so the Mario fragment shader takes the vertex-
  // colour path and skips the texture lookup.
  std::vector<float> pos;
  std::vector<float> nrm;
  std::vector<float> col;
  std::vector<float> uv;
  pos.reserve(m_vertex_count * 3);
  nrm.reserve(m_vertex_count * 3);
  col.reserve(m_vertex_count * 3);
  uv.reserve(m_vertex_count * 2);

  for (const auto& t : tris) {
    // World positions (SM64 units -> Jak units).
    float p0[3] = {sm64_to_jak(t.verts[0][0]), sm64_to_jak(t.verts[0][1]), sm64_to_jak(t.verts[0][2])};
    float p1[3] = {sm64_to_jak(t.verts[1][0]), sm64_to_jak(t.verts[1][1]), sm64_to_jak(t.verts[1][2])};
    float p2[3] = {sm64_to_jak(t.verts[2][0]), sm64_to_jak(t.verts[2][1]), sm64_to_jak(t.verts[2][2])};

    // Face normal (unnormalised -> normalise).  SM64 surface winding
    // follows the SM64 convention; if it comes out backwards the shader
    // still picks the brighter of the two shading sides via abs-less
    // clamp in mario_sm64.vert (which max()s against 0), so we just
    // use whichever direction the cross gives us.
    float e1[3] = {p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]};
    float e2[3] = {p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2]};
    float n[3] = {e1[1] * e2[2] - e1[2] * e2[1],
                  e1[2] * e2[0] - e1[0] * e2[2],
                  e1[0] * e2[1] - e1[1] * e2[0]};
    float nl = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    if (nl > 1e-6f) {
      n[0] /= nl; n[1] /= nl; n[2] /= nl;
    } else {
      n[0] = 0.0f; n[1] = 1.0f; n[2] = 0.0f;
    }

    float c[3];
    surface_color(t.type, c);

    for (int v = 0; v < 3; v++) {
      const float* p = (v == 0) ? p0 : (v == 1) ? p1 : p2;
      pos.push_back(p[0]);
      pos.push_back(p[1]);
      pos.push_back(p[2]);
      nrm.push_back(n[0]);
      nrm.push_back(n[1]);
      nrm.push_back(n[2]);
      col.push_back(c[0]);
      col.push_back(c[1]);
      col.push_back(c[2]);
      uv.push_back(-1.0f);
      uv.push_back(-1.0f);
    }
  }

  glBindBuffer(GL_ARRAY_BUFFER, m_vbo_position);
  glBufferData(GL_ARRAY_BUFFER, pos.size() * sizeof(float), pos.data(), GL_STATIC_DRAW);
  glBindBuffer(GL_ARRAY_BUFFER, m_vbo_normal);
  glBufferData(GL_ARRAY_BUFFER, nrm.size() * sizeof(float), nrm.data(), GL_STATIC_DRAW);
  glBindBuffer(GL_ARRAY_BUFFER, m_vbo_color);
  glBufferData(GL_ARRAY_BUFFER, col.size() * sizeof(float), col.data(), GL_STATIC_DRAW);
  glBindBuffer(GL_ARRAY_BUFFER, m_vbo_uv);
  glBufferData(GL_ARRAY_BUFFER, uv.size() * sizeof(float), uv.data(), GL_STATIC_DRAW);
  glBindBuffer(GL_ARRAY_BUFFER, 0);

  m_uploaded_version = mgr.static_surfaces_version();
  lg::info("[SM64CollisionRenderer] Uploaded {} triangles (version {})",
           m_vertex_count / 3, static_cast<uint64_t>(m_uploaded_version));
}

void SM64CollisionRenderer::render(const float* camera_matrix,
                                    const float* hvdf_offset,
                                    const float* /*camera_pos*/,
                                    float fog_constant) {
  auto& mgr = LibSM64Manager::instance();
  if (!mgr.show_collision || !mgr.is_initialized() || !m_initialized) return;

  // Re-upload if the static surface list has changed since we last mirrored it.
  if (mgr.static_surfaces_version() != m_uploaded_version) {
    rebuild_buffers();
  }
  if (m_vertex_count == 0) return;

  // Activate MARIO_SM64 shader implicitly — the caller handles activation
  // (render_mario_sm64 does it once per frame before MarioRenderer draws,
  // and we reuse that binding).  Our uniforms are scoped to this shader.
  GLint program = 0;
  glGetIntegerv(GL_CURRENT_PROGRAM, &program);
  if (program == 0) {
    // Shader not bound — bail; caller is expected to activate MARIO_SM64.
    return;
  }
  glUniformMatrix4fv(glGetUniformLocation(program, "camera"), 1, GL_FALSE, camera_matrix);
  glUniform4fv(glGetUniformLocation(program, "hvdf_offset"), 1, hvdf_offset);
  glUniform1f(glGetUniformLocation(program, "fog_constant"), fog_constant);
  // Flat-ish lighting so slopes read differently from flat ground.
  float light_dir[3] = {0.5f, 0.8f, 0.3f};
  float ll = std::sqrt(light_dir[0] * light_dir[0] + light_dir[1] * light_dir[1] + light_dir[2] * light_dir[2]);
  light_dir[0] /= ll; light_dir[1] /= ll; light_dir[2] /= ll;
  glUniform3fv(glGetUniformLocation(program, "light_dir"), 1, light_dir);
  // We don't bind a texture — the negative UVs route the shader through
  // the vertex-colour branch, so tex_T0 is untouched.  Still has to
  // resolve to something though, so point it at unit 0.
  glUniform1i(glGetUniformLocation(program, "tex_T0"), 0);

  // Two passes: translucent filled + opaque wireframe on top.
  //
  //   Pass 1 (filled, alpha=0.35): depth-test on so near tris correctly
  //   hide far tris; depth-write OFF so the fill doesn't occlude Jak's
  //   world geometry that renders later in the same bucket (the fill's
  //   alpha handles that on the colour side, but without `glDepthMask
  //   (GL_FALSE)` the depth buffer would still win for anything depth-
  //   tested against it).
  //
  //   Pass 2 (wireframe, alpha=1.0): keeps edges crisp on top of the
  //   fill.  Same depth-test setup.
  //
  // The `alpha` uniform on the MARIO_SM64 shader does the heavy lifting
  // — the filled pass writes a translucent pixel that blends against
  // whatever the framebuffer already had (which includes Jak's world
  // from earlier buckets).
  glEnable(GL_DEPTH_TEST);
  glDepthFunc(GL_GEQUAL);  // Jak reverse-Z
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  glDepthMask(GL_FALSE);

  glBindVertexArray(m_vao);

  // Filled translucent pass.
  glUniform1f(glGetUniformLocation(program, "alpha"), 0.35f);
  glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
  glDrawArrays(GL_TRIANGLES, 0, m_vertex_count);

  // Wireframe opaque pass.
  glUniform1f(glGetUniformLocation(program, "alpha"), 1.0f);
  glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
  glDrawArrays(GL_TRIANGLES, 0, m_vertex_count);
  glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

  glBindVertexArray(0);

  // Restore shader state so whoever renders after us (MarioRenderer on
  // subsequent frames reuses the same program) sees the default.
  glUniform1f(glGetUniformLocation(program, "alpha"), 1.0f);
  glDepthMask(GL_TRUE);
}

}  // namespace sm64
