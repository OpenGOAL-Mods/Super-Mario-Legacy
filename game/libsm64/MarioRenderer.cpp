/*!
 * @file MarioRenderer.cpp
 * OpenGL renderer for Mario from libsm64, plus the authentic SM64 Koopa
 * shell mesh drawn when Mario is in ACT_RIDING_SHELL_GROUND.
 *
 * The shell model is extracted at runtime from the SM64 ROM's compressed
 * actor segment (see LibSM64Manager::extract_shell_from_rom) — no
 * copyrighted vertex/texture data is stored in the source tree.
 */

#include "MarioRenderer.h"

#include <cmath>

#include "common/log/log.h"

#include "game/graphics/pipelines/opengl.h"

namespace sm64 {

// ACT_FLAG_RIDING_SHELL from sm64.h — any action with this bit means "on shell".
static constexpr uint32_t kActFlagRidingShell = 0x00010000;

// GEO_SCALE(16384) = 16384/65536 = 0.25, from koopa_shell_geo in SM64.
// The shell model's vertex positions are 4× larger than they appear in-game.
static constexpr float kGeoScale = 16384.0f / 65536.0f;

// ---------------------------------------------------------------------------

MarioRenderer::MarioRenderer() = default;

MarioRenderer::~MarioRenderer() {
  if (m_vao) glDeleteVertexArrays(1, &m_vao);
  if (m_vbo_position) glDeleteBuffers(1, &m_vbo_position);
  if (m_vbo_normal) glDeleteBuffers(1, &m_vbo_normal);
  if (m_vbo_color) glDeleteBuffers(1, &m_vbo_color);
  if (m_vbo_uv) glDeleteBuffers(1, &m_vbo_uv);
  if (m_texture) glDeleteTextures(1, &m_texture);

  destroy_corpse_meshes();

  if (m_shell_vao) glDeleteVertexArrays(1, &m_shell_vao);
  if (m_shell_vbo_position) glDeleteBuffers(1, &m_shell_vbo_position);
  if (m_shell_vbo_normal) glDeleteBuffers(1, &m_shell_vbo_normal);
  if (m_shell_vbo_color) glDeleteBuffers(1, &m_shell_vbo_color);
  if (m_shell_vbo_uv) glDeleteBuffers(1, &m_shell_vbo_uv);
  if (m_shell_texture) glDeleteTextures(1, &m_shell_texture);
}

void MarioRenderer::build_shell_local_mesh() {
  auto& mgr = LibSM64Manager::instance();
  const auto& shell = mgr.get_shell_mesh();

  if (!shell.valid || shell.tri_count == 0) {
    lg::warn("[MarioRenderer] No shell mesh data from ROM — shell will not render");
    m_shell_tri_count = 0;
    return;
  }

  const float scale = SM64_TO_JAK_SCALE * kGeoScale;

  m_shell_local.clear();
  m_shell_local.reserve(shell.vertices.size());

  for (const auto& v : shell.vertices) {
    ShellVertex sv;
    sv.px = v.px * scale;
    sv.py = v.py * scale;
    sv.pz = v.pz * scale;
    sv.nx = v.nx;
    sv.ny = v.ny;
    sv.nz = v.nz;
    sv.cr = v.cr;
    sv.cg = v.cg;
    sv.cb = v.cb;
    sv.u  = v.u;
    sv.v  = v.v;
    m_shell_local.push_back(sv);
  }

  m_shell_tri_count = shell.tri_count;
  lg::info("[MarioRenderer] Shell mesh from ROM: {} triangles, {} vertices",
           m_shell_tri_count, (int)m_shell_local.size());
}

void MarioRenderer::init(ShaderLibrary& shaders) {
  if (m_initialized) return;

  // Mario VAO and VBOs
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

  // Corpse meshes are allocated lazily in rebuild_corpse_meshes() — one
  // VAO + 4 VBOs per captured death, sized exactly for that corpse's tri
  // count.  No pre-allocation here.

  // ---- Shell VAO and VBOs (from ROM-extracted mesh) -----------------------
  build_shell_local_mesh();

  if (m_shell_tri_count > 0) {
    int sv = m_shell_tri_count * 3;  // shell vertex count

    glGenVertexArrays(1, &m_shell_vao);
    glGenBuffers(1, &m_shell_vbo_position);
    glGenBuffers(1, &m_shell_vbo_normal);
    glGenBuffers(1, &m_shell_vbo_color);
    glGenBuffers(1, &m_shell_vbo_uv);

    glBindVertexArray(m_shell_vao);

    glBindBuffer(GL_ARRAY_BUFFER, m_shell_vbo_position);
    glBufferData(GL_ARRAY_BUFFER, sv * 3 * sizeof(float), nullptr, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, nullptr);

    glBindBuffer(GL_ARRAY_BUFFER, m_shell_vbo_normal);
    glBufferData(GL_ARRAY_BUFFER, sv * 3 * sizeof(float), nullptr, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 0, nullptr);

    glBindBuffer(GL_ARRAY_BUFFER, m_shell_vbo_color);
    glBufferData(GL_ARRAY_BUFFER, sv * 3 * sizeof(float), nullptr, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, 0, nullptr);

    // UVs — dome vertices carry real texture coords, belly/ring are negative.
    {
      std::vector<float> uv(sv * 2);
      for (int i = 0; i < sv; ++i) {
        uv[i * 2 + 0] = m_shell_local[i].u;
        uv[i * 2 + 1] = m_shell_local[i].v;
      }
      glBindBuffer(GL_ARRAY_BUFFER, m_shell_vbo_uv);
      glBufferData(GL_ARRAY_BUFFER, uv.size() * sizeof(float), uv.data(), GL_STATIC_DRAW);
      glEnableVertexAttribArray(3);
      glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    }

    // Static colour buffer.
    {
      std::vector<float> col(sv * 3);
      for (int i = 0; i < sv; ++i) {
        col[i * 3 + 0] = m_shell_local[i].cr;
        col[i * 3 + 1] = m_shell_local[i].cg;
        col[i * 3 + 2] = m_shell_local[i].cb;
      }
      glBindBuffer(GL_ARRAY_BUFFER, m_shell_vbo_color);
      glBufferSubData(GL_ARRAY_BUFFER, 0, col.size() * sizeof(float), col.data());
    }

    // Shell texture (extracted from ROM, RGBA16 → RGBA8888).
    auto& mgr = LibSM64Manager::instance();
    const auto& shell = mgr.get_shell_mesh();
    if (shell.valid && !shell.texture_rgba.empty()) {
      glGenTextures(1, &m_shell_texture);
      glBindTexture(GL_TEXTURE_2D, m_shell_texture);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, shell.tex_width, shell.tex_height, 0,
                   GL_RGBA, GL_UNSIGNED_BYTE, shell.texture_rgba.data());
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
      glBindTexture(GL_TEXTURE_2D, 0);
      lg::info("[MarioRenderer] Shell texture uploaded ({}x{})",
               shell.tex_width, shell.tex_height);
    }

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);
  }

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

void MarioRenderer::destroy_corpse_meshes() {
  for (auto& m : m_corpse_meshes) {
    if (m.vao) glDeleteVertexArrays(1, &m.vao);
    if (m.vbo_position) glDeleteBuffers(1, &m.vbo_position);
    if (m.vbo_normal) glDeleteBuffers(1, &m.vbo_normal);
    if (m.vbo_color) glDeleteBuffers(1, &m.vbo_color);
    if (m.vbo_uv) glDeleteBuffers(1, &m.vbo_uv);
  }
  m_corpse_meshes.clear();
}

void MarioRenderer::rebuild_corpse_meshes(const std::vector<MarioGeometry>& corpses) {
  // Full rebuild: cheap because corpse capture is rare (one per Mario death)
  // and each corpse only takes a few KB of GPU memory.  Preserves nothing
  // from the previous mesh list — capture/clear semantics are bumpy enough
  // that incremental sync would only complicate things without measurable
  // gain.
  destroy_corpse_meshes();
  m_corpse_meshes.reserve(corpses.size());

  for (const auto& geo : corpses) {
    if (geo.num_triangles == 0) continue;

    CorpseMesh m;
    m.num_triangles = geo.num_triangles;
    int num_verts = m.num_triangles * 3;

    glGenVertexArrays(1, &m.vao);
    glGenBuffers(1, &m.vbo_position);
    glGenBuffers(1, &m.vbo_normal);
    glGenBuffers(1, &m.vbo_color);
    glGenBuffers(1, &m.vbo_uv);

    glBindVertexArray(m.vao);

    glBindBuffer(GL_ARRAY_BUFFER, m.vbo_position);
    glBufferData(GL_ARRAY_BUFFER, num_verts * 3 * sizeof(float), geo.position.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, nullptr);

    glBindBuffer(GL_ARRAY_BUFFER, m.vbo_normal);
    glBufferData(GL_ARRAY_BUFFER, num_verts * 3 * sizeof(float), geo.normal.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 0, nullptr);

    glBindBuffer(GL_ARRAY_BUFFER, m.vbo_color);
    glBufferData(GL_ARRAY_BUFFER, num_verts * 3 * sizeof(float), geo.color.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, 0, nullptr);

    glBindBuffer(GL_ARRAY_BUFFER, m.vbo_uv);
    glBufferData(GL_ARRAY_BUFFER, num_verts * 2 * sizeof(float), geo.uv.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, 0, nullptr);

    m_corpse_meshes.push_back(m);
  }

  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glBindVertexArray(0);
}

void MarioRenderer::render_corpses() {
  // Same shader / texture / uniforms as the live Mario — caller (render())
  // has already set them up.  Just swap VAO per corpse and draw.
  for (const auto& m : m_corpse_meshes) {
    if (m.num_triangles == 0) continue;
    glBindVertexArray(m.vao);
    glDrawArrays(GL_TRIANGLES, 0, m.num_triangles * 3);
  }
  glBindVertexArray(0);
}

void MarioRenderer::render_shell(const MarioState& state) {
  if (m_shell_tri_count == 0) return;

  // Bind the shell's own texture so the dome picks up the green pattern.
  if (m_shell_texture) {
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_shell_texture);
  }

  // Transform local-space shell verts to world space:
  //   1. Rotate around Y by face_angle
  //   2. Translate to Mario's position (in Jak units)
  const float ca = cosf(state.face_angle);
  const float sa = sinf(state.face_angle);
  const float mx = state.position.x();
  const float my = state.position.y();
  const float mz = state.position.z();

  int nv = m_shell_tri_count * 3;
  std::vector<float> pos(nv * 3);
  std::vector<float> nrm(nv * 3);

  for (int i = 0; i < nv; i++) {
    const auto& v = m_shell_local[i];
    // Rotate local position around Y
    float rx = v.px * ca - v.pz * sa;
    float rz = v.px * sa + v.pz * ca;
    pos[i * 3 + 0] = rx + mx;
    pos[i * 3 + 1] = v.py + my;
    pos[i * 3 + 2] = rz + mz;

    // Rotate normal the same way
    nrm[i * 3 + 0] = v.nx * ca - v.nz * sa;
    nrm[i * 3 + 1] = v.ny;
    nrm[i * 3 + 2] = v.nx * sa + v.nz * ca;
  }

  glBindBuffer(GL_ARRAY_BUFFER, m_shell_vbo_position);
  glBufferSubData(GL_ARRAY_BUFFER, 0, pos.size() * sizeof(float), pos.data());

  glBindBuffer(GL_ARRAY_BUFFER, m_shell_vbo_normal);
  glBufferSubData(GL_ARRAY_BUFFER, 0, nrm.size() * sizeof(float), nrm.data());

  glBindBuffer(GL_ARRAY_BUFFER, 0);

  glBindVertexArray(m_shell_vao);
  glDrawArrays(GL_TRIANGLES, 0, nv);
  glBindVertexArray(0);
}

void MarioRenderer::render(const float* camera_matrix,
                            const float* hvdf_offset,
                            const float* camera_pos,
                            float fog_constant) {
  auto& mgr = LibSM64Manager::instance();
  if (!mgr.is_initialized() || !m_initialized) return;

  // Hide Mario while Jak is dying / being eaten by the lurker shark / playing
  // the death animation.  Without this Mario stays visible (still riding the
  // shell), floating in mid-air during the cutscene.  read_target_flags
  // populates target_dying from the *sm64-jak-dying* GOAL symbol; the flag
  // clears once Jak's `dying` state-flag drops on respawn, at which point
  // the existing teleport gate snaps Mario back to Jak's checkpoint.  We
  // bail BEFORE the corpse render too so an in-progress shark-grab doesn't
  // leak Mario's last-frame geometry through the corpse path.
  if (mgr.target_dying) return;

  const bool draw_live = mgr.has_mario();
  const size_t corpse_count = mgr.corpse_count();
  const bool draw_corpses = corpse_count > 0;
  if (!draw_live && !draw_corpses) return;

  // Upload texture on first render
  if (!m_texture_uploaded) {
    upload_texture();
  }

  // Sync the corpse mesh list with the manager's snapshot only when the
  // version changes (capture / clear).  Full rebuild is fine — corpse capture
  // is rare and each corpse is a few KB.
  uint64_t cv = mgr.corpse_version();
  if (cv != m_corpse_uploaded_version) {
    if (corpse_count > 0) {
      auto corpses = mgr.get_mario_corpses();
      rebuild_corpse_meshes(corpses);
    } else {
      destroy_corpse_meshes();
    }
    m_corpse_uploaded_version = cv;
  }

  // Get latest geometry from the SM64 tick (skip if no live Mario — corpses
  // can still render below).
  MarioGeometry geo;
  if (draw_live) {
    geo = mgr.get_geometry();
    if (geo.num_triangles == 0 && !draw_corpses) return;
    if (geo.num_triangles > 0) {
      update_geometry(geo);
    }
  }

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
  // Mario is opaque — the alpha uniform exists so SM64CollisionRenderer
  // can fade its overlay, but Mario himself always wants 1.0.
  glUniform1f(glGetUniformLocation(program, "alpha"), 1.0f);

  // Light direction (simple sun-like, normalized)
  float light_dir[3] = {0.5f, 0.8f, 0.3f};
  float len = sqrtf(light_dir[0]*light_dir[0] + light_dir[1]*light_dir[1] + light_dir[2]*light_dir[2]);
  light_dir[0] /= len; light_dir[1] /= len; light_dir[2] /= len;
  glUniform3fv(glGetUniformLocation(program, "light_dir"), 1, light_dir);

  // Draw Mario (skip when there's no live Mario — only the corpse should
  // appear during the brief no-Mario window between delete + auto-respawn,
  // or for as long as the user keeps a corpse around with no live Mario).
  if (draw_live && m_num_triangles > 0) {
    glBindVertexArray(m_vao);
    glDrawArrays(GL_TRIANGLES, 0, m_num_triangles * 3);
    glBindVertexArray(0);
  }

  // Draw all corpses (frozen meshes at each death location).  Same shader /
  // texture / uniforms as the live Mario, so we just swap VAO per corpse.
  if (draw_corpses) {
    render_corpses();
  }

  // Shell visual is now handled GOAL-side by the sm64-crab-shell process
  // (mario.gc) — a subtype of the global lurkercrab type.  Leaving the
  // procedural-shell path available here but disabled by default so we
  // don't stack two overlapping shells on Mario.  Re-enable by setting
  // the block to run (or by re-exposing the bool on LibSM64Manager) if
  // you want to test the ROM-extracted koopa shell as a fallback.
#if 0
  auto state = mgr.get_render_state();
  if (state.action & kActFlagRidingShell) {
    render_shell(state);
  }
#endif

  glBindTexture(GL_TEXTURE_2D, 0);
}

}  // namespace sm64
