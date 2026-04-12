/*!
 * @file libsm64_integration.cpp
 * Implementation of the libsm64 integration manager.
 */

#include "libsm64_integration.h"

#include "sm64_audio.h"

#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>

#include "common/goal_constants.h"
#include "common/log/log.h"
#include "common/symbols.h"
#include "common/util/FileUtil.h"
#include "game/kernel/common/Ptr.h"
#include "game/kernel/common/Symbol4.h"
#include "game/kernel/common/kscheme.h"
#include "game/kernel/jak1/kscheme.h"

extern "C" {
#include "libsm64.h"
#include "decomp/tools/libmio0.h"
}

namespace sm64 {

[[maybe_unused]] static void sm64_debug_print(const char* msg) {
  // libsm64's audio engine (audio/load.c, audio/external.c) fires DEBUG_PRINT
  // from the cubeb worker thread on every audio tick. Routing any of that
  // through lg stalls the audio thread on stdio and torches the main-thread
  // FPS. We keep this stub around for manual re-registration when debugging
  // libsm64 internals, but don't wire it up by default.
  lg::debug("[libsm64] {}", msg);
}

LibSM64Manager& LibSM64Manager::instance() {
  static LibSM64Manager mgr;
  return mgr;
}

LibSM64Manager::~LibSM64Manager() {
  shutdown();
}

// SM64 US ROM is exactly 8 MiB. libsm64 expects the US revision; we use the
// size as a cheap selector when the user drops any .z64 into the search path.
static constexpr std::uintmax_t kExpectedSm64RomSize = 8u * 1024u * 1024u;

bool LibSM64Manager::init(const std::string& rom_path) {
  if (m_initialized) {
    lg::warn("[libsm64] Already initialized");
    return true;
  }

  // Read the SM64 ROM file
  std::ifstream rom_file(rom_path, std::ios::binary | std::ios::ate);
  if (!rom_file.is_open()) {
    lg::error("[libsm64] Failed to open ROM file: {}", rom_path);
    return false;
  }

  auto rom_size = rom_file.tellg();
  rom_file.seekg(0, std::ios::beg);
  std::vector<uint8_t> rom_data(rom_size);
  if (!rom_file.read(reinterpret_cast<char*>(rom_data.data()), rom_size)) {
    lg::error("[libsm64] Failed to read ROM file");
    return false;
  }
  rom_file.close();

  lg::info("[libsm64] ROM loaded: {} bytes", static_cast<size_t>(rom_size));

  // NOTE: intentionally NOT registering sm64_debug_print — libsm64's audio
  // engine spams DEBUG_PRINT on every audio tick from the cubeb worker
  // thread, which stalls on the log and destroys FPS. Re-enable manually if
  // you need to debug libsm64 internals.

  // Allocate texture atlas buffer
  m_texture_data.resize(4 * TEXTURE_WIDTH * TEXTURE_HEIGHT);

  // Initialize the library
  sm64_global_init(rom_data.data(), m_texture_data.data());

  // Boot the N64 audio engine from the same ROM, then kick off the cubeb
  // worker thread so audio starts playing immediately (title-screen jingle,
  // sfx, etc.). Failures are non-fatal — we just run silent.
  sm64_audio_init(rom_data.data());
  m_audio = std::make_unique<SM64AudioPlayer>(m_sm64_lock);
  m_audio->set_volume(m_audio_volume);
  if (!m_audio->start()) {
    lg::warn("[libsm64] Audio stream failed to start; continuing without audio");
  }

  // Pre-allocate tick buffers
  m_tick_position_buf.resize(9 * GEO_MAX_TRIANGLES);
  m_tick_normal_buf.resize(9 * GEO_MAX_TRIANGLES);
  m_tick_color_buf.resize(9 * GEO_MAX_TRIANGLES);
  m_tick_uv_buf.resize(6 * GEO_MAX_TRIANGLES);

  // Keep the ROM bytes long enough to extract the koopa-shell model + texture
  // from the compressed actor segment, then free them.
  m_rom_data = std::move(rom_data);
  if (!extract_shell_from_rom()) {
    lg::warn("[libsm64] Shell model extraction failed — shell won't render");
  }

  m_initialized = true;
  m_last_rom_path = rom_path;
  lg::info("[libsm64] Initialized successfully");
  return true;
}

bool LibSM64Manager::init_autodetect() {
  if (m_initialized) {
    return true;
  }
  // Uses the project's ghc::filesystem alias from FileUtil.h.

  // Search order: directory next to gk.exe first (so a user drop-in ROM wins),
  // then iso_data/mario/ under the project dir. We pick the first .z64 whose
  // size matches the expected US ROM size.
  std::vector<fs::path> search_dirs;
  try {
    std::string exe_str = file_util::get_current_executable_path();
    if (!exe_str.empty()) {
      fs::path exe(exe_str);
      search_dirs.push_back(exe.parent_path());
    }
  } catch (...) {
    // fall through; we still have iso_data/mario as a fallback
  }
  try {
    fs::path proj = file_util::get_jak_project_dir();
    if (!proj.empty()) {
      search_dirs.push_back(proj / "iso_data" / "mario");
    }
  } catch (...) {
  }

  fs::path picked;
  for (const auto& dir : search_dirs) {
    std::error_code ec;
    if (!fs::exists(dir, ec) || !fs::is_directory(dir, ec)) continue;
    fs::directory_iterator it(dir, ec), end;
    if (ec) continue;
    for (; it != end; it.increment(ec)) {
      if (ec) break;
      const auto& entry = *it;
      if (!entry.is_regular_file(ec)) continue;
      const auto& p = entry.path();
      auto ext = p.extension().string();
      // Case-insensitive .z64 check.
      if (ext.size() != 4) continue;
      if ((ext[0] != '.') || (ext[1] != 'z' && ext[1] != 'Z') ||
          (ext[2] != '6') || (ext[3] != '4')) continue;
      auto sz = fs::file_size(p, ec);
      if (ec) continue;
      if (sz != kExpectedSm64RomSize) {
        lg::info("[libsm64] Skipping {} ({} bytes, expected {})", p.string(),
                 static_cast<size_t>(sz), static_cast<size_t>(kExpectedSm64RomSize));
        continue;
      }
      picked = p;
      break;
    }
    if (!picked.empty()) break;
  }

  if (picked.empty()) {
    lg::warn("[libsm64] Auto-detect: no matching .z64 found next to gk or in iso_data/mario");
    return false;
  }
  lg::info("[libsm64] Auto-detected ROM: {}", picked.string());
  return init(picked.string());
}

void LibSM64Manager::set_audio_volume(int volume) {
  if (volume < 0) volume = 0;
  if (volume > 100) volume = 100;
  m_audio_volume = volume;
  if (m_audio) {
    m_audio->set_volume(volume);
  }
}

int LibSM64Manager::get_audio_volume() const {
  return m_audio_volume;
}

// ---------------------------------------------------------------------------
// Koopa-shell model extraction from the SM64 ROM
// ---------------------------------------------------------------------------
// The koopa_shell model lives inside an MIO0-compressed actor-group segment
// in the SM64 US ROM.  We scan every MIO0 block, decompress it, and search
// the decompressed data for the known koopa-shell vertex pattern (first
// three dome vertex positions).  Once found we use the relative layout from
// the SM64 matching decomp to locate the three F3D display lists (dome /
// belly / ring) and the 32×32 RGBA16 texture.
//
// If the data is not found in any MIO0 block we also search the raw
// (uncompressed) ROM as a fallback.
// ---------------------------------------------------------------------------

// Helpers — big-endian reads for N64 binary data.
static uint32_t rom_be32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
         (uint32_t(p[2]) << 8) | p[3];
}
static int16_t rom_be_s16(const uint8_t* p) {
  return static_cast<int16_t>((p[0] << 8) | p[1]);
}

// (Vertex pattern search removed — we now locate the koopa-shell display lists
//  by searching the ROM for the GEO_SCALE(16384) geo layout pattern instead.)

bool LibSM64Manager::extract_shell_from_rom() {
  if (m_rom_data.size() < 0x100000) {
    lg::warn("[libsm64] ROM too small for shell extraction");
    return false;
  }

  static constexpr uint32_t MIO0_MAGIC = 0x4D494F30;  // "MIO0"
  static constexpr int      TEX_W      = 32;
  static constexpr int      TEX_H      = 32;
  static constexpr int      TEX_BYTES  = TEX_W * TEX_H * 2;

  // (No hardcoded relative offsets needed — DL addresses are read from the
  //  geo layout found in the ROM, making this ROM-version-independent.)

  // Per-region vertex colours — baked from SM64's Lights1 definitions
  // (ambient + 0.5 × diffuse, approximating a 45° light angle).
  static const float region_col[3][3] = {
      {0.20f, 0.60f, 0.07f},  // dome  — green
      {0.39f, 0.56f, 0.67f},  // belly — blue-grey
      {0.55f, 0.56f, 0.55f},  // ring  — light grey
  };

  // ---- Locate the koopa-shell display lists in the ROM --------------------
  //
  // Strategy: search the raw ROM for the koopa-shell GEO LAYOUT, which
  // contains GEO_SCALE(0, 16384) followed by three GEO_DISPLAY_LIST commands.
  // Those commands embed the actual segmented addresses of the dome / belly /
  // ring display lists inside segment 8.  We then decompress the segment-8
  // MIO0 block (at a known ROM offset) and parse the DLs directly.
  //
  // This approach works regardless of ROM region (US, JP, EU) or ROM hacks
  // because it reads the DL addresses from the geo layout data itself.

  std::vector<uint8_t> seg_data;       // decompressed segment-8 data
  uint32_t dome_dl_off  = UINT32_MAX;  // buffer offsets of the 3 shell DLs
  uint32_t belly_dl_off = UINT32_MAX;
  uint32_t ring_dl_off  = UINT32_MAX;

  // 1. Decompress the segment-8 MIO0 block (actor model data).
  //    Known ROM offset: 0x114750 (SM64 US/JP/EU all have MIO0 here).
  constexpr uint32_t SEG8_ROM = 0x114750;
  if (SEG8_ROM + MIO0_HEADER_LENGTH < m_rom_data.size() &&
      rom_be32(m_rom_data.data() + SEG8_ROM) == MIO0_MAGIC) {
    mio0_header_t hdr;
    if (mio0_decode_header(m_rom_data.data() + SEG8_ROM, &hdr) &&
        hdr.dest_size > 0x10000) {
      seg_data.resize(hdr.dest_size);
      if (mio0_decode(m_rom_data.data() + SEG8_ROM, seg_data.data(), nullptr) < 0) {
        seg_data.clear();
      } else {
        lg::info("[libsm64] Segment 8 decompressed: {} bytes from MIO0 @ROM 0x{:06X}",
                 hdr.dest_size, SEG8_ROM);
      }
    }
  }

  if (seg_data.empty()) {
    lg::warn("[libsm64] Failed to decompress segment-8 MIO0 block");
    m_rom_data.clear();
    m_rom_data.shrink_to_fit();
    return false;
  }

  // 2. Search for the koopa-shell geo layout.
  //    Binary pattern of GEO_SCALE(0, 16384) + GEO_OPEN_NODE:
  //      1C 00 00 00  00 00 40 00  04 00 00 00
  //    Followed by three GEO_DISPLAY_LIST (opcode 0x15) commands,
  //    each 8 bytes: 15 LL 00 00  08 XX XX XX
  //
  //    We search the raw ROM first (geo data is usually uncompressed),
  //    then every decompressed MIO0 block as fallback.
  // Flexible geo pattern: GEO_SCALE(any_params, any_scale) + GEO_OPEN_NODE.
  // We only fix: byte[0]=0x1C, bytes[8..11]=04000000, then check for 3× 0x15.
  // This works even if the ROM hack changed the scale value.
  auto match_geo_scale_open = [](const uint8_t* p) -> bool {
    return p[0] == 0x1C &&                             // GEO_SCALE opcode
           p[8] == 0x04 && p[9] == 0x00 &&             // GEO_OPEN_NODE
           p[10] == 0x00 && p[11] == 0x00;
  };
  constexpr size_t GP_SIZE = 12;  // GEO_SCALE (8 bytes) + GEO_OPEN_NODE (4 bytes)

  // Helper: check if a buffer at a given offset contains the geo layout pattern
  // followed by three valid GEO_DISPLAY_LIST commands pointing into seg_data.
  auto try_geo_match = [&](const uint8_t* buf, size_t buf_size,
                           size_t off) -> bool {
    uint32_t cmd_base = static_cast<uint32_t>(off) + 12;
    uint32_t dl_seg_tmp[3];
    for (int i = 0; i < 3; ++i) {
      uint32_t co = cmd_base + i * 8;
      if (co + 8 > buf_size) return false;
      if (buf[co] != 0x15) return false;
      uint32_t addr = rom_be32(buf + co + 4);
      if ((addr >> 24) != 0x08) return false;
      uint32_t seg_off = addr & 0x00FFFFFF;
      if (seg_off >= seg_data.size()) return false;
      dl_seg_tmp[i] = seg_off;
    }
    // Validate the first DL has real F3D/F3DEX2 commands.
    bool has_gvtx_or_tri = false;
    int valid_ops = 0;
    for (int ci = 0; ci < 15; ++ci) {
      uint32_t co = dl_seg_tmp[0] + ci * 8;
      if (co + 8 > seg_data.size()) break;
      uint8_t op = seg_data[co];
      if (op == 0xB8 || op == 0xDF) break;
      if (op == 0x04 || op == 0x01 || op == 0xBF || op == 0x05)
        has_gvtx_or_tri = true;
      if ((op >= 0xB0 && op <= 0xBF) || op >= 0xE0 || op == 0x04 || op == 0x01)
        ++valid_ops;
    }
    if (!has_gvtx_or_tri || valid_ops < 3) return false;

    dome_dl_off  = dl_seg_tmp[0];
    belly_dl_off = dl_seg_tmp[1];
    ring_dl_off  = dl_seg_tmp[2];
    return true;
  };

  // 2a. Search raw ROM (geo data is typically uncompressed)
  for (size_t off = 0; off + GP_SIZE + 28 < m_rom_data.size(); ++off) {
    if (!match_geo_scale_open(m_rom_data.data() + off))
      continue;
    if (try_geo_match(m_rom_data.data(), m_rom_data.size(), off)) {
      lg::info("[libsm64] Found koopa-shell geo layout in raw ROM at 0x{:06X}", off);
      lg::info("[libsm64] DL seg offsets: dome=0x{:06X} belly=0x{:06X} ring=0x{:06X}",
               dome_dl_off, belly_dl_off, ring_dl_off);
      break;
    }
  }

  // 2b. Fallback: search ALL decompressed MIO0 blocks for the geo layout.
  if (dome_dl_off == UINT32_MAX) {
    lg::info("[libsm64] Geo layout not in raw ROM — searching MIO0 blocks...");
    for (size_t moff = 0; moff + MIO0_HEADER_LENGTH < m_rom_data.size(); moff += 4) {
      if (rom_be32(m_rom_data.data() + moff) != MIO0_MAGIC) continue;
      mio0_header_t hdr;
      if (!mio0_decode_header(m_rom_data.data() + moff, &hdr)) continue;
      if (hdr.dest_size < GP_SIZE + 28) continue;
      std::vector<uint8_t> tmp(hdr.dest_size);
      if (mio0_decode(m_rom_data.data() + moff, tmp.data(), nullptr) < 0) continue;
      for (size_t off = 0; off + GP_SIZE + 28 < hdr.dest_size; ++off) {
        if (!match_geo_scale_open(tmp.data() + off)) continue;
        if (try_geo_match(tmp.data(), hdr.dest_size, off)) {
          lg::info("[libsm64] Found koopa-shell geo layout in MIO0 @ROM 0x{:06X} at offset 0x{:X}",
                   moff, off);
          lg::info("[libsm64] DL seg offsets: dome=0x{:06X} belly=0x{:06X} ring=0x{:06X}",
                   dome_dl_off, belly_dl_off, ring_dl_off);
          break;
        }
      }
      if (dome_dl_off != UINT32_MAX) break;
    }
  }

  // 2c. Fallback: brute-force F3D display-list scan of decompressed segment 8.
  //     Instead of relying on a geo layout pattern (which ROM hacks may alter),
  //     scan for valid F3D command sequences ending with G_ENDDL directly.
  if (dome_dl_off == UINT32_MAX) {
    lg::info("[libsm64] Geo layout not found — scanning seg8 for F3D display lists...");

    // Count G_ENDDL markers to reliably detect GBI encoding.
    // F3D: G_ENDDL = B8000000 00000000.  F3DEX2: DF000000 00000000.
    int enddl_f3d = 0, enddl_f3dex2 = 0;
    for (size_t off = 0; off + 8 <= seg_data.size(); off += 8) {
      uint32_t w0 = rom_be32(seg_data.data() + off);
      uint32_t w1 = rom_be32(seg_data.data() + off + 4);
      if (w0 == 0xB8000000 && w1 == 0) enddl_f3d++;
      if (w0 == 0xDF000000 && w1 == 0) enddl_f3dex2++;
    }
    lg::info("[libsm64] G_ENDDL counts: F3D(0xB8)={}, F3DEX2(0xDF)={}", enddl_f3d, enddl_f3dex2);


    // Use whichever GBI has more G_ENDDL markers.
    // If both are 0, try F3DEX2 first (common in ROM hacks).
    bool scan_f3dex2 = (enddl_f3dex2 >= enddl_f3d);
    if (enddl_f3d == 0 && enddl_f3dex2 == 0) scan_f3dex2 = true;
    lg::info("[libsm64] DL scan using GBI: {}", scan_f3dex2 ? "F3DEX2" : "F3D");

    struct FoundDL {
      uint32_t offset;
      int tri_count;
      int vtx_cmds;
      bool has_tex;         // any G_SETTIMG command
      uint32_t tex_seg_addr;
    };
    std::vector<FoundDL> found_dls;

    // Try DL scan — if the chosen GBI yields 0 results, flip and try the other.
    for (int gbi_try = 0; gbi_try < 2 && found_dls.empty(); ++gbi_try) {
      if (gbi_try == 1) {
        scan_f3dex2 = !scan_f3dex2;
        lg::info("[libsm64] Retrying with GBI: {}", scan_f3dex2 ? "F3DEX2" : "F3D");
      }

      const uint8_t S_VTX  = scan_f3dex2 ? 0x01 : 0x04;
      const uint8_t S_TRI1 = scan_f3dex2 ? 0x05 : 0xBF;
      const uint8_t S_TRI2 = 0x06;
      const uint8_t S_END  = scan_f3dex2 ? 0xDF : 0xB8;

      for (size_t off = 0; off + 8 <= seg_data.size(); off += 8) {
        uint8_t first = seg_data[off];
        if (first == 0x00 || (first >= 0x07 && first < 0xB0)) continue;

        int tris = 0, vtxc = 0, total = 0;
        bool has_tex = false;
        uint32_t tex_addr = 0;
        bool ended = false;

        for (int ci = 0; ci < 500; ++ci) {
          size_t co = off + static_cast<size_t>(ci) * 8;
          if (co + 8 > seg_data.size()) break;
          uint8_t  op = seg_data[co];
          uint32_t w0 = rom_be32(seg_data.data() + co);
          uint32_t w1 = rom_be32(seg_data.data() + co + 4);

          if (op == S_END) {
            if (w1 == 0 && (w0 >> 24) == S_END) { total = ci + 1; ended = true; }
            break;
          }

          if (op == S_VTX) {
            uint8_t seg_byte = static_cast<uint8_t>(w1 >> 24);
            uint32_t v_off = w1 & 0x00FFFFFF;
            if (seg_byte >= 0x04 && seg_byte <= 0x0F && v_off + 16 <= seg_data.size()) {
              vtxc++;
            } else {
              break;
            }
            continue;
          }
          if (op == S_TRI1) {
            if (!scan_f3dex2 && (w0 & 0x00FFFFFF) != 0) break;
            tris++;
            continue;
          }
          if (scan_f3dex2 && op == S_TRI2) { tris += 2; continue; }
          if (op == 0xFD) {  // G_SETTIMG — any format, any segment
            has_tex = true;
            tex_addr = w1;
            continue;
          }
          if (op == 0x00 || (op >= 0x07 && op < 0xB0)) break;
        }

        if (ended && vtxc >= 1 && tris >= 2) {
          found_dls.push_back({static_cast<uint32_t>(off), tris, vtxc, has_tex, tex_addr});
          off += static_cast<size_t>(total - 1) * 8;
        }
      }
    }

    lg::info("[libsm64] DL scan: {} display lists found in segment 8", found_dls.size());

    // Find the best 3-DL cluster matching the koopa shell (76 triangles).
    int best_idx = -1;
    int best_tris = 0;
    int best_score = -1;
    for (size_t i = 0; i + 2 < found_dls.size(); ++i) {
      uint32_t span = found_dls[i + 2].offset - found_dls[i].offset;
      if (span > 0x3000) continue;
      int tc = 0, tot = 0;
      for (int j = 0; j < 3; ++j) {
        tot += found_dls[i + j].tri_count;
        if (found_dls[i + j].has_tex) tc++;
      }

      // Score: textured cluster (exactly 1 tex) gets +100, then prefer totals
      // close to 76 (the US decomp shell triangle count).
      int score = 0;
      if (tc == 1) score += 100;  // strong signal: 1 textured + 2 plain
      score -= std::abs(tot - 76);  // penalty for deviating from expected total
      if (tot < 15 || tot > 250) continue;

      // Use >= to prefer LATER clusters with equal score.  SM64's koopa shell
      // has TWO sets of DLs in segment 8: inside (opaque, normals inward) then
      // outside (transparent, normals outward).  Both have 76 triangles and
      // score identically, but we want the outside shell since its normals face
      // the camera for correct lighting.
      if (score >= best_score) {
        best_idx = static_cast<int>(i);
        best_tris = tot;
        best_score = score;
      }
    }

    if (best_idx >= 0) {
      // Identify dome = the textured DL (if any), otherwise the largest.
      int dome_j = -1;
      for (int j = 0; j < 3; ++j)
        if (found_dls[best_idx + j].has_tex) dome_j = j;
      if (dome_j < 0) {
        // No textured DL — pick the one with the most triangles as the dome.
        int max_t = 0;
        for (int j = 0; j < 3; ++j) {
          if (found_dls[best_idx + j].tri_count > max_t) {
            max_t = found_dls[best_idx + j].tri_count;
            dome_j = j;
          }
        }
      }
      dome_dl_off = found_dls[best_idx + dome_j].offset;
      int nt = 0;
      for (int j = 0; j < 3; ++j) {
        if (j == dome_j) continue;
        if (nt == 0) belly_dl_off = found_dls[best_idx + j].offset;
        else          ring_dl_off  = found_dls[best_idx + j].offset;
        ++nt;
      }
      lg::info("[libsm64] Shell identified: dome=0x{:06X} belly=0x{:06X} ring=0x{:06X} ({} tris, score={})",
               dome_dl_off, belly_dl_off, ring_dl_off, best_tris, best_score);
    }
  }

  if (dome_dl_off == UINT32_MAX) {
    lg::warn("[libsm64] Koopa-shell display lists not found in segment 8");
    m_rom_data.clear();
    m_rom_data.shrink_to_fit();
    return false;
  }

  // ---- Vertex data buffer ---------------------------------------------------
  // In SM64 ROM hacks, segment 4 (common actor data) and segment 8 (actor DLs)
  // often map to the same decompressed MIO0 block. The G_VTX address byte may
  // say 0x04 but the data lives in our segment 8 buffer. We always read vertices
  // from seg_data using the 24-bit offset, ignoring the segment byte.
  const uint8_t* vtx_buf = seg_data.data();
  const size_t vtx_buf_size = seg_data.size();

  // Log the vertex segment reference for diagnostics.
  for (int ci = 0; ci < 30; ++ci) {
    uint32_t co = dome_dl_off + ci * 8;
    if (co + 8 > seg_data.size()) break;
    uint8_t op = seg_data[co];
    if (op == 0xB8 || op == 0xDF) break;
    if (op == 0x04 || op == 0x01) {
      uint32_t w1 = rom_be32(seg_data.data() + co + 4);
      uint32_t vtx_off = w1 & 0x00FFFFFF;
      lg::info("[libsm64] Vertex ref: seg=0x{:02X} offset=0x{:06X} (using seg8 buffer)",
               static_cast<uint8_t>(w1 >> 24), vtx_off);
      // Dump the first 3 vertices at this offset for validation.
      for (int vi = 0; vi < 3 && vtx_off + (vi + 1) * 16 <= seg_data.size(); ++vi) {
        const uint8_t* vp = seg_data.data() + vtx_off + vi * 16;
        lg::info("[libsm64]   vtx[{}] pos=[{},{},{}] tc=[{},{}] n=[{},{},{}]",
                 vi, rom_be_s16(vp), rom_be_s16(vp + 2), rom_be_s16(vp + 4),
                 rom_be_s16(vp + 8), rom_be_s16(vp + 10),
                 static_cast<int8_t>(vp[12]), static_cast<int8_t>(vp[13]),
                 static_cast<int8_t>(vp[14]));
      }
      break;
    }
  }

  // ---- Parse an N64 Vtx from the buffer -----------------------------------
  struct N64Vtx {
    int16_t x, y, z;
    int16_t tc_s, tc_t;
    int8_t nx, ny, nz;
  };

  auto read_vtx = [&](uint32_t voff) -> N64Vtx {
    const uint8_t* p = vtx_buf + voff;
    N64Vtx v{};
    v.x    = rom_be_s16(p + 0);
    v.y    = rom_be_s16(p + 2);
    v.z    = rom_be_s16(p + 4);
    v.tc_s = rom_be_s16(p + 8);
    v.tc_t = rom_be_s16(p + 10);
    v.nx   = static_cast<int8_t>(p[12]);
    v.ny   = static_cast<int8_t>(p[13]);
    v.nz   = static_cast<int8_t>(p[14]);
    return v;
  };

  // Detect GBI encoding by looking for G_VTX opcodes in the dome DL.
  // F3D uses 0x04 for G_VTX; F3DEX2 uses 0x01.
  bool use_f3dex2 = false;
  for (int ci = 0; ci < 30; ++ci) {
    uint32_t co = dome_dl_off + ci * 8;
    if (co + 8 > seg_data.size()) break;
    uint8_t op = seg_data[co];
    if (op == 0x04) { use_f3dex2 = false; break; }
    if (op == 0x01) { use_f3dex2 = true; break; }
    if (op == 0xB8 || op == 0xDF) break;  // hit end-DL before finding G_VTX
  }

  const uint8_t OP_VTX   = use_f3dex2 ? 0x01 : 0x04;
  const uint8_t OP_TRI1  = use_f3dex2 ? 0x05 : 0xBF;
  const uint8_t OP_TRI2  = 0x06;  // F3DEX2 only
  const uint8_t OP_ENDDL = use_f3dex2 ? 0xDF : 0xB8;
  const int IDX_DIV = use_f3dex2 ? 2 : 10;

  lg::info("[libsm64] GBI encoding: {}", use_f3dex2 ? "F3DEX2" : "F3D");

  lg::info("[libsm64] DL offsets: dome=0x{:X} belly=0x{:X} ring=0x{:X}",
           dome_dl_off, belly_dl_off, ring_dl_off);

  // ---- Walk one display list, emitting triangles --------------------------
  auto parse_dl = [&](uint32_t dl_off, int region_idx,
                      std::vector<ShellMeshData::Vertex>& out) {
    N64Vtx vbuf[32] = {};   // F3DEX2 can have 32 slots
    bool   vvalid[32] = {};

    const uint8_t* dl = seg_data.data() + dl_off;
    for (int ci = 0; ci < 300; ++ci, dl += 8) {
      if (dl + 8 > seg_data.data() + seg_data.size()) break;
      const uint8_t op = dl[0];
      const uint32_t w0 = rom_be32(dl);
      const uint32_t w1 = rom_be32(dl + 4);

      if (op == OP_ENDDL) break;

      if (op == OP_VTX) {
        // Translate segmented address to buffer offset within vtx_buf.
        // vtx_buf points to the correct decompressed segment (seg 4 or seg 8).
        uint32_t vaddr = w1 & 0x00FFFFFF;
        int n, v0;
        if (use_f3dex2) {
          n  = ((w0 >> 12) & 0xFF) / 2;
          v0 = ((w0 >>  1) & 0x7F) / 2 - n;
        } else {
          // Derive n from the size field (bits 0-15) rather than the 4-bit
          // n field (bits 20-23).  Some ROM hacks encode actual_n - 1 in the
          // 4-bit field to fit n=16 into 4 bits; the size field is always
          // sizeof(Vtx) * (v0 + n) or sizeof(Vtx) * (v0 + n) - 1.
          // Using the size field is more reliable.
          int total_slots = ((w0 & 0xFFFF) + 1) / 16;
          n  = total_slots;
          v0 = 0;
        }
        if (v0 < 0) v0 = 0;
        if (n > 32) n = 32;
        for (int i = 0; i < n && (v0 + i) < 32; ++i) {
          uint32_t vpos = vaddr + i * 16;
          if (vpos + 16 <= vtx_buf_size) {
            vbuf[v0 + i]   = read_vtx(vpos);
            vvalid[v0 + i] = true;
          }
        }
        continue;
      }

      // Triangle commands (F3D: 0xBF; F3DEX2: 0x05 for TRI1, 0x06 for TRI2)
      auto emit_tri = [&](int i0, int i1, int i2) {
        if (i0 >= 32 || i1 >= 32 || i2 >= 32) return;
        if (!vvalid[i0] || !vvalid[i1] || !vvalid[i2]) return;
        for (int vi : {i0, i1, i2}) {
          ShellMeshData::Vertex sv{};
          sv.px = static_cast<float>(vbuf[vi].x);
          sv.py = static_cast<float>(vbuf[vi].y);
          sv.pz = static_cast<float>(vbuf[vi].z);
          sv.nx = vbuf[vi].nx / 127.0f;
          sv.ny = vbuf[vi].ny / 127.0f;
          sv.nz = vbuf[vi].nz / 127.0f;
          if (region_idx == 0 && (vbuf[vi].tc_s != 0 || vbuf[vi].tc_t != 0)) {
            // Dome vertex with real texture coords — sample the texture.
            sv.u = vbuf[vi].tc_s / (32.0f * TEX_W);
            sv.v = vbuf[vi].tc_t / (32.0f * TEX_H);
          } else {
            // Belly, ring, or dome vertex with tc=0 — use vertex colour.
            sv.u = -1.0f;
            sv.v = -1.0f;
          }
          sv.cr = region_col[region_idx][0];
          sv.cg = region_col[region_idx][1];
          sv.cb = region_col[region_idx][2];
          out.push_back(sv);
        }
      };

      if (op == OP_TRI1) {
        if (use_f3dex2) {
          // F3DEX2 TRI1: w0 bytes 1,2,3 are indices * 2
          emit_tri(((w0 >> 16) & 0xFF) / IDX_DIV,
                   ((w0 >>  8) & 0xFF) / IDX_DIV,
                   ( w0        & 0xFF) / IDX_DIV);
        } else {
          // F3D TRI1: w1 bytes 1,2,3 are indices * 10
          emit_tri(((w1 >> 16) & 0xFF) / IDX_DIV,
                   ((w1 >>  8) & 0xFF) / IDX_DIV,
                   ( w1        & 0xFF) / IDX_DIV);
        }
      }

      if (use_f3dex2 && op == OP_TRI2) {
        // Two triangles packed in one command
        emit_tri(((w0 >> 16) & 0xFF) / IDX_DIV,
                 ((w0 >>  8) & 0xFF) / IDX_DIV,
                 ( w0        & 0xFF) / IDX_DIV);
        emit_tri(((w1 >> 16) & 0xFF) / IDX_DIV,
                 ((w1 >>  8) & 0xFF) / IDX_DIV,
                 ( w1        & 0xFF) / IDX_DIV);
      }
    }
  };

  // ---- Parse the three shell display lists --------------------------------
  m_shell_mesh.vertices.clear();
  const uint32_t dl_offsets[3] = {dome_dl_off, belly_dl_off, ring_dl_off};
  const char* dl_names[3] = {"dome", "belly", "ring"};
  for (int di = 0; di < 3; ++di) {
    size_t before = m_shell_mesh.vertices.size();
    parse_dl(dl_offsets[di], di, m_shell_mesh.vertices);
    int tris_here = static_cast<int>((m_shell_mesh.vertices.size() - before) / 3);
    lg::info("[libsm64] {} DL at 0x{:X}: {} triangles", dl_names[di], dl_offsets[di], tris_here);
  }
  m_shell_mesh.tri_count = static_cast<int>(m_shell_mesh.vertices.size()) / 3;

  if (m_shell_mesh.tri_count == 0) {
    lg::warn("[libsm64] No triangles parsed from any koopa-shell display list");
    m_rom_data.clear();
    m_rom_data.shrink_to_fit();
    return false;
  }

  lg::info("[libsm64] Shell total: {} triangles, {} vertices",
           m_shell_mesh.tri_count, (int)m_shell_mesh.vertices.size());

  // Log vertex bounding box to validate geometry.
  {
    float mn[3] = { 1e9f,  1e9f,  1e9f};
    float mx[3] = {-1e9f, -1e9f, -1e9f};
    for (const auto& v : m_shell_mesh.vertices) {
      mn[0] = std::min(mn[0], v.px); mx[0] = std::max(mx[0], v.px);
      mn[1] = std::min(mn[1], v.py); mx[1] = std::max(mx[1], v.py);
      mn[2] = std::min(mn[2], v.pz); mx[2] = std::max(mx[2], v.pz);
    }
    lg::info("[libsm64] Shell vertex bbox: [{:.0f},{:.0f},{:.0f}] to [{:.0f},{:.0f},{:.0f}]",
             mn[0], mn[1], mn[2], mx[0], mx[1], mx[2]);
    lg::info("[libsm64] Shell size: {:.0f} x {:.0f} x {:.0f}",
             mx[0] - mn[0], mx[1] - mn[1], mx[2] - mn[2]);
  }

  // ---- Extract texture (RGBA16 32×32) -------------------------------------
  // Search for a gsDPSetTextureImage (0xFD) command in AND around the dome DL.
  // In some ROM hacks the texture setup is in a separate DL nearby, so we scan
  // a window of ±0x800 bytes around the dome DL offset.
  uint32_t tex_off = UINT32_MAX;
  {
    // 1) Search inside the dome DL first.
    const uint8_t* dl = seg_data.data() + dome_dl_off;
    for (int ci = 0; ci < 100; ++ci, dl += 8) {
      if (dl + 8 > seg_data.data() + seg_data.size()) break;
      if (dl[0] == OP_ENDDL) break;
      if (dl[0] == 0xFD) {
        uint32_t seg_addr = rom_be32(dl + 4) & 0x00FFFFFF;
        if (seg_addr + TEX_BYTES <= seg_data.size()) {
          tex_off = seg_addr;
          lg::info("[libsm64] Shell texture in dome DL: seg 0x{:06X}", tex_off);
          break;
        }
      }
    }
    // 2) Search ±0x800 bytes around the dome DL for G_SETTIMG referencing
    //    segment 8 (our decompressed buffer) with RGBA16 format.
    if (tex_off == UINT32_MAX) {
      uint32_t lo = (dome_dl_off > 0x800) ? dome_dl_off - 0x800 : 0;
      uint32_t hi = std::min(static_cast<uint32_t>(seg_data.size()),
                             dome_dl_off + 0x800);
      for (uint32_t off = lo; off + 8 <= hi; off += 8) {
        if (seg_data[off] != 0xFD) continue;
        uint32_t w0 = rom_be32(seg_data.data() + off);
        uint32_t w1 = rom_be32(seg_data.data() + off + 4);
        if ((w0 & 0xFF180000) != 0xFD100000) continue;
        uint8_t seg_byte = static_cast<uint8_t>(w1 >> 24);
        if (seg_byte != 0x08) continue;  // only accept segment 8 refs
        uint32_t seg_addr = w1 & 0x00FFFFFF;
        if (seg_addr + TEX_BYTES <= seg_data.size()) {
          tex_off = seg_addr;
          lg::info("[libsm64] Shell texture near dome DL @0x{:X}: seg8 0x{:06X}",
                   off, tex_off);
          break;
        }
      }
    }
    // 3) The DL may reference a texture in segment 4 (common group data).
    //    Search for the MIO0 block that holds segment 4 and extract from there.
    if (tex_off == UINT32_MAX) {
      // Find the G_SETTIMG near the dome DL (any segment).
      uint32_t tex_seg_addr = UINT32_MAX;
      uint32_t lo = (dome_dl_off > 0x800) ? dome_dl_off - 0x800 : 0;
      uint32_t hi = std::min(static_cast<uint32_t>(seg_data.size()),
                             dome_dl_off + 0x800);
      for (uint32_t off = lo; off + 8 <= hi; off += 8) {
        if (seg_data[off] != 0xFD) continue;
        uint32_t w0 = rom_be32(seg_data.data() + off);
        uint32_t w1 = rom_be32(seg_data.data() + off + 4);
        if ((w0 & 0xFF180000) != 0xFD100000) continue;
        tex_seg_addr = w1;
        break;
      }
      if (tex_seg_addr != UINT32_MAX) {
        uint32_t tex_seg_off = tex_seg_addr & 0x00FFFFFF;
        lg::info("[libsm64] Shell texture refs seg 0x{:02X} offset 0x{:06X}",
                 (tex_seg_addr >> 24), tex_seg_off);
        // Try each MIO0 block — decompress and check if it has enough data.
        for (size_t moff = 0; moff + MIO0_HEADER_LENGTH < m_rom_data.size(); moff += 4) {
          if (rom_be32(m_rom_data.data() + moff) != MIO0_MAGIC) continue;
          mio0_header_t hdr;
          if (!mio0_decode_header(m_rom_data.data() + moff, &hdr)) continue;
          if (hdr.dest_size < tex_seg_off + TEX_BYTES) continue;
          std::vector<uint8_t> tmp(hdr.dest_size);
          if (mio0_decode(m_rom_data.data() + moff, tmp.data(), nullptr) < 0) continue;
          // Validate: check that data at tex_seg_off looks like RGBA16 pixels.
          const uint8_t* t = tmp.data() + tex_seg_off;
          int nz = 0, a1 = 0;
          for (int i = 0; i < TEX_W * TEX_H; ++i) {
            uint16_t px = (t[i * 2] << 8) | t[i * 2 + 1];
            if (px) nz++;
            if (px & 1) a1++;
          }
          if (nz < TEX_W * TEX_H / 4) continue;  // mostly zeros — not a texture
          // Found a plausible texture!
          tex_off = 0;  // placeholder — copy directly below
          lg::info("[libsm64] Shell texture found in MIO0 @ROM 0x{:06X} (nz={}, a1={})",
                   moff, nz, a1);
          // Copy the texture bytes into a temporary location within seg_data
          // (not ideal, but avoids managing another buffer).  Instead, just
          // decode the texture directly here.
          m_shell_mesh.texture_rgba.resize(TEX_W * TEX_H * 4);
          m_shell_mesh.tex_width  = TEX_W;
          m_shell_mesh.tex_height = TEX_H;
          for (int i = 0; i < TEX_W * TEX_H; ++i) {
            uint16_t px = (t[i * 2] << 8) | t[i * 2 + 1];
            m_shell_mesh.texture_rgba[i * 4 + 0] = static_cast<uint8_t>(((px >> 11) & 0x1F) << 3);
            m_shell_mesh.texture_rgba[i * 4 + 1] = static_cast<uint8_t>(((px >>  6) & 0x1F) << 3);
            m_shell_mesh.texture_rgba[i * 4 + 2] = static_cast<uint8_t>(((px >>  1) & 0x1F) << 3);
            m_shell_mesh.texture_rgba[i * 4 + 3] = (px & 1) ? 0xFF : 0x00;
          }
          tex_off = UINT32_MAX - 1;  // signal: texture already decoded
          break;
        }
      }
    }
  }
  // Fallback: try known US decomp texture offset (0x025778)
  if (tex_off == UINT32_MAX) {
    tex_off = 0x025778;
    lg::info("[libsm64] Using fallback texture offset: 0x{:X}", tex_off);
  }

  if (tex_off == UINT32_MAX - 1) {
    // Texture was already decoded directly from a different MIO0 block above.
    lg::info("[libsm64] Shell texture decoded: {}×{} RGBA16 → RGBA8888",
             m_shell_mesh.tex_width, m_shell_mesh.tex_height);
  } else if (tex_off + TEX_BYTES <= seg_data.size()) {
    m_shell_mesh.texture_rgba.resize(TEX_W * TEX_H * 4);
    m_shell_mesh.tex_width  = TEX_W;
    m_shell_mesh.tex_height = TEX_H;
    const uint8_t* tex = seg_data.data() + tex_off;
    for (int i = 0; i < TEX_W * TEX_H; ++i) {
      uint16_t px = (tex[i * 2] << 8) | tex[i * 2 + 1];
      m_shell_mesh.texture_rgba[i * 4 + 0] = static_cast<uint8_t>(((px >> 11) & 0x1F) << 3);
      m_shell_mesh.texture_rgba[i * 4 + 1] = static_cast<uint8_t>(((px >>  6) & 0x1F) << 3);
      m_shell_mesh.texture_rgba[i * 4 + 2] = static_cast<uint8_t>(((px >>  1) & 0x1F) << 3);
      m_shell_mesh.texture_rgba[i * 4 + 3] = (px & 1) ? 0xFF : 0x00;
    }
    lg::info("[libsm64] Shell texture extracted: {}×{} RGBA16 → RGBA8888", TEX_W, TEX_H);
  } else {
    lg::warn("[libsm64] Shell texture offset out of bounds — dome will use vertex colour");
    for (auto& v : m_shell_mesh.vertices) {
      v.u = -1.0f;
      v.v = -1.0f;
    }
  }

  m_shell_mesh.valid = true;

  // ROM bytes are no longer needed.
  m_rom_data.clear();
  m_rom_data.shrink_to_fit();
  return true;
}

void LibSM64Manager::shutdown() {
  if (!m_initialized) return;

  // Stop the audio worker thread first so it can't race against the global
  // terminate below. Destruct before we drop libsm64 state.
  if (m_audio) {
    m_audio->stop();
    m_audio.reset();
  }

  // Drop any tracked actor collision objects before terminating libsm64.
  clear_actor_collision();
  clear_yakow_grab();
  clear_safety_floor();
  m_type_cache = {};
  m_is_process_drawable_cache.clear();
  m_is_collide_shape_cache.clear();
  m_yakow_type = 0;
  m_is_yakow_cache.clear();

  if (m_mario_id >= 0) {
    sm64_mario_delete(m_mario_id);
    m_mario_id = -1;
  }

  sm64_global_terminate();
  m_initialized = false;
  lg::info("[libsm64] Shutdown complete");
}

int32_t LibSM64Manager::create_mario(float x, float y, float z) {
  if (!m_initialized) {
    lg::error("[libsm64] Cannot create Mario: not initialized");
    return -1;
  }

  if (m_mario_id >= 0) {
    sm64_mario_delete(m_mario_id);
  }

  // Tear the old safety floor down before the new spawn so the next tick
  // starts a fresh surface object at the new Mario position. Without this,
  // a respawn into a completely different area would leave the safety
  // surface object at stale XYZ for one frame.
  clear_safety_floor();

  // Reset the lava-entry edge state so a respawn into a dry area doesn't
  // see a stale "was in lava" from the last Mario's death-in-lava frame.
  m_prev_in_lava = false;
  m_in_launcher = false;

  // Convert Jak coordinates to SM64 coordinates
  // Jak: Y-up, same as SM64, but different scale
  float sm64_x = x * JAK_TO_SM64_SCALE;
  float sm64_y = y * JAK_TO_SM64_SCALE;
  float sm64_z = z * JAK_TO_SM64_SCALE;

  m_mario_id = sm64_mario_create(sm64_x, sm64_y, sm64_z);
  if (m_mario_id < 0) {
    lg::error("[libsm64] Failed to create Mario");
    return -1;
  }

  // Seed m_state.position with the spawn position so the very first tick's
  // update_safety_floor call sees the right XYZ. Without this, the first
  // frame's safety quad would land at (0,0,0) minus drop, which is
  // useless for any level whose spawn isn't near the origin.
  {
    std::lock_guard<std::mutex> lock(m_geo_mutex);
    m_state.position = math::Vector3f(x, y, z);
  }

  lg::info("[libsm64] Mario created at ({}, {}, {}) [SM64: ({}, {}, {})]",
           x, y, z, sm64_x, sm64_y, sm64_z);
  return m_mario_id;
}

void LibSM64Manager::delete_mario(int32_t mario_id) {
  if (m_mario_id == mario_id && m_mario_id >= 0) {
    // Release any yakow we're holding before tearing down the Mario instance;
    // otherwise the stale m_grabbed_yakow_ee would leak and the next Mario
    // spawn would start "already holding".
    clear_yakow_grab();
    // Drop the safety floor too so a later respawn starts clean.
    clear_safety_floor();
    sm64_mario_delete(m_mario_id);
    m_mario_id = -1;
  }
}

void LibSM64Manager::tick(const MarioInputState& input) {
  if (!m_initialized || m_mario_id < 0) return;

  // Edge-detect the B (punch/grab) button for update_yakow_grab. Shift the
  // previous frame into _prev first so (_cur && !_prev) becomes a single-
  // frame "just pressed" pulse.
  m_prev_button_b = m_cur_button_b;
  m_cur_button_b = input.button_b;

  SM64MarioInputs sm64_input{};
  sm64_input.camLookX = input.cam_look_x;
  sm64_input.camLookZ = input.cam_look_z;
  sm64_input.stickX = input.stick_x;
  sm64_input.stickY = input.stick_y;
  sm64_input.buttonA = input.button_a ? 1 : 0;
  sm64_input.buttonB = input.button_b ? 1 : 0;
  sm64_input.buttonZ = input.button_z ? 1 : 0;

  SM64MarioState sm64_state{};
  SM64MarioGeometryBuffers sm64_geo{};
  sm64_geo.position = m_tick_position_buf.data();
  sm64_geo.normal = m_tick_normal_buf.data();
  sm64_geo.color = m_tick_color_buf.data();
  sm64_geo.uv = m_tick_uv_buf.data();

  {
    // Serialize against the audio worker thread — libsm64's global state is
    // not reentrant and sm64_audio_tick() runs on cubeb's callback thread.
    std::scoped_lock lock(m_sm64_lock);

    // Reposition the safety floor under Mario BEFORE the tick so libsm64's
    // per-frame floor query (inside perform_ground_quarter_step /
    // perform_air_quarter_step) sees a valid floor beneath him. We use
    // last frame's position — on the very first tick after spawn m_state
    // was seeded to the spawn coords in create_mario, so it's already
    // correct. The quad tracks Mario's XYZ with a fixed Y drop; see
    // update_safety_floor's comment for why this never makes Mario "land"
    // on the pseudo floor during normal play.
    update_safety_floor(m_state.position.x() * JAK_TO_SM64_SCALE,
                        m_state.position.y() * JAK_TO_SM64_SCALE,
                        m_state.position.z() * JAK_TO_SM64_SCALE);

    sm64_mario_tick(m_mario_id, &sm64_input, &sm64_state, &sm64_geo);

    // Post-tick shell-over-water correction.
    // In native SM64 the koopa shell object floats on the water surface and
    // Mario rides it at a stable Y. We don't have that object, so terrain
    // near the waterline can cause Mario's Y to oscillate between the real
    // floor and the water pseudo-floor each frame, producing a visible
    // bounce, unwanted audio, and briefly blocking jumps.
    //
    // Fix: when shell-riding on the ground over water, pin Mario to the
    // water surface. SHELL_JUMP is left alone so the player can still jump.
    // SHELL_FALL over water is caught and reverted to SHELL_GROUND.
    constexpr uint32_t kActRidingShellGround_tick = 0x20810446;
    constexpr uint32_t kActRidingShellFall_tick   = 0x0081089B;
    constexpr uint32_t kActFlagRidingShell_tick   = 0x00010000;

    if (m_in_water_volume && (sm64_state.action & kActFlagRidingShell_tick)) {
      if (sm64_state.action == kActRidingShellGround_tick) {
        // Pin to the water surface every frame while riding on ground.
        sm64_set_mario_position(m_mario_id,
                                sm64_state.position[0],
                                m_water_level_sm64,
                                sm64_state.position[2]);
        sm64_state.position[1] = m_water_level_sm64;
      } else if (sm64_state.action == kActRidingShellFall_tick) {
        // Fell off the pseudo-floor — snap back to the water surface and
        // return to the ground action so the player can jump again.
        sm64_set_mario_position(m_mario_id,
                                sm64_state.position[0],
                                m_water_level_sm64,
                                sm64_state.position[2]);
        sm64_state.position[1] = m_water_level_sm64;

        if (sm64_state.velocity[1] < 0.0f) {
          sm64_set_mario_velocity(m_mario_id,
                                  sm64_state.velocity[0],
                                  0.0f,
                                  sm64_state.velocity[2]);
          sm64_state.velocity[1] = 0.0f;
        }

        sm64_set_mario_action_arg(m_mario_id, kActRidingShellGround_tick, 1);
        sm64_state.action = kActRidingShellGround_tick;
      }
      // SHELL_JUMP: don't touch — let the player jump freely.
    }

    // --- Launcher glue: override Mario's position with Jak's launch pos ---
    // m_in_launcher is set by update_launcher_glue (runs before tick).
    if (m_in_launcher) {
      float lx = m_launcher_target_jak.x() * JAK_TO_SM64_SCALE;
      float ly = m_launcher_target_jak.y() * JAK_TO_SM64_SCALE;
      float lz = m_launcher_target_jak.z() * JAK_TO_SM64_SCALE;

      sm64_set_mario_position(m_mario_id, lx, ly, lz);
      sm64_state.position[0] = lx;
      sm64_state.position[1] = ly;
      sm64_state.position[2] = lz;
    }
  }

  // Copy results into our managed buffers (threadsafe)
  std::lock_guard<std::mutex> lock(m_geo_mutex);

  m_geometry.num_triangles = sm64_geo.numTrianglesUsed;
  int num_verts = sm64_geo.numTrianglesUsed * 3;

  m_geometry.position.resize(num_verts * 3);
  m_geometry.normal.resize(num_verts * 3);
  m_geometry.color.resize(num_verts * 3);
  m_geometry.uv.resize(num_verts * 2);

  // Copy and scale positions from SM64 to Jak coordinate space
  for (int i = 0; i < num_verts * 3; i++) {
    m_geometry.position[i] = m_tick_position_buf[i] * SM64_TO_JAK_SCALE;
  }
  std::memcpy(m_geometry.normal.data(), m_tick_normal_buf.data(), num_verts * 3 * sizeof(float));
  std::memcpy(m_geometry.color.data(), m_tick_color_buf.data(), num_verts * 3 * sizeof(float));
  std::memcpy(m_geometry.uv.data(), m_tick_uv_buf.data(), num_verts * 2 * sizeof(float));

  // Update state with Jak-scale coordinates
  m_state.position = math::Vector3f(
      sm64_state.position[0] * SM64_TO_JAK_SCALE,
      sm64_state.position[1] * SM64_TO_JAK_SCALE,
      sm64_state.position[2] * SM64_TO_JAK_SCALE);
  m_state.velocity = math::Vector3f(
      sm64_state.velocity[0] * SM64_TO_JAK_SCALE,
      sm64_state.velocity[1] * SM64_TO_JAK_SCALE,
      sm64_state.velocity[2] * SM64_TO_JAK_SCALE);
  m_state.face_angle = sm64_state.faceAngle;
  m_state.forward_velocity = sm64_state.forwardVelocity * SM64_TO_JAK_SCALE;
  m_state.health = sm64_state.health;
  m_state.action = sm64_state.action;
  m_state.flags = sm64_state.flags;
  m_state.anim_id = sm64_state.animID;
  m_state.anim_frame = sm64_state.animFrame;

  // ---- Ground-pound hitbox simulation -------------------------------------
  // Replicates SM64's INT_GROUND_POUND_OR_TWIRL classification from
  // libsm64/src/decomp/game/interaction.c::determine_interaction. We don't
  // have access to libsm64's per-object collision pool (Jak actors are surface
  // objects, not behavior objects), so we evaluate the same hitbox geometry
  // here in C++ against our tracked Jak actors.
  //
  // Constants come from libsm64/src/decomp/game/object_stuff.c (Mario hitbox
  // radius = 37, height = 160, downOffset = 0) and the action IDs from
  // libsm64/src/decomp/include/sm64.h.
  constexpr uint32_t kActGroundPound = 0x008008A9;       // ACT_GROUND_POUND
  constexpr uint32_t kActGroundPoundLand = 0x0080023C;   // ACT_GROUND_POUND_LAND
  constexpr float kMarioHitboxRadiusSm64 = 37.0f;
  constexpr float kMarioHitboxHeightSm64 = 160.0f;

  // Reset per-frame flags. We do NOT reset frames_active or total_hits — those
  // accumulate across the lifetime so the debug GUI can show them. hits_this_frame
  // is overwritten by update_actor_collision when it runs the hit pass.
  bool active = false;
  bool impact = false;
  if (sm64_state.action == kActGroundPound) {
    // INT_GROUND_POUND_OR_TWIRL when vel.y < 0. In actionState 0 vel.y is set
    // to -50 every frame; in actionState 1 air_step preserves it. So just check.
    if (sm64_state.velocity[1] < 0.0f) {
      active = true;
    }
  } else if (sm64_state.action == kActGroundPoundLand) {
    // ACT_GROUND_POUND_LAND's handler immediately sets actionState=1 on entry,
    // so the "actionState == 0" window in determine_interaction is exactly the
    // first frame Mario is in this action. We detect that via prev_action edge.
    if (m_prev_action != kActGroundPoundLand) {
      active = true;
      impact = true;
    }
  }

  m_gp_hitbox.active = active;
  m_gp_hitbox.impact_frame = impact;
  m_gp_hitbox.center = m_state.position;  // already in Jak units, Mario's feet
  m_gp_hitbox.radius = kMarioHitboxRadiusSm64 * SM64_TO_JAK_SCALE;
  m_gp_hitbox.bottom_y = m_state.position.y();
  m_gp_hitbox.top_y = m_state.position.y() + kMarioHitboxHeightSm64 * SM64_TO_JAK_SCALE;
  if (active) {
    m_gp_hitbox.frames_active++;
  }
  if (!active) {
    // Once the pound finishes, clear the per-frame hit count so the GUI shows 0.
    m_gp_hitbox.hits_this_frame = 0;
  }

  // ---- Fire-action handling (hot coals / lava) ---------------------------
  // When Mario touches a SURFACE_BURNING triangle (hot coals or lava in our
  // Jak level-collision filter), libsm64 routes him through two paths:
  //
  //   1. Grounded contact → interaction.c::check_lava_boost() which calls
  //      drop_and_set_mario_action(m, ACT_LAVA_BOOST, 0) and adds 12/18 to
  //      Mario's hurtCounter.
  //   2. Airborne wall hit → mario_actions_airborne.c::lava_boost_on_wall()
  //      which also sets ACT_LAVA_BOOST (with actionArg=1) after returning
  //      AIR_STEP_HIT_LAVA_WALL from mario_step.c.
  //
  // Stock SM64's act_lava_boost at mario_actions_airborne.c:1568-1570 checks
  // `if (m->health < 0x100) level_trigger_warp(m, WARP_OP_DEATH);` — i.e.,
  // it tries to warp Mario to the level death spawn when his health drops.
  // libsm64 stubs warps out (there's no level to reload), so Mario's native
  // lava death NEVER fires. A 0-wedge Mario would bounce forever on fire.
  //
  // Behavior: if Mario is in any fire action AND has 0 wedges, we force him
  // into ACT_IDLE. He stops burning, stands still on the hot surface, and
  // the lava boost loop breaks. (Next frame the lava check might re-trigger
  // since he's still physically on the burning tri, so we keep forcing idle
  // — which has the visual effect of him standing motionless and smoking.)
  //
  // We watch ACT_LAVA_BOOST / ACT_LAVA_BOOST_LAND (the real burning actions
  // libsm64 uses) AND the three ACT_BURNING_* actions (stock SM64 entries
  // from fire-piranha enemies that libsm64 doesn't ship — kept for defense
  // in depth). Health in SM64 is laid out as 0xHHSS where HH's low nibble
  // is the wedge count, so `wedges` = (health >> 8) & F.
  constexpr uint32_t kActLavaBoost = 0x010208B7;
  constexpr uint32_t kActLavaBoostLand = 0x08000239;
  constexpr uint32_t kActBurningGround = 0x00020449;
  constexpr uint32_t kActBurningJump = 0x010208B4;
  constexpr uint32_t kActBurningFall = 0x010208B5;
  constexpr uint32_t kActIdle = 0x0C400201;
  auto is_fire_action = [&](uint32_t a) {
    return a == kActLavaBoost || a == kActLavaBoostLand ||
           a == kActBurningGround || a == kActBurningJump || a == kActBurningFall;
  };
  const bool is_burning = is_fire_action(sm64_state.action);
  const bool was_burning = is_fire_action(m_prev_action);
  const uint32_t wedges = (static_cast<uint32_t>(sm64_state.health) >> 8) & 0xF;

  // Diagnostic: log every edge into / out of a fire action so we can see
  // both (a) whether Mario actually enters a burning action when stepping
  // on a Jak hot-surface tri, and (b) what his health is at that moment.
  // This is critical for debugging — if these lines never fire, the
  // SURFACE_BURNING tagging in load_level_collision isn't reaching Mario.
  if (is_burning && !was_burning) {
    lg::info(
        "[libsm64] Mario entered fire action 0x{:08X} (health=0x{:04X}, {}/8 wedges, "
        "prev=0x{:08X})",
        sm64_state.action, sm64_state.health, wedges, m_prev_action);
  }
  if (!is_burning && was_burning) {
    lg::info("[libsm64] Mario left fire action 0x{:08X} -> 0x{:08X} (health=0x{:04X}, {}/8)",
             m_prev_action, sm64_state.action, sm64_state.health, wedges);
  }

  if (is_burning && wedges == 0) {
    {
      std::scoped_lock lock(m_sm64_lock);
      sm64_set_mario_action(m_mario_id, kActIdle);
    }
    if (!was_burning) {
      lg::info(
          "[libsm64] Burning at 0/8 wedges — forcing Mario to ACT_IDLE (was 0x{:08X}, health=0x{:04X})",
          sm64_state.action, sm64_state.health);
    }
  }

  m_prev_action = sm64_state.action;
}

GroundPoundHitbox LibSM64Manager::get_ground_pound_hitbox() {
  std::lock_guard<std::mutex> lock(m_geo_mutex);
  return m_gp_hitbox;
}

// Pure geometry test, exposed for unit tests. Both inputs in Jak units. The
// 2D check is `dx² + dz² < (hb.radius + actor_radius)²`. The Y check inflates
// the cylinder by the actor's half-height on both ends to model the actor as a
// vertical capsule rather than a point.
bool ground_pound_hitbox_overlaps(const GroundPoundHitbox& hb,
                                  const math::Vector3f& actor_pos,
                                  float actor_radius,
                                  float actor_half_height) {
  if (!hb.active) return false;
  float dx = actor_pos.x() - hb.center.x();
  float dz = actor_pos.z() - hb.center.z();
  float r = hb.radius + actor_radius;
  if (dx * dx + dz * dz > r * r) return false;
  float ay = actor_pos.y();
  if (ay + actor_half_height < hb.bottom_y) return false;
  if (ay - actor_half_height > hb.top_y) return false;
  return true;
}

bool ground_pound_hitbox_overlaps_aabb(const GroundPoundHitbox& hb,
                                        const float aabb_min[3],
                                        const float aabb_max[3]) {
  if (!hb.active) return false;
  // Y interval test.
  if (aabb_max[1] < hb.bottom_y) return false;
  if (aabb_min[1] > hb.top_y) return false;
  // XZ: closest point on the AABB rectangle to the hitbox center.
  float cx = hb.center.x();
  float cz = hb.center.z();
  float closest_x = cx < aabb_min[0] ? aabb_min[0] : (cx > aabb_max[0] ? aabb_max[0] : cx);
  float closest_z = cz < aabb_min[2] ? aabb_min[2] : (cz > aabb_max[2] ? aabb_max[2] : cz);
  float dx = closest_x - cx;
  float dz = closest_z - cz;
  return dx * dx + dz * dz <= hb.radius * hb.radius;
}

void LibSM64Manager::load_flat_ground(float y_height, float half_extent) {
  if (!m_initialized) return;

  float e = half_extent * JAK_TO_SM64_SCALE;
  int32_t y = static_cast<int32_t>(y_height * JAK_TO_SM64_SCALE);

  // Two triangles forming a flat ground plane
  SM64Surface surfaces[2];
  std::memset(surfaces, 0, sizeof(surfaces));

  // Triangle 1
  surfaces[0].type = 0x0000;  // SURFACE_DEFAULT
  surfaces[0].force = 0;
  surfaces[0].terrain = 0x0001;  // TERRAIN_STONE
  surfaces[0].vertices[0][0] = static_cast<int32_t>(-e);
  surfaces[0].vertices[0][1] = y;
  surfaces[0].vertices[0][2] = static_cast<int32_t>(-e);
  surfaces[0].vertices[1][0] = static_cast<int32_t>(e);
  surfaces[0].vertices[1][1] = y;
  surfaces[0].vertices[1][2] = static_cast<int32_t>(-e);
  surfaces[0].vertices[2][0] = static_cast<int32_t>(-e);
  surfaces[0].vertices[2][1] = y;
  surfaces[0].vertices[2][2] = static_cast<int32_t>(e);

  // Triangle 2
  surfaces[1].type = 0x0000;
  surfaces[1].force = 0;
  surfaces[1].terrain = 0x0001;
  surfaces[1].vertices[0][0] = static_cast<int32_t>(e);
  surfaces[1].vertices[0][1] = y;
  surfaces[1].vertices[0][2] = static_cast<int32_t>(e);
  surfaces[1].vertices[1][0] = static_cast<int32_t>(-e);
  surfaces[1].vertices[1][1] = y;
  surfaces[1].vertices[1][2] = static_cast<int32_t>(e);
  surfaces[1].vertices[2][0] = static_cast<int32_t>(e);
  surfaces[1].vertices[2][1] = y;
  surfaces[1].vertices[2][2] = static_cast<int32_t>(-e);

  sm64_static_surfaces_load(surfaces, 2);
  lg::info("[libsm64] Loaded flat ground at y={} extent={}", y_height, half_extent);
}

void LibSM64Manager::update_safety_floor(float mario_x_sm64,
                                         float mario_y_sm64,
                                         float mario_z_sm64) {
  // Lazy-create a 300x300 SM64u quad as a libsm64 surface object, then
  // move it each tick so it sits exactly `safety_floor_drop_sm64` units
  // below Mario. Purely there so find_floor never returns NULL.
  //
  // The surfaces array is declared in LOCAL space (centered at origin) so
  // sm64_surface_object_move can translate it without us rebuilding geom.
  // Winding matches load_flat_ground (cw from above → upward normal in
  // SM64's left-handed world space, i.e. a floor Mario can stand on).
  if (!m_initialized || !safety_floor) return;
  if (m_mario_id < 0) return;

  // Half-extent of the safety quad in SM64 units. 150 → 300x300 SM64u.
  constexpr int32_t kSafetyHalfExtent = 150;

  // Compute world-space Y for the quad this frame. The quad tracks Mario
  // with a small fixed drop.
  const float safety_y_sm64 = mario_y_sm64 - safety_floor_drop_sm64;

  if (!m_safety_floor_created) {
    SM64Surface surfaces[2];
    std::memset(surfaces, 0, sizeof(surfaces));

    // CRITICAL: SM64 computes the surface normal as (v2-v1) x (v3-v2) and
    // find_floor_from_list rejects anything with normal.y <= 0.01. So
    // triangles need to wind in the order that gives +Y normal. For a
    // flat XZ quad on y=0, this means picking v2 and v3 such that going
    // v1→v2→v3 looks CCW when viewed from BELOW (i.e. CW from above in
    // SM64's left-handed Y-up space). Empirically verified by the ny
    // formula below:
    //   ny = (z2-z1)*(x3-x2) - (x2-x1)*(z3-z2) > 0  →  floor
    //   ny < 0                                       →  ceiling (rejected)
    //
    // Triangle 1: v1=(-e,0,-e), v2=(-e,0,e), v3=(e,0,-e)
    //   ny = (e - (-e))*(e - (-e)) - ((-e) - (-e))*((-e) - e)
    //      = (2e)(2e) - (0)(-2e) = 4e² > 0 ✓
    surfaces[0].type = 0x0000;       // SURFACE_DEFAULT
    surfaces[0].force = 0;
    surfaces[0].terrain = 0x0001;    // TERRAIN_STONE
    surfaces[0].vertices[0][0] = -kSafetyHalfExtent;
    surfaces[0].vertices[0][1] = 0;  // local Y — world Y comes from transform
    surfaces[0].vertices[0][2] = -kSafetyHalfExtent;
    surfaces[0].vertices[1][0] = -kSafetyHalfExtent;
    surfaces[0].vertices[1][1] = 0;
    surfaces[0].vertices[1][2] = kSafetyHalfExtent;
    surfaces[0].vertices[2][0] = kSafetyHalfExtent;
    surfaces[0].vertices[2][1] = 0;
    surfaces[0].vertices[2][2] = -kSafetyHalfExtent;

    // Triangle 2: v1=(e,0,-e), v2=(-e,0,e), v3=(e,0,e)
    //   ny = (e - (-e))*(e - (-e)) - ((-e) - e)*(e - e)
    //      = (2e)(2e) - (-2e)(0) = 4e² > 0 ✓
    surfaces[1].type = 0x0000;
    surfaces[1].force = 0;
    surfaces[1].terrain = 0x0001;
    surfaces[1].vertices[0][0] = kSafetyHalfExtent;
    surfaces[1].vertices[0][1] = 0;
    surfaces[1].vertices[0][2] = -kSafetyHalfExtent;
    surfaces[1].vertices[1][0] = -kSafetyHalfExtent;
    surfaces[1].vertices[1][1] = 0;
    surfaces[1].vertices[1][2] = kSafetyHalfExtent;
    surfaces[1].vertices[2][0] = kSafetyHalfExtent;
    surfaces[1].vertices[2][1] = 0;
    surfaces[1].vertices[2][2] = kSafetyHalfExtent;

    SM64SurfaceObject obj{};
    obj.transform.position[0] = mario_x_sm64;
    obj.transform.position[1] = safety_y_sm64;
    obj.transform.position[2] = mario_z_sm64;
    obj.transform.eulerRotation[0] = 0.0f;
    obj.transform.eulerRotation[1] = 0.0f;
    obj.transform.eulerRotation[2] = 0.0f;
    obj.surfaceCount = 2;
    obj.surfaces = surfaces;

    m_safety_floor_id = sm64_surface_object_create(&obj);
    m_safety_floor_created = true;
    lg::info(
        "[libsm64] Safety floor created at Mario XYZ=({:.0f}, {:.0f}, {:.0f}) "
        "safetyY={:.0f} drop={:.0f} (id={}, extent={} SM64u)",
        mario_x_sm64, mario_y_sm64, mario_z_sm64, safety_y_sm64,
        safety_floor_drop_sm64, m_safety_floor_id, kSafetyHalfExtent * 2);
    return;
  }

  // Subsequent frames: just translate to Mario's new XYZ (minus the drop).
  // SM64's surface-object move path also derives a platform velocity from
  // the delta, which is fine here — the safety quad mirrors Mario's own
  // velocity, so the "platform pushing Mario" path cancels out any tiny
  // relative motion even if Mario were briefly standing on it.
  SM64ObjectTransform xform{};
  xform.position[0] = mario_x_sm64;
  xform.position[1] = safety_y_sm64;
  xform.position[2] = mario_z_sm64;
  xform.eulerRotation[0] = 0.0f;
  xform.eulerRotation[1] = 0.0f;
  xform.eulerRotation[2] = 0.0f;
  sm64_surface_object_move(m_safety_floor_id, &xform);
}

void LibSM64Manager::clear_safety_floor() {
  if (!m_safety_floor_created) return;
  // sm64_surface_object_delete touches libsm64 global state; callers
  // (shutdown/delete_mario/create_mario on respawn) must guarantee thread
  // safety — shutdown serializes against the audio thread higher up, and
  // create_mario / delete_mario run on the main game thread.
  sm64_surface_object_delete(m_safety_floor_id);
  m_safety_floor_id = 0;
  m_safety_floor_created = false;
}

void LibSM64Manager::load_surfaces(const std::vector<SM64Surface>& surfaces) {
  if (!m_initialized || surfaces.empty()) return;
  sm64_static_surfaces_load(surfaces.data(), static_cast<uint32_t>(surfaces.size()));
  m_loaded_surface_count = static_cast<int>(surfaces.size());
  lg::info("[libsm64] Loaded {} collision surfaces", surfaces.size());
}

void LibSM64Manager::load_level_collision(
    const std::vector<tfrag3::CollisionMesh::Vertex>& vertices) {
  if (!m_initialized) return;
  if (vertices.size() < 3) {
    lg::warn("[libsm64] No collision vertices to load");
    return;
  }

  // Jak bakes "camera-only" collision (invisible walls that only block the
  // camera) into the level collision mesh with the PAT `noentity` bit set.
  // At runtime Jak passes a `pat-ignore-mask` to collide queries — entity
  // queries use (pat-surface :noentity #x1) to reject those tris, and camera
  // queries use (pat-surface :nocamera #x1) to reject tris that are entity-
  // only. Mario acts like an entity, so we need to do the same filter here
  // or he'll trip over invisible slabs that the real player can walk through.
  //
  // pat-surface layout (see goal_src/jak1/engine/collide/pat-h.gc):
  //   bit 0     = noentity      ← skip for player/Mario
  //   bit 1     = nocamera
  //   bit 2     = noedge
  //   bits 3-5  = mode
  //   bits 6-11 = material (6 bits, values from the pat-material enum)
  //   bit 12    = nolineofsight
  //
  // Relevant pat-material values for hot surfaces:
  //   11 = hotcoals   (fire canyon warm rock, lavatube ledges)
  //   12 = lava       (actual magma in lavatube / firecanyon / citadel)
  //
  // The per-triangle PAT lives on every CollisionMesh::Vertex (all 3 verts
  // of a tri share the same value) — we check vertex 0 per tri.
  constexpr uint32_t PAT_NOENTITY_BIT = 0x1;
  constexpr uint32_t PAT_MATERIAL_SHIFT = 6;
  constexpr uint32_t PAT_MATERIAL_MASK = 0x3F;
  constexpr uint32_t PAT_MAT_HOTCOALS = 11;
  constexpr uint32_t PAT_MAT_LAVA = 12;

  // SM64 surface type that triggers the classic "burn your butt" launch —
  // when Mario touches a floor with this type, his butt catches fire and
  // SM64 bumps him into ACT_BURNING_JUMP / ACT_BURNING_FALL. See
  // third-party/libsm64/src/decomp/include/surface_terrains.h:6.
  constexpr int16_t SURFACE_BURNING_TYPE = 0x0001;

  size_t num_tris = vertices.size() / 3;
  std::vector<SM64Surface> surfaces;
  surfaces.reserve(num_tris);
  size_t skipped_noentity = 0;
  size_t burning_tris = 0;

  for (size_t i = 0; i < num_tris; i++) {
    const auto& v0 = vertices[i * 3 + 0];
    if (v0.pat & PAT_NOENTITY_BIT) {
      // Camera-only collision — Mario ignores it, same as Jak does.
      skipped_noentity++;
      continue;
    }

    SM64Surface surf;
    const uint32_t material = (v0.pat >> PAT_MATERIAL_SHIFT) & PAT_MATERIAL_MASK;
    if (material == PAT_MAT_HOTCOALS || material == PAT_MAT_LAVA) {
      // Hot surface — SM64 will launch Mario with the butt-on-fire action.
      surf.type = SURFACE_BURNING_TYPE;
      burning_tris++;
    } else {
      surf.type = 0x0000;    // SURFACE_DEFAULT
    }
    surf.force = 0;
    surf.terrain = 0x0001;  // TERRAIN_STONE

    for (int v = 0; v < 3; v++) {
      const auto& vert = vertices[i * 3 + v];
      // Jak positions are in meters, SM64 expects its own units (43x scale)
      surf.vertices[v][0] = static_cast<int32_t>(vert.x * JAK_TO_SM64_SCALE);
      surf.vertices[v][1] = static_cast<int32_t>(vert.y * JAK_TO_SM64_SCALE);
      surf.vertices[v][2] = static_cast<int32_t>(vert.z * JAK_TO_SM64_SCALE);
    }
    surfaces.push_back(surf);
  }

  if (surfaces.empty()) {
    lg::warn("[libsm64] All {} level triangles were noentity — nothing to load", num_tris);
    return;
  }

  sm64_static_surfaces_load(surfaces.data(), static_cast<uint32_t>(surfaces.size()));
  m_loaded_surface_count = static_cast<int>(surfaces.size());
  lg::info(
      "[libsm64] Loaded {} collision surfaces from level geometry ({} noentity skipped, {} burning)",
      surfaces.size(), skipped_noentity, burning_tris);
}

bool LibSM64Manager::write_mario_pos_to_target(u8* ee_mem,
                                                u32 ee_mem_size,
                                                u32 false_val,
                                                u32 target_ptr,
                                                const math::Vector3f& mario_pos) {
  if (!ee_mem || target_ptr == 0 || target_ptr == false_val) return false;

  // process-drawable.root is declared at GOAL :offset 112, but for boxed (basic) types
  // the runtime offset is (declared - 4) — see goalc/compiler/compilation/Type.cpp:1632.
  // So root lives at target_ptr + 108. Reading 4 bytes there → need target_ptr + 112 in bounds.
  constexpr u32 ROOT_RUNTIME_OFF = 108;
  if (target_ptr + ROOT_RUNTIME_OFF + 4 > ee_mem_size) {
    lg::warn("[libsm64] write: target_ptr 0x{:X} + {} > mem_size 0x{:X}",
             target_ptr, ROOT_RUNTIME_OFF + 4, ee_mem_size);
    return false;
  }

  u32 root_ptr;
  std::memcpy(&root_ptr, ee_mem + target_ptr + ROOT_RUNTIME_OFF, 4);
  if (root_ptr == 0 || root_ptr == false_val) {
    lg::warn("[libsm64] write: root_ptr 0x{:X} is null or #f", root_ptr);
    return false;
  }

  // trs.trans is declared at GOAL :offset 16 → runtime offset 12 (boxed adjustment).
  // The vector is 16 bytes (x,y,z,w floats).
  constexpr u32 TRANS_RUNTIME_OFF = 12;
  if (root_ptr + TRANS_RUNTIME_OFF + 16 > ee_mem_size) {
    lg::warn("[libsm64] write: root_ptr 0x{:X} + {} > mem_size 0x{:X}",
             root_ptr, TRANS_RUNTIME_OFF + 16, ee_mem_size);
    return false;
  }

  float trans[4];
  trans[0] = mario_pos.x();
  trans[1] = mario_pos.y();
  trans[2] = mario_pos.z();
  trans[3] = 1.0f;

  // Log the first few writes for debugging
  static int write_count = 0;
  if (write_count < 3) {
    float existing[4];
    std::memcpy(existing, ee_mem + root_ptr + TRANS_RUNTIME_OFF, 16);
    lg::info("[libsm64] write #{}: root=0x{:X}, existing=({}, {}, {}, {}), new=({}, {}, {})",
             write_count, root_ptr,
             existing[0], existing[1], existing[2], existing[3],
             trans[0], trans[1], trans[2]);
    write_count++;
  }

  std::memcpy(ee_mem + root_ptr + TRANS_RUNTIME_OFF, trans, 16);
  return true;
}

void LibSM64Manager::resolve_target_symbol() {
  // Resolves and caches the *target* symbol offset.
  // Safe to call from any thread — intern_from_c only reads for existing symbols.
  if (m_cached_target_sym_offset != 0) return;

  u32 false_val = s7.offset;
  if (false_val == 0) {
    lg::warn("[libsm64] resolve_target_symbol: s7 not set yet");
    return;
  }

  auto target_sym = jak1::intern_from_c("*target*");
  if (target_sym.offset != 0) {
    m_cached_target_sym_offset = target_sym.offset;
    lg::info("[libsm64] Cached *target* symbol at offset 0x{:X}", m_cached_target_sym_offset);
  } else {
    lg::warn("[libsm64] resolve_target_symbol: could not find *target*");
  }
}

void LibSM64Manager::sync_jak_to_mario(u8* ee_mem, u32 s7_offset) {
  if (!m_initialized || m_mario_id < 0 || !ee_mem) return;

  u32 false_val = s7.offset;
  if (false_val == 0) return;

  // Look up *target* directly via kernel (called every tick, but only a read)
  auto target_sym = jak1::intern_from_c("*target*");
  if (target_sym.offset == 0) return;
  u32 target_ptr = target_sym->value;
  if (target_ptr == 0 || target_ptr == false_val) return;

  auto mario_pos = get_state().position;
  write_mario_pos_to_target(ee_mem, EE_MAIN_MEM_SIZE, false_val, target_ptr, mario_pos);
}

bool LibSM64Manager::read_target_transform(u8* ee_mem,
                                           math::Vector3f* out_pos,
                                           float* out_yaw_rad) {
  if (!ee_mem) return false;
  u32 false_val = s7.offset;
  if (false_val == 0) return false;

  auto target_sym = jak1::intern_from_c("*target*");
  if (target_sym.offset == 0) return false;
  u32 target_ptr = target_sym->value;
  if (target_ptr == 0 || target_ptr == false_val) return false;

  // Same offset walk as write_mario_pos_to_target: process-drawable.root is
  // declared at GOAL :offset 112 → runtime 108, then trsqv.trans at GOAL
  // :offset 16 → runtime 12, and quat overlays rot.x at GOAL :offset 32 →
  // runtime 28 (16 bytes, x/y/z/w floats).
  constexpr u32 ROOT_RUNTIME_OFF = 108;
  constexpr u32 TRANS_RUNTIME_OFF = 12;
  constexpr u32 QUAT_RUNTIME_OFF = 28;
  if (target_ptr + ROOT_RUNTIME_OFF + 4 > EE_MAIN_MEM_SIZE) return false;

  u32 root_ptr;
  std::memcpy(&root_ptr, ee_mem + target_ptr + ROOT_RUNTIME_OFF, 4);
  if (root_ptr == 0 || root_ptr == false_val) return false;
  if (root_ptr + QUAT_RUNTIME_OFF + 16 > EE_MAIN_MEM_SIZE) return false;

  float trans[4];
  std::memcpy(trans, ee_mem + root_ptr + TRANS_RUNTIME_OFF, 16);
  float quat[4];  // x, y, z, w
  std::memcpy(quat, ee_mem + root_ptr + QUAT_RUNTIME_OFF, 16);

  if (out_pos) {
    *out_pos = math::Vector3f(trans[0], trans[1], trans[2]);
  }
  if (out_yaw_rad) {
    // Extract Y-axis yaw from the quaternion. Using the full formula so it
    // stays well-defined even if the Jak player picks up some roll/pitch.
    const float x = quat[0];
    const float y = quat[1];
    const float z = quat[2];
    const float w = quat[3];
    *out_yaw_rad = std::atan2(2.0f * (w * y + x * z),
                              1.0f - 2.0f * (y * y + x * x));
  }
  return true;
}

void LibSM64Manager::set_mario_face_angle(float yaw_rad) {
  if (!m_initialized || m_mario_id < 0) return;
  std::scoped_lock lock(m_sm64_lock);
  sm64_set_mario_faceangle(m_mario_id, yaw_rad);
}

void LibSM64Manager::update_mario_water(u8* ee_mem) {
  if (!m_initialized || m_mario_id < 0 || !water_sync || !ee_mem) return;
  u32 false_val = s7.offset;
  if (false_val == 0) return;

  auto target_sym = jak1::intern_from_c("*target*");
  if (target_sym.offset == 0) return;
  u32 target_ptr = target_sym->value;
  if (target_ptr == 0 || target_ptr == false_val) return;

  // process-drawable field layout from goal_src/jak1/engine/game/game-h.gc:
  //   root(112), node-list(116), draw(120), skel(124), nav(128), align(132),
  //   path(136), vol(140), fact(144), link(148), part(152), water(156).
  // All 4-byte basic pointers; subtract 4 for runtime offsets.
  constexpr u32 WATER_FIELD_RUNTIME_OFF = 152;
  if (target_ptr + WATER_FIELD_RUNTIME_OFF + 4 > EE_MAIN_MEM_SIZE) return;

  u32 water_ctrl_ptr;
  std::memcpy(&water_ctrl_ptr, ee_mem + target_ptr + WATER_FIELD_RUNTIME_OFF, 4);
  if (water_ctrl_ptr == 0 || water_ctrl_ptr == false_val) {
    // No water-control allocated — leave the level way below Mario so he's dry.
    std::scoped_lock lock(m_sm64_lock);
    sm64_set_mario_water_level(m_mario_id, -100000);
    return;
  }

  // water-control layout (basic, from water-h.gc). Offsets are computed
  // sequentially from the declared field list, accounting for 8-byte
  // alignment on the time-frame (int64) members:
  //   0   flags        (u32)
  //   4   process      (basic ptr)
  //   8   joint-index  (i32)
  //  12   top-y-offset (f32)
  //  16   ripple-size  (f32)
  //  20   enter-water-time  (i64)
  //  28   wade-time         (i64)
  //  36   on-water-time     (i64)
  //  44   enter-swim-time   (i64)
  //  52   swim-time         (i64)
  //  60   base-height       (f32)
  //  64   wade-height       (f32)
  //  68   swim-height       (f32)
  //  72   surface-height    (f32)
  //  76   bottom-height     (f32)
  //  80   height            (f32) <-- this is what water.gc computes each tick
  constexpr u32 WC_FLAGS_OFF = 0;
  constexpr u32 WC_HEIGHT_OFF = 80;
  if (water_ctrl_ptr + WC_HEIGHT_OFF + 4 > EE_MAIN_MEM_SIZE) return;

  uint32_t flags;
  std::memcpy(&flags, ee_mem + water_ctrl_ptr + WC_FLAGS_OFF, 4);
  // wt09 (bit 9) = "target is inside a water volume"; see water.gc:866.
  const bool in_water = (flags & (1u << 9)) != 0;
  // wt25 (bit 25) is set by every lava water-vol in Jak 1: villagec-lava
  // (village3-obs.gc:69), ogre-lava (ogre-obs.gc:1022) and lavatube-lava
  // (lavatube-obs.gc:1023) all `(logior! flags (water-flags wt25))` inside
  // water-vol-method-22 right after clearing wt23 (the "swimmable" bit).
  // water-vol::update! propagates its flags into the target's water-control
  // via `(logior! (-> s5-0 flags) (-> this flags))` (water.gc:958), so
  // reading wt25 on the water-control here reliably detects "Mario is
  // submerged in lava right now".
  const bool is_lava_volume = (flags & (1u << 25)) != 0;
  const bool in_lava = in_water && is_lava_volume;

  // Snapshot current action/health under the geo mutex so we can decide
  // whether we already kicked Mario into a fire action on a previous frame
  // (in which case we shouldn't re-kick every tick — that would freeze the
  // upward launch in place). Values reflect the state libsm64 left after the
  // last sm64_mario_tick, which is exactly what we want for the edge check.
  uint32_t current_action = 0;
  int16_t current_health = 0x880;  // 8 wedges default
  {
    std::lock_guard<std::mutex> g(m_geo_mutex);
    current_action = m_state.action;
    current_health = m_state.health;
  }

  // ACT_ constants lifted from libsm64/src/decomp/include/sm64.h.
  constexpr uint32_t kActLavaBoost = 0x010208B7;
  constexpr uint32_t kActLavaBoostLand = 0x08000239;
  constexpr uint32_t kActBurningGround = 0x00020449;
  constexpr uint32_t kActBurningJump = 0x010208B4;
  constexpr uint32_t kActBurningFall = 0x010208B5;
  const bool already_burning =
      current_action == kActLavaBoost || current_action == kActLavaBoostLand ||
      current_action == kActBurningGround || current_action == kActBurningJump ||
      current_action == kActBurningFall;

  // Shell-riding immunity to match native libsm64: check_lava_boost in
  // interaction.c:902 no-ops when `m->action & ACT_FLAG_RIDING_SHELL`, so
  // Mario on a Koopa shell can cruise through lava unharmed. We mirror
  // that here so the lava rocks in Fire Canyon / Lava Tube don't kick
  // Mario into ACT_LAVA_BOOST while Jak is on the zoomer (which forces
  // Mario into ACT_RIDING_SHELL_GROUND via update_zoomer_shell).
  //
  // ACT_FLAG_RIDING_SHELL = 0x00010000 — set in the action-ID bitfield of
  // all three shell actions (ground, jump, fall). Testing the bit covers
  // every shell variant without an explicit action enumeration.
  constexpr uint32_t kActFlagRidingShell = 0x00010000;
  const bool riding_shell = (current_action & kActFlagRidingShell) != 0;

  // Decide the SM64 water level to feed libsm64 this tick.
  int sm64_water_level;
  if (in_water && !is_lava_volume) {
    float water_y_jak;
    std::memcpy(&water_y_jak, ee_mem + water_ctrl_ptr + WC_HEIGHT_OFF, 4);
    // libsm64 stores waterLevel in SM64 units, same space as Mario's position.
    sm64_water_level = static_cast<int>(water_y_jak * JAK_TO_SM64_SCALE);
    // Publish for the post-tick shell-over-water correction in tick().
    m_in_water_volume = true;
    m_water_level_sm64 = water_y_jak * JAK_TO_SM64_SCALE;
  } else {
    // Dry or lava: keep the SM64 water level far below Mario so libsm64
    // never puts him into ACT_WATER_IDLE / swim state.
    sm64_water_level = -100000;
    m_in_water_volume = false;
  }

  // Edge-triggered re-entry: fire a fresh kick if Mario JUST crossed into
  // the lava volume this frame, even when already_burning is true.
  //
  // Why we need this: a single ACT_LAVA_BOOST launches Mario with vel[1]=84
  // and runs through the air step until he lands. While airborne he's still
  // "already burning", so the simple !already_burning gate blocks a second
  // kick. But the arc takes him ABOVE the lava surface (in_lava briefly
  // false) and then back DOWN through it (in_lava true again). Natively
  // SM64 handles this re-bounce via `if (m->floor->type == SURFACE_BURNING)`
  // inside act_lava_boost — but our Jak level tris are all SURFACE_DEFAULT,
  // so the native loop never fires and Mario just falls through the lava
  // plane without any reaction.
  //
  // The m_prev_in_lava -> in_lava rising edge is the best host-side
  // approximation of "Mario just touched the lava surface from above": it
  // fires on every arc re-entry, which gives a visible bounce-on-contact
  // loop while the player stays over the lava pool. Re-calling
  // sm64_set_mario_action(m, ACT_LAVA_BOOST) re-initializes vel[1]=84
  // (see mario.c:858-863), so the second+ bounces get the same upward
  // launch as the first.
  const bool lava_entry_edge = in_lava && !m_prev_in_lava;
  // Shell-riding suppresses the lava kick entirely — Mario is supposed to
  // be invulnerable to fire floors in this state (see comment above).
  const bool needs_kick =
      in_lava && !riding_shell && (!already_burning || lava_entry_edge);

  std::scoped_lock lock(m_sm64_lock);
  sm64_set_mario_water_level(m_mario_id, sm64_water_level);

  if (needs_kick) {
    // Mirror SM64's native check_lava_boost (interaction.c:901-910): subtract
    // one wedge of health and drop Mario into ACT_LAVA_BOOST. Stock SM64 adds
    // 12 (cap) / 18 (no cap) to hurtCounter which burns 3 or ~4.5 wedges over
    // a few frames via the hurt-counter drain; libsm64 doesn't expose
    // hurtCounter, so we just knock one wedge off directly per kick. Every
    // re-entry bounce takes another wedge. If Mario runs out of health while
    // still bouncing, the 0-wedge fire-action handler in tick() catches him
    // in ACT_LAVA_BOOST and forces ACT_IDLE so he doesn't loop forever at 0.
    uint16_t new_health = 0;
    if (current_health > 0x100) {
      new_health = static_cast<uint16_t>(current_health) - 0x100;
    }
    sm64_set_mario_health(m_mario_id, new_health);
    sm64_set_mario_action(m_mario_id, kActLavaBoost);
    if (lava_entry_edge) {
      lg::info(
          "[libsm64] Mario re-entered LAVA (edge) — ACT_LAVA_BOOST re-fire, "
          "prev_action=0x{:08X}, health 0x{:04X} -> 0x{:04X}",
          current_action, static_cast<uint16_t>(current_health), new_health);
    } else {
      lg::info(
          "[libsm64] Mario entered LAVA volume (wt25) — ACT_LAVA_BOOST, "
          "health 0x{:04X} -> 0x{:04X}",
          static_cast<uint16_t>(current_health), new_health);
    }
  }

  // Cache the current frame's lava state for next frame's edge detection.
  // We store this after the kick so the next frame sees the correct
  // "was I in lava last tick?" value.
  m_prev_in_lava = in_lava;
}

MarioGeometry LibSM64Manager::get_geometry() {
  std::lock_guard<std::mutex> lock(m_geo_mutex);
  return m_geometry;
}

MarioState LibSM64Manager::get_state() {
  std::lock_guard<std::mutex> lock(m_geo_mutex);
  return m_state;
}

// ========================================================================
// Dynamic actor collision
// ========================================================================
//
// Every "tick" we walk the Jak process tree rooted at *active-pool*, find
// process-drawables whose root is a collide-shape (or subclass) that owns a
// collide-mesh (directly via collide-shape-prim-mesh, or via a nested
// collide-shape-prim-group). For each such mesh we bake its local vertices
// into world space using the actor's current trans+quat and register it with
// libsm64 via sm64_surface_object_create. Subsequent frames re-bake on motion
// and either move the existing object (same signature: skip) or delete+recreate.
//
// GOAL runtime offsets — remember boxed (basic) types subtract 4 from the
// declared offset, see goalc/compiler/compilation/Type.cpp:1632.
//
//   process-drawable.root                :offset 112  -> runtime 108
//   collide-shape.trans (from trs)       :offset 16   -> runtime 12
//   collide-shape.quat  (from trsq)      :offset 32   -> runtime 28
//   collide-shape.root-prim              :offset 160  -> runtime 156
//   collide-shape-prim.prim-core         :offset 16   -> runtime 12 (inline)
//   collide-shape-prim-mesh.mesh         :offset 72   -> runtime 68
//   collide-shape-prim-group.num-prims   :offset 72   -> runtime 68
//   collide-shape-prim-group.prim[0]     :offset 80   -> runtime 76
//   collide-mesh.num-tris                :offset 8    -> runtime 4
//   collide-mesh.num-verts               :offset 12   -> runtime 8
//   collide-mesh.vertex-data             :offset 16   -> runtime 12 (pointer to inline array)
//   collide-mesh.tris (inline array)     :offset 32   -> runtime 28 (in-place, 8 bytes each)

namespace ac {  // "actor collision" — isolated from the rest of the file

constexpr u32 PDRAW_ROOT_OFF = 108;
constexpr u32 PDRAW_NODE_LIST_OFF = 112;        // process-drawable.node-list (basic), declared 116 - 4
constexpr u32 CSHAPE_TRANS_OFF = 12;
constexpr u32 CSHAPE_QUAT_OFF = 28;
constexpr u32 CSHAPE_ROOT_PRIM_OFF = 156;
constexpr u32 PRIM_TRANSFORM_INDEX_OFF = 8;     // collide-shape-prim.transform-index (int8)
// Jak 1 collide-kind is a 64-bit bitfield (uint64). Both the root prim's
// collide-with (declared @64) and its inline prim-core.collide-as (declared @32)
// are 8 bytes. Memory offsets are the declared offsets minus 4 — same basic-
// header adjustment every other constant in this file uses. When a GOAL actor
// "dies" or otherwise disables its collision, it clears both to (collide-kind)
// — i.e., all bits zero — and we skip the actor so Mario doesn't trip over
// an invisible corpse.
constexpr u32 PRIM_CORE_COLLIDE_AS_OFF = 28;    // declared 32 - 4
constexpr u32 PRIM_CORE_ACTION_OFF = 36;        // declared 40 - 4 (prim-core @16 + action @24)
constexpr u32 PRIM_COLLIDE_WITH_OFF = 60;       // declared 64 - 4
constexpr u32 PRIM_MESH_MESH_OFF = 68;
constexpr u32 PRIM_GROUP_NUM_PRIMS_OFF = 68;
constexpr u32 PRIM_GROUP_PRIM_ARRAY_OFF = 76;
// collide-shape-prim.local-sphere is an inline vec4 (xyz = center, w = radius)
// right after the 32-byte prim-core. prim-core starts at runtime offset 12
// (declared 16 - 4), so local-sphere lands at 12 + 32 = 44. Used by the
// sphere prim tessellator. The sphere class (`collide-shape-prim-sphere`)
// overlays `radius` at `local-sphere.w`, so both are the same 16-byte slot.
constexpr u32 PRIM_LOCAL_SPHERE_OFF = 44;
constexpr u32 MESH_NUM_TRIS_OFF = 4;
constexpr u32 MESH_NUM_VERTS_OFF = 8;
constexpr u32 MESH_VERTEX_DATA_OFF = 12;
constexpr u32 MESH_TRIS_OFF = 28;
constexpr u32 MESH_TRI_SIZE = 8;  // collide-mesh-tri: 3 u8 indices + 1 u8 pad + 1 u32 pat
// cspace-array (process-drawable.node-list) layout:
//   inline-array-class is a basic. `data` is declared at offset 16; runtime
//   offset = 12 (declared - 4). Each cspace is a 32-byte structure (cspace-array
//   heap-base = 32). cspace itself is a `structure` (no type tag), so its field
//   offsets are NOT -4 adjusted: parent@0, joint@4, joint-num@8, geo@12, bone@16, ...
constexpr u32 CSPACE_ARRAY_DATA_OFF = 12;       // declared 16 - 4
constexpr u32 CSPACE_SIZE = 32;
constexpr u32 CSPACE_BONE_OFF = 16;             // structure field, no -4 adjust
// bone.transform is a 4x4 matrix at offset 0 of `bone`. Column-major:
//   col0 (offset 0..15)  = X basis
//   col1 (offset 16..31) = Y basis
//   col2 (offset 32..47) = Z basis
//   col3 (offset 48..63) = world translation (xyz, w)

// Process-tree layout (unboxed-adjusted runtime offsets).
// process-tree.child is at GOAL :offset 20 -> runtime 16
// process-tree.brother is at GOAL :offset 16 -> runtime 12
constexpr u32 PTREE_BROTHER_OFF = 12;
constexpr u32 PTREE_CHILD_OFF = 16;

// Limits, tunable
constexpr int MAX_PROCESS_TREE_NODES = 4096;     // safety cap on DFS
constexpr int MAX_ACTOR_SURFACE_OBJECTS = 32;    // cap new objects per frame
constexpr u32 MAX_VERTS_PER_MESH = 256;          // collide-mesh uses u8 indices
constexpr u32 MAX_TRIS_PER_MESH = 512;
constexpr int MAX_PRIM_DEPTH = 8;
constexpr u32 MAX_PRIMS_IN_GROUP = 64;
constexpr int MAX_TYPE_CHAIN_DEPTH = 32;

// Is `addr` a plausible EE-memory pointer that can hold `size` bytes?
// Rejects null/tiny/unaligned-ish pointers and anything past the end.
inline bool valid_ee_addr(u32 addr, u32 size, u32 mem_size) {
  if (addr < 16) return false;                        // no real GOAL object lives this low
  if (size == 0) return false;
  if (addr > mem_size) return false;
  if (size > mem_size) return false;
  if (addr + size > mem_size) return false;            // also catches a+s overflow given a<mem_size
  return true;
}

// Stricter check for a *basic pointer*: must satisfy alignment that real
// jak1 basics use (low 3 bits == 4, since basics are 8-byte aligned at the
// type tag and basic_ptr = type_tag + 4), and must live above the kernel
// scratch / symbol-table region. Walking garbage interpreted as basic ptrs
// was crashing the dynamic-actor-collision walker — see crash log analysis
// in 14:42 trace where node 0x4BD3CC's `brother` field decoded as 0x36837
// (unaligned + below the heap floor) and walking it died.
//
// MIN_HEAP_ADDR is intentionally generous: kernel/symbol-table state lives
// well below 1 MB in jak1, so anything below this is definitely not a real
// heap-allocated basic.
inline bool valid_basic_ptr(u32 addr, u32 mem_size) {
  constexpr u32 MIN_HEAP_ADDR = 0x100000;  // 1 MB
  if (addr < MIN_HEAP_ADDR) return false;
  if ((addr & 0x7u) != 4u) return false;
  // Need at least the type tag at addr-4 plus a small payload.
  return valid_ee_addr(addr, 32, mem_size);
}

// A u32 read that bails on out-of-bounds.
inline bool read_u32(u8* mem, u32 addr, u32 mem_size, u32& out) {
  if (!valid_ee_addr(addr, 4, mem_size)) return false;
  std::memcpy(&out, mem + addr, 4);
  return true;
}

// A u64 read that bails on out-of-bounds. Used for Jak 1 collide-kind fields
// which are 64-bit bitfields.
inline bool read_u64(u8* mem, u32 addr, u32 mem_size, uint64_t& out) {
  if (!valid_ee_addr(addr, 8, mem_size)) return false;
  std::memcpy(&out, mem + addr, 8);
  return true;
}

inline bool read_vec4(u8* mem, u32 addr, u32 mem_size, float out[4]) {
  if (!valid_ee_addr(addr, 16, mem_size)) return false;
  std::memcpy(out, mem + addr, 16);
  return true;
}

// Read the type pointer for a basic object (stored at basic_ptr - 4).
inline bool read_basic_type(u8* mem, u32 basic_ptr, u32 mem_size, u32& type_out) {
  if (basic_ptr < 16) return false;                    // same floor as valid_ee_addr
  return read_u32(mem, basic_ptr - 4, mem_size, type_out);
}

// Build a rotation matrix from a unit quaternion (x, y, z, w).
// Result is row-major: out = M * v. Translation is added separately.
inline void quat_to_rot_mat3(const float q[4], float m[9]) {
  float x = q[0], y = q[1], z = q[2], w = q[3];
  float xx = x * x, yy = y * y, zz = z * z;
  float xy = x * y, xz = x * z, yz = y * z;
  float wx = w * x, wy = w * y, wz = w * z;
  m[0] = 1.0f - 2.0f * (yy + zz);  m[1] = 2.0f * (xy - wz);        m[2] = 2.0f * (xz + wy);
  m[3] = 2.0f * (xy + wz);         m[4] = 1.0f - 2.0f * (xx + zz); m[5] = 2.0f * (yz - wx);
  m[6] = 2.0f * (xz - wy);         m[7] = 2.0f * (yz + wx);        m[8] = 1.0f - 2.0f * (xx + yy);
}

// Walk the type-parent chain of a basic type, looking for `needle`.
// Memoizes the result keyed by the starting type pointer.
bool type_is_descendant(u8* ee_mem, u32 mem_size, u32 type_ptr, u32 needle,
                        std::unordered_map<u32, bool>& cache) {
  if (type_ptr == 0 || needle == 0) return false;
  auto it = cache.find(type_ptr);
  if (it != cache.end()) return it->second;

  u32 cur = type_ptr;
  for (int i = 0; i < MAX_TYPE_CHAIN_DEPTH; i++) {
    if (cur == 0) break;
    if (cur == needle) {
      cache[type_ptr] = true;
      return true;
    }
    // Type struct (basic): symbol @4->0, parent @8->4 after BASIC_OFFSET.
    u32 parent;
    if (!read_u32(ee_mem, cur + 4, mem_size, parent)) break;
    if (parent == cur) break;  // self-loop (object's parent)
    cur = parent;
  }
  cache[type_ptr] = false;
  return false;
}

// Decompose a row-major 3x3 rotation matrix into the (pitch, yaw, roll) Euler
// angles in degrees that libsm64's `mtxf_rotate_zxy_and_translate` expects in
// SM64ObjectTransform::eulerRotation. libsm64 internally negates degrees via
// `CONVERT_ANGLE`, so we precompute -extracted_angle here.
//
// Matrix indices: R[row*3+col]. libsm64 builds:
//   R[1][2] = -sin(pitch)            -> pitch = asin(-R[1][2])
//   R[1][0] / R[1][1] = sz/cz        -> roll  = atan2(R[1][0], R[1][1])
//   R[0][2] / R[2][2] = sy/cy        -> yaw   = atan2(R[0][2], R[2][2])
inline void rot_mat3_to_zxy_euler_degrees(const float rot[9], float out_deg[3]) {
  float r12 = rot[1 * 3 + 2];
  float r10 = rot[1 * 3 + 0];
  float r11 = rot[1 * 3 + 1];
  float r02 = rot[0 * 3 + 2];
  float r22 = rot[2 * 3 + 2];

  float sx = -r12;
  if (sx > 1.0f) sx = 1.0f;
  if (sx < -1.0f) sx = -1.0f;

  float pitch_rad = std::asin(sx);
  float yaw_rad, roll_rad;
  // Gimbal lock guard: when |sx| ~ 1, cos(pitch) ~ 0 and r10/r11 are tiny —
  // fall back to extracting roll from the (0,0)/(0,1) cell.
  if (std::abs(sx) > 0.9999f) {
    roll_rad = 0.0f;
    yaw_rad = std::atan2(-rot[2 * 3 + 0], rot[0 * 3 + 0]);
  } else {
    roll_rad = std::atan2(r10, r11);
    yaw_rad = std::atan2(r02, r22);
  }

  constexpr float RAD_TO_DEG = 57.29577951308232f;
  // Negate to cancel libsm64's CONVERT_ANGLE negation.
  out_deg[0] = -pitch_rad * RAD_TO_DEG;
  out_deg[1] = -yaw_rad * RAD_TO_DEG;
  out_deg[2] = -roll_rad * RAD_TO_DEG;
}

// Quaternion variant — go via the row-major 3x3 helper above.
inline void quat_to_zxy_euler_degrees(const float q[4], float out_deg[3]) {
  float rot[9];
  quat_to_rot_mat3(q, rot);
  rot_mat3_to_zxy_euler_degrees(rot, out_deg);
}

// Extract a single collide-mesh's local-space triangles as SM64Surfaces,
// in actor-local SM64 units (no rotation/translation applied). The caller
// passes the world transform separately to libsm64 via SM64ObjectTransform —
// this is what allows `sm64_surface_object_move` to compute platform velocity
// each frame and carry Mario along with moving platforms.
bool extract_mesh_local(u8* ee_mem, u32 mem_size, u32 mesh_ptr,
                        std::vector<SM64Surface>& out_surfaces,
                        float* out_local_aabb_min_jak = nullptr,
                        float* out_local_aabb_max_jak = nullptr) {
  u32 num_tris = 0, num_verts = 0, vertex_data_ptr = 0;
  if (!read_u32(ee_mem, mesh_ptr + MESH_NUM_TRIS_OFF, mem_size, num_tris)) return false;
  if (!read_u32(ee_mem, mesh_ptr + MESH_NUM_VERTS_OFF, mem_size, num_verts)) return false;
  if (!read_u32(ee_mem, mesh_ptr + MESH_VERTEX_DATA_OFF, mem_size, vertex_data_ptr)) return false;

  if (num_tris == 0 || num_tris > MAX_TRIS_PER_MESH) return false;
  if (num_verts == 0 || num_verts > MAX_VERTS_PER_MESH) return false;
  if (!valid_ee_addr(vertex_data_ptr, num_verts * 16u, mem_size)) return false;
  if (!valid_ee_addr(mesh_ptr + MESH_TRIS_OFF, num_tris * MESH_TRI_SIZE, mem_size)) return false;

  float lmin[3] = {std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                    std::numeric_limits<float>::max()};
  float lmax[3] = {-std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(),
                    -std::numeric_limits<float>::max()};
  std::vector<std::array<int32_t, 3>> local_verts(num_verts);
  for (u32 i = 0; i < num_verts; i++) {
    float local_v[4];
    if (!read_vec4(ee_mem, vertex_data_ptr + i * 16u, mem_size, local_v)) return false;
    for (int k = 0; k < 3; k++) {
      if (local_v[k] < lmin[k]) lmin[k] = local_v[k];
      if (local_v[k] > lmax[k]) lmax[k] = local_v[k];
    }
    local_verts[i][0] = static_cast<int32_t>(local_v[0] * JAK_TO_SM64_SCALE);
    local_verts[i][1] = static_cast<int32_t>(local_v[1] * JAK_TO_SM64_SCALE);
    local_verts[i][2] = static_cast<int32_t>(local_v[2] * JAK_TO_SM64_SCALE);
  }
  if (out_local_aabb_min_jak) std::memcpy(out_local_aabb_min_jak, lmin, sizeof(lmin));
  if (out_local_aabb_max_jak) std::memcpy(out_local_aabb_max_jak, lmax, sizeof(lmax));

  out_surfaces.reserve(out_surfaces.size() + num_tris);
  for (u32 i = 0; i < num_tris; i++) {
    u32 tri_addr = mesh_ptr + MESH_TRIS_OFF + i * MESH_TRI_SIZE;
    u8 i0 = ee_mem[tri_addr + 0];
    u8 i1 = ee_mem[tri_addr + 1];
    u8 i2 = ee_mem[tri_addr + 2];
    if (i0 >= num_verts || i1 >= num_verts || i2 >= num_verts) continue;

    SM64Surface s{};
    s.type = 0;      // SURFACE_DEFAULT
    s.force = 0;
    s.terrain = 1;   // TERRAIN_STONE
    const u8 indices[3] = {i0, i1, i2};
    for (int v = 0; v < 3; v++) {
      s.vertices[v][0] = local_verts[indices[v]][0];
      s.vertices[v][1] = local_verts[indices[v]][1];
      s.vertices[v][2] = local_verts[indices[v]][2];
    }
    out_surfaces.push_back(s);
  }
  return true;
}

// Tessellate a sphere-prim into SM64Surface triangles in actor-local space
// (Jak → SM64 scale applied). We use a SQUARE PRISM (diamond-oriented, 4
// slices) with NO bottom cap. This geometry is carefully chosen to dodge
// several libsm64 collision quirks that a more "natural" approximation (UV
// sphere, octagonal prism, cylinder, closed box) would trigger:
//
//   1. (Floor classification) libsm64 classifies normal.y > 0.01 as floor.
//      A UV sphere's near-equator tris have |normal.y| ~0.3 → picked up as
//      floors → find_floor_from_list accepts them up to 78 units above
//      Mario → stationary_ground_step chain-snaps Mario up the side of the
//      sphere. Any prism built from purely vertical side walls sidesteps
//      this because the cross product has ny = 0 exactly.
//
//   2. (Wall Overlaps bug — surface_collision.c line ~277) When Mario is
//      within the wall-collision radius (30 for lower, 60 for upper) of
//      multiple walls simultaneously, each wall applies its full push using
//      the ORIGINAL offset, rather than the offset after the previous
//      pushes. An octagon with adjacent walls 45° apart has midpoint wall
//      separations of apothem * (1 - cos(45°)) ~= 0.293 * apothem, which is
//      well under 30 for any reasonable scarecrow radius. Three walls end
//      up pushing simultaneously (~100+ units of overshoot per frame → the
//      "Mario flies all over the place" symptom). A square (n=4) has
//      adjacent wall separations of exactly `apothem` at the midpoint —
//      always > 30 for non-tiny scarecrows — so only ONE wall pushes at
//      each edge midpoint. At the 4 vertices, two orthogonal walls push
//      simultaneously, but because their pushes are perpendicular the
//      overshoot is just sqrt(2)*30 − 30 ~= 12 units. n=4 is the only n
//      where this calculus is gentle enough for r=77-sized scarecrows.
//
//   3. (Exposed Ceilings bug — surface_collision.c line ~69) A tri with
//      normal.y < -0.01 is classified as a ceiling, and the check
//      `if (y - (height + 78) > 0) continue;` keeps any ceiling within 78
//      units BELOW the reference y. `vec3f_find_ceil` passes
//      floorHeight + 80 as the reference y, so a bottom cap at
//      center.y - r triggers as a ceiling whenever the scarecrow sphere2's
//      bottom (at cy − r ≈ 52 local, well within 78 of the floor+80 check y)
//      appears near ground level. perform_ground_quarter_step then fires
//      `if (floorHeight + 160 >= ceilHeight) STOP_QSTEPS;` and Mario
//      lurches. The fix is to simply NOT emit the bottom cap — a prism open
//      at the bottom is fine because Mario approaches from the side (walls
//      catch him) and can't teleport into the interior.
//
// Triangle budget: 4 slices × 2 tris (walls) + 2 tris (top cap) = 10 tris
// per sphere prim. Tiny compared to the old 36-tri UV sphere, and the
// square shape leaks about 31 units of phantom collision into each corner
// outside the sphere's actual radius — an acceptable tradeoff for Mario
// not launching across the map.
//
// Out AABBs are in Jak units (matches extract_mesh_local for the caller).
void tessellate_sphere_local(const float center_jak[3], float radius_jak,
                              std::vector<SM64Surface>& out_surfaces,
                              float* out_local_aabb_min_jak = nullptr,
                              float* out_local_aabb_max_jak = nullptr) {
  // 4 slices = square prism in diamond orientation (vertices on the axes).
  // Walls bisect at 45°/135°/225°/315°. See the comment above for why n=4
  // is the only n that survives libsm64's wall overlap quirk.
  constexpr int SLICES = 4;
  constexpr float kPi = 3.14159265358979323846f;

  auto to_sm64 = [](float v) { return static_cast<int32_t>(v * JAK_TO_SM64_SCALE); };

  // Two rings of SLICES vertices each — top ring at center.y + r, bottom ring
  // at center.y - r. Vertices walk CCW around +y in math (x, z) orientation
  // (increasing theta). This ordering matters for cap winding below.
  std::array<std::array<int32_t, 3>, SLICES> ring_top{};
  std::array<std::array<int32_t, 3>, SLICES> ring_bot{};
  for (int i = 0; i < SLICES; i++) {
    float theta = 2.0f * kPi * float(i) / float(SLICES);
    float x_local = center_jak[0] + radius_jak * std::cos(theta);
    float z_local = center_jak[2] + radius_jak * std::sin(theta);
    ring_top[i][0] = to_sm64(x_local);
    ring_top[i][1] = to_sm64(center_jak[1] + radius_jak);
    ring_top[i][2] = to_sm64(z_local);
    ring_bot[i][0] = to_sm64(x_local);
    ring_bot[i][1] = to_sm64(center_jak[1] - radius_jak);
    ring_bot[i][2] = to_sm64(z_local);
  }

  auto push_tri = [&out_surfaces](const int32_t a[3], const int32_t b[3],
                                    const int32_t c[3]) {
    SM64Surface s{};
    s.type = 0;      // SURFACE_DEFAULT
    s.force = 0;
    s.terrain = 1;   // TERRAIN_STONE
    s.vertices[0][0] = a[0]; s.vertices[0][1] = a[1]; s.vertices[0][2] = a[2];
    s.vertices[1][0] = b[0]; s.vertices[1][1] = b[1]; s.vertices[1][2] = b[2];
    s.vertices[2][0] = c[0]; s.vertices[2][1] = c[1]; s.vertices[2][2] = c[2];
    out_surfaces.push_back(s);
  };

  // Side walls: each slice is a vertical quad between ring_bot[i]/ring_top[i]
  // and ring_bot[ni]/ring_top[ni]. We emit two tris per quad with winding
  // chosen so the outward-radial normal falls out of (v2-v1) × (v3-v2):
  //   Tri A: (B_i, T_ni, B_ni) — edges (B→T) vertical, (T→B) vertical
  //   Tri B: (B_i, T_i, T_ni) — edge (B→T) vertical, (T→T) horizontal
  // Because both tris contain a purely-vertical edge, the cross-product's
  // y-component is 0 exactly. libsm64 classifies them as walls, not floors,
  // so find_floor never picks them up and Mario doesn't snap up the side.
  // The two coplanar tris are NOT a double-push: find_wall_collisions_from_list
  // runs the triangle-inside test in (y, x) or (y, -z) projected space, and a
  // single point can only be inside one of the two coplanar tris per quad.
  for (int i = 0; i < SLICES; i++) {
    int ni = (i + 1) % SLICES;
    push_tri(ring_bot[i].data(), ring_top[ni].data(), ring_bot[ni].data());
    push_tri(ring_bot[i].data(), ring_top[i].data(), ring_top[ni].data());
  }

  // Top cap: triangle fan rooted at ring_top[0] using (T_0, T_{i+1}, T_i).
  // The ring is CCW around +y in math orientation, so reversing the last two
  // verts in each fan tri gives CW winding. CW in math (x, z) is exactly what
  // find_floor_from_list's inside-triangle test accepts, and the cross
  // product gives normal.y > 0 (floor). Mario can land on top of the prism.
  // For SLICES=4 this fan emits exactly 2 tris covering the full square.
  for (int i = 1; i < SLICES - 1; i++) {
    push_tri(ring_top[0].data(), ring_top[i + 1].data(), ring_top[i].data());
  }

  // Bottom cap intentionally NOT emitted. See the (Exposed Ceilings bug)
  // section of the block comment above: any ceiling tri within 78 units
  // below floor+80 is treated as a valid ceiling for Mario at ground level,
  // and sphere2's bottom cap at cy − r lands inside that window for
  // scarecrow-sized prims — triggering STOP_QSTEPS as soon as Mario walks
  // near. Leaving the prism open-bottomed is safe: Mario can't teleport
  // inside, so he only ever meets the walls and top cap.

  if (out_local_aabb_min_jak) {
    out_local_aabb_min_jak[0] = center_jak[0] - radius_jak;
    out_local_aabb_min_jak[1] = center_jak[1] - radius_jak;
    out_local_aabb_min_jak[2] = center_jak[2] - radius_jak;
  }
  if (out_local_aabb_max_jak) {
    out_local_aabb_max_jak[0] = center_jak[0] + radius_jak;
    out_local_aabb_max_jak[1] = center_jak[1] + radius_jak;
    out_local_aabb_max_jak[2] = center_jak[2] + radius_jak;
  }
}

// Per-prim collection result: identifies the prim, its collide-mesh (for mesh
// prims) or local sphere params (for sphere prims), and which bone it's
// attached to via transform-index. We need the prim ptr separately because two
// prim-meshes in the same group can share the same collide-mesh template —
// keying tracked actors by mesh_ptr alone collapses them onto each other.
struct CollectedPrim {
  enum class Kind { Mesh, Sphere };
  u32 prim_ptr;
  Kind kind;
  u32 mesh_ptr;                    // valid when kind == Mesh
  float sphere_center_jak[3];      // valid when kind == Sphere
  float sphere_radius_jak;         // valid when kind == Sphere
  int8_t transform_index;
};

// Recursively collect prim-mesh and prim-sphere entries from a collide-shape-prim
// hierarchy. When `prim_sphere_type == 0`, sphere prims are silently skipped
// (back-compat: tests that don't pass a sphere type see the old behavior).
void collect_mesh_prims(u8* ee_mem, u32 mem_size, u32 prim_ptr, u32 false_val,
                        u32 prim_mesh_type, u32 prim_group_type, u32 prim_sphere_type,
                        std::vector<CollectedPrim>& out_prims, int depth = 0) {
  if (prim_ptr == 0 || prim_ptr == false_val || depth > MAX_PRIM_DEPTH) return;
  u32 prim_type;
  if (!read_basic_type(ee_mem, prim_ptr, mem_size, prim_type)) return;

  auto read_xform_idx = [&](u32 pp) -> int8_t {
    int8_t xi = -2;
    if (valid_ee_addr(pp + PRIM_TRANSFORM_INDEX_OFF, 1, mem_size)) {
      xi = static_cast<int8_t>(ee_mem[pp + PRIM_TRANSFORM_INDEX_OFF]);
    }
    return xi;
  };

  if (prim_type == prim_mesh_type) {
    u32 mesh_ptr;
    if (!read_u32(ee_mem, prim_ptr + PRIM_MESH_MESH_OFF, mem_size, mesh_ptr)) return;
    if (mesh_ptr != 0 && mesh_ptr != false_val) {
      // transform-index is an int8 in collide-shape-prim. -2 = no joint
      // attachment, >=0 = bone index into process-drawable.node-list.
      CollectedPrim cp{};
      cp.prim_ptr = prim_ptr;
      cp.kind = CollectedPrim::Kind::Mesh;
      cp.mesh_ptr = mesh_ptr;
      cp.transform_index = read_xform_idx(prim_ptr);
      out_prims.push_back(cp);
    }
  } else if (prim_sphere_type != 0 && prim_type == prim_sphere_type) {
    // Sphere prim — pull center/radius from local-sphere (inline vec4 right
    // after the 32-byte prim-core). collide-shape-prim-sphere overlays its
    // `radius` field onto local-sphere.w, so both live in the same 16-byte
    // slot at PRIM_LOCAL_SPHERE_OFF.
    float local_sphere[4];
    if (!read_vec4(ee_mem, prim_ptr + PRIM_LOCAL_SPHERE_OFF, mem_size, local_sphere)) return;
    // Sanity check: finite, positive radius, center within a sane range.
    // Bogus values here usually mean the collide-shape was never initialized,
    // in which case we don't want to feed garbage into libsm64.
    for (int i = 0; i < 4; i++) {
      if (!std::isfinite(local_sphere[i])) return;
    }
    if (local_sphere[3] <= 0.0f || local_sphere[3] > 1.0e7f) return;
    for (int i = 0; i < 3; i++) {
      if (std::abs(local_sphere[i]) > 1.0e7f) return;
    }
    CollectedPrim cp{};
    cp.prim_ptr = prim_ptr;
    cp.kind = CollectedPrim::Kind::Sphere;
    cp.sphere_center_jak[0] = local_sphere[0];
    cp.sphere_center_jak[1] = local_sphere[1];
    cp.sphere_center_jak[2] = local_sphere[2];
    cp.sphere_radius_jak = local_sphere[3];
    cp.transform_index = read_xform_idx(prim_ptr);
    out_prims.push_back(cp);
  } else if (prim_type == prim_group_type) {
    u32 num_prims;
    if (!read_u32(ee_mem, prim_ptr + PRIM_GROUP_NUM_PRIMS_OFF, mem_size, num_prims)) return;
    if (num_prims == 0 || num_prims > MAX_PRIMS_IN_GROUP) return;
    for (u32 i = 0; i < num_prims; i++) {
      u32 child;
      if (!read_u32(ee_mem, prim_ptr + PRIM_GROUP_PRIM_ARRAY_OFF + i * 4, mem_size, child)) break;
      if (child == 0 || child == false_val) continue;
      collect_mesh_prims(ee_mem, mem_size, child, false_val, prim_mesh_type, prim_group_type,
                         prim_sphere_type, out_prims, depth + 1);
    }
  }
  // Other prim types (unknown) are ignored.
}

// Resolve a prim's per-prim world transform. For prims with transform_index >= 0
// and a valid node-list, looks up `process-drawable.node-list[index].bone.transform`
// and decomposes it into translation + rotation. For everything else falls back
// to the actor's root cshape trans/quat (the existing behavior).
//
// out_pos is in Jak units; out_rot is row-major 3x3 (orthonormal rotation
// extracted from the bone matrix, with scale stripped).
bool compute_prim_world_transform(u8* ee_mem, u32 mem_size, u32 false_val, u32 pd_node,
                                   int8_t transform_index, const float root_trans[3],
                                   const float root_rot[9], float out_pos[3], float out_rot[9],
                                   bool* out_used_bone = nullptr) {
  // Default: actor root.
  auto use_root = [&]() {
    out_pos[0] = root_trans[0];
    out_pos[1] = root_trans[1];
    out_pos[2] = root_trans[2];
    std::memcpy(out_rot, root_rot, 9 * sizeof(float));
    if (out_used_bone) *out_used_bone = false;
    return true;
  };

  if (transform_index < 0) return use_root();

  // process-drawable.node-list — basic ptr to a cspace-array.
  u32 node_list = 0;
  if (!read_u32(ee_mem, pd_node + PDRAW_NODE_LIST_OFF, mem_size, node_list)) return false;
  if (node_list == 0 || node_list == false_val) return use_root();
  if (!valid_basic_ptr(node_list, mem_size)) return use_root();

  // Bounds-check transform_index against cspace-array.length (offset 0).
  // Skeleton may not be initialized yet (length == 0) or the index might
  // be bogus. Either way, fall back to the actor root rather than reading
  // garbage from beyond the end of the array.
  u32 cspace_len_raw = 0;
  if (!read_u32(ee_mem, node_list, mem_size, cspace_len_raw)) return use_root();
  int32_t cspace_len = static_cast<int32_t>(cspace_len_raw);
  if (cspace_len <= 0 || cspace_len > 1024) return use_root();  // sanity bound
  if (static_cast<int32_t>(transform_index) >= cspace_len) return use_root();

  // cspace[i] starts at node_list + 16 + i*32. cspace.bone is a basic ptr
  // at +16 within the cspace struct.
  u32 cspace_addr =
      node_list + CSPACE_ARRAY_DATA_OFF + static_cast<u32>(transform_index) * CSPACE_SIZE;
  u32 bone_ptr = 0;
  if (!read_u32(ee_mem, cspace_addr + CSPACE_BONE_OFF, mem_size, bone_ptr)) return use_root();
  if (bone_ptr == 0 || bone_ptr == false_val) return use_root();
  // bone is a structure (not a basic), so no -4 type tag — just check the
  // raw matrix range is in-bounds. Require 16-byte alignment so the matrix
  // load is well-defined.
  if ((bone_ptr & 0xF) != 0) return use_root();
  if (!valid_ee_addr(bone_ptr, 64, mem_size)) return use_root();

  // bone.transform: 4x4 column-major, 16 floats. Sanity-check finiteness
  // AND that translation is within a reasonable Jak-world range, so we
  // don't push libsm64 absurd transforms when the bone is uninitialized.
  float m[16];
  std::memcpy(m, ee_mem + bone_ptr, 64);
  for (int i = 0; i < 16; i++) {
    if (!std::isfinite(m[i])) return use_root();
  }
  // Translation is column 3. Jak world coords are in the millions max
  // (4096 units/meter). Anything beyond ±1e8 is garbage.
  for (int i = 0; i < 3; i++) {
    if (std::abs(m[3 * 4 + i]) > 1.0e8f) return use_root();
  }

  // Translation = column 3 (xyz). m[col*4 + row].
  out_pos[0] = m[3 * 4 + 0];
  out_pos[1] = m[3 * 4 + 1];
  out_pos[2] = m[3 * 4 + 2];

  // Build row-major 3x3 rotation from columns 0/1/2 of the bone matrix.
  // out_rot[row*3 + col] = m[col*4 + row]
  for (int r = 0; r < 3; r++) {
    for (int c = 0; c < 3; c++) {
      out_rot[r * 3 + c] = m[c * 4 + r];
    }
  }

  // Strip scale by normalizing each column (each column is a basis vector).
  // Bone matrices in jak1 are typically scale=1 anyway, but be safe.
  for (int c = 0; c < 3; c++) {
    float lx = out_rot[0 * 3 + c];
    float ly = out_rot[1 * 3 + c];
    float lz = out_rot[2 * 3 + c];
    float len2 = lx * lx + ly * ly + lz * lz;
    if (len2 < 1e-12f || !std::isfinite(len2)) return use_root();
    float inv = 1.0f / std::sqrt(len2);
    out_rot[0 * 3 + c] = lx * inv;
    out_rot[1 * 3 + c] = ly * inv;
    out_rot[2 * 3 + c] = lz * inv;
  }
  if (out_used_bone) *out_used_bone = true;
  return true;
}

}  // namespace ac

void LibSM64Manager::clear_actor_collision() {
  if (m_tracked_actors.empty()) {
    m_broken_meshes.clear();
    return;
  }
  for (auto& [mesh_addr, tracked] : m_tracked_actors) {
    if (tracked.has_obj) {
      sm64_surface_object_delete(tracked.sm64_obj_id);
    }
  }
  m_tracked_actors.clear();
  m_broken_meshes.clear();
  lg::info("[libsm64] Cleared all actor surface objects");
}

// --------------------------------------------------------------------------
// The real per-frame entry point and a test-visible variant.
// --------------------------------------------------------------------------
//
// Both share the same walker. The "real" version pulls parameters from the
// live kernel via find_symbol_from_c; the test variant takes everything as
// explicit arguments so unit tests can drive it against a synthetic buffer.

namespace {

// Internal walker state passed around by the sweep routine.
struct WalkCtx {
  u8* ee_mem;
  u32 mem_size;
  u32 false_val;
  u32 active_pool_sym;
  u32 process_drawable_type;
  u32 collide_shape_type;
  u32 prim_mesh_type;
  u32 prim_group_type;
  // collide-shape-prim-sphere type. 0 disables sphere handling (tests that
  // don't care about spheres leave it 0 — matches the pre-sphere behavior).
  u32 prim_sphere_type;
  // Camera-related process-drawable types. Either can be 0 to disable that
  // specific filter (e.g. citadelcam is 0 outside the citadel level).
  u32 pov_camera_type;
  u32 citadelcam_type;
  // Current *target* (= Jak) process-drawable pointer, or 0 if unknown. We
  // MUST skip this node in the walker: Jak's own collide-shape has a
  // prim-group containing sphere prims for his body, and if we mirror them
  // into libsm64 Mario spawns literally inside Jak's own collision walls
  // (because "Spawn Mario at Target" places him at Jak's position). The
  // result is Mario being violently ejected every frame and bouncing back
  // into the sphere as Jak drifts — a very distinctive "bounces toward
  // Jak forever" failure mode. 0 disables the filter (unit tests).
  u32 target_ptr;
  bool dry_run;
  int& diag_logs_remaining;
  std::unordered_map<u32, bool>& is_process_drawable_cache;
  std::unordered_map<u32, bool>& is_collide_shape_cache;
  std::unordered_map<u32, bool>& is_pov_camera_cache;
  std::unordered_map<u32, bool>& is_citadelcam_cache;
  std::unordered_map<uint64_t, LibSM64Manager::TrackedActor>& tracked_actors;
  std::unordered_set<u32>& broken_meshes;
  LibSM64Manager::TestSweepResult& result;
};

// Compose a tracking key from (process-drawable address, collide-shape-prim
// address). We key on the prim — not the mesh — because two prims inside the
// same prim-group can share a single collide-mesh template (e.g. mirrored
// sub-pieces) yet need their own libsm64 surface object because their
// `transform-index` (and therefore world transform) differs.
inline uint64_t make_actor_key(u32 pd_node, u32 prim_ptr) {
  return (static_cast<uint64_t>(pd_node) << 32) | static_cast<uint64_t>(prim_ptr);
}

// Perform one actor-collision sweep. Returns normally on any expected failure
// (unreadable memory, missing types, etc.) — the walker never throws or crashes.
void do_sweep(WalkCtx& c) {
  using namespace ac;

  if (c.ee_mem == nullptr) return;
  if (c.mem_size < 1024) return;
  if (c.false_val == 0) return;
  if (c.active_pool_sym == 0 || c.process_drawable_type == 0 || c.collide_shape_type == 0 ||
      c.prim_mesh_type == 0 || c.prim_group_type == 0) {
    return;
  }

  // Read the *active-pool* symbol value (= pointer to the active pool process-tree).
  u32 root_process;
  if (!read_u32(c.ee_mem, c.active_pool_sym, c.mem_size, root_process)) return;
  if (root_process == 0 || root_process == c.false_val) return;

  // process-tree.child / .brother are (pointer process-tree) — ppointers, not
  // direct pointers. The kernel sets each to the address of some other
  // process's `self` slot so it can rewrite the slot if the process moves.
  // To get the actual process-tree basic ptr we deref once. See
  // gkernel.gc / ppointer->process.
  auto deref_ppointer = [&c](u32 pp, u32& out_node) -> bool {
    if (pp == 0 || pp == c.false_val) return false;
    if (!valid_ee_addr(pp, 4, c.mem_size)) return false;
    u32 actual = 0;
    if (!read_u32(c.ee_mem, pp, c.mem_size, actual)) return false;
    if (actual == 0 || actual == c.false_val) return false;
    if (!valid_basic_ptr(actual, c.mem_size)) return false;
    out_node = actual;
    return true;
  };

  std::vector<u32> stack;
  stack.reserve(256);
  {
    u32 child_pp;
    if (read_u32(c.ee_mem, root_process + PTREE_CHILD_OFF, c.mem_size, child_pp)) {
      u32 first_child = 0;
      if (deref_ppointer(child_pp, first_child)) {
        stack.push_back(first_child);
      }
    }
  }

  // Reset per-frame seen flags on previously tracked actors.
  for (auto& [k, v] : c.tracked_actors) v.seen_this_frame = false;

  std::unordered_set<u32> visited_set;  // cycle guard: never re-enqueue a node
  visited_set.reserve(512);

  int created_this_frame = 0;

  while (!stack.empty() && c.result.process_tree_nodes_visited < MAX_PROCESS_TREE_NODES) {
    u32 node = stack.back();
    stack.pop_back();

    if (node == 0 || node == c.false_val) continue;

    // Skip *target* (Jak). His collide-shape-prim-group contains sphere
    // prims for his body (eichar-cs in collide-shape-h.gc). Mirroring those
    // into libsm64 means Mario's spawn — which is AT Jak's position when
    // using "Spawn Mario at Target" — lands inside Jak's own collision
    // walls, and the per-frame wall push ejects Mario violently before he
    // re-enters the sphere as Jak drifts the next frame. Net effect: Mario
    // pinballs back toward Jak endlessly (user report: "bounces toward
    // 0,0,0 and keeps freaking out" — 0,0,0 = wherever Jak happens to be).
    // Run before the visited-set check so a diagnostic re-queue can't
    // resurrect Jak. 0 disables the filter (unit tests, or a pre-kernel
    // tick where *target* isn't bound yet).
    if (c.target_ptr != 0 && node == c.target_ptr) continue;

    if (!visited_set.insert(node).second) continue;   // already walked
    c.result.process_tree_nodes_visited++;

    // The popped node itself must look like a basic pointer.
    if (!valid_basic_ptr(node, c.mem_size)) continue;

    // Read brother/child ppointers and deref to actual process-tree basic ptrs.
    u32 brother_pp = 0, child_pp = 0;
    read_u32(c.ee_mem, node + PTREE_BROTHER_OFF, c.mem_size, brother_pp);
    read_u32(c.ee_mem, node + PTREE_CHILD_OFF, c.mem_size, child_pp);
    u32 brother = 0, child = 0;
    if (deref_ppointer(brother_pp, brother) &&
        visited_set.find(brother) == visited_set.end()) {
      stack.push_back(brother);
    }
    if (deref_ppointer(child_pp, child) &&
        visited_set.find(child) == visited_set.end()) {
      stack.push_back(child);
    }

    // Only process-drawable nodes have a useful root; type-check via parent chain.
    u32 node_type;
    if (!read_basic_type(c.ee_mem, node, c.mem_size, node_type)) continue;
    // Self-referential type tag (node_type == node) is garbage; same for
    // unaligned / too-low type ptrs.
    if (node_type == node || !valid_basic_ptr(node_type, c.mem_size)) continue;
    if (!type_is_descendant(c.ee_mem, c.mem_size, node_type, c.process_drawable_type,
                             c.is_process_drawable_cache)) {
      continue;
    }

    // Reject camera-owned process-drawables. These aren't things Mario should
    // stand on — they exist just to host cutscene cameras and level-specific
    // camera overrides. Filters with a 0 target are no-ops (type_is_descendant
    // returns false when needle==0), so this works fine before citadelcam is
    // loaded.
    if (type_is_descendant(c.ee_mem, c.mem_size, node_type, c.pov_camera_type,
                            c.is_pov_camera_cache)) {
      continue;
    }
    if (type_is_descendant(c.ee_mem, c.mem_size, node_type, c.citadelcam_type,
                            c.is_citadelcam_cache)) {
      continue;
    }

    c.result.process_drawables_seen++;

    // Read root (collide-shape or subclass) from process-drawable @108.
    u32 root;
    if (!read_u32(c.ee_mem, node + PDRAW_ROOT_OFF, c.mem_size, root)) continue;
    if (root == 0 || root == c.false_val) continue;
    if (!valid_basic_ptr(root, c.mem_size)) continue;

    u32 root_type;
    if (!read_basic_type(c.ee_mem, root, c.mem_size, root_type)) continue;
    if (!valid_basic_ptr(root_type, c.mem_size)) continue;
    if (!type_is_descendant(c.ee_mem, c.mem_size, root_type, c.collide_shape_type,
                             c.is_collide_shape_cache)) {
      continue;
    }

    // Read root-prim and walk the prim tree to find any meshes.
    u32 root_prim;
    if (!read_u32(c.ee_mem, root + CSHAPE_ROOT_PRIM_OFF, c.mem_size, root_prim)) continue;
    if (root_prim == 0 || root_prim == c.false_val) continue;

    // Dead-actor check: when a GOAL actor is killed (see
    // `clear-collide-with-as` in collide-shape.gc, and the per-actor death
    // cleanup in yakow/seagull/target-util/robotboss/etc.), its root prim's
    // `collide-with` AND `prim-core.collide-as` are both zeroed. The
    // collide-shape itself stays linked to the process until GC, so naïvely
    // feeding it to Mario would give him an invisible corpse to walk into.
    // Mirrors the GOAL pattern: (when (and (zero? collide-with) (zero? collide-as)) skip).
    {
      uint64_t cwith = 0, cas = 0;
      if (!read_u64(c.ee_mem, root_prim + PRIM_COLLIDE_WITH_OFF, c.mem_size, cwith)) continue;
      if (!read_u64(c.ee_mem, root_prim + PRIM_CORE_COLLIDE_AS_OFF, c.mem_size, cas)) continue;
      if (cwith == 0 && cas == 0) continue;
    }

    // Skip actors whose root prim doesn't have the "solid" collide-action bit.
    // Non-solid actors (launchers, event triggers, etc.) are interaction-only in
    // GOAL — they fire events on contact but aren't physical geometry. Feeding
    // them to libsm64 gives Mario invisible walls around spring pads, etc.
    {
      constexpr uint32_t kCollideActionSolid = 1u << 0;  // collide-action bit 0
      u32 action = 0;
      if (!read_u32(c.ee_mem, root_prim + PRIM_CORE_ACTION_OFF, c.mem_size, action)) continue;
      if (!(action & kCollideActionSolid)) continue;
    }

    std::vector<CollectedPrim> prims;
    collect_mesh_prims(c.ee_mem, c.mem_size, root_prim, c.false_val, c.prim_mesh_type,
                       c.prim_group_type, c.prim_sphere_type, prims);
    if (prims.empty()) continue;

    // Read the actor's world transform.
    float trans[4], quat[4];
    if (!read_vec4(c.ee_mem, root + CSHAPE_TRANS_OFF, c.mem_size, trans)) continue;
    if (!read_vec4(c.ee_mem, root + CSHAPE_QUAT_OFF, c.mem_size, quat)) continue;

    // Sanity: finite trans, unit-ish quaternion. Replace garbage rotations with identity.
    if (!std::isfinite(trans[0]) || !std::isfinite(trans[1]) || !std::isfinite(trans[2])) {
      continue;
    }
    float qlen2 = quat[0] * quat[0] + quat[1] * quat[1] + quat[2] * quat[2] + quat[3] * quat[3];
    if (!std::isfinite(qlen2) || qlen2 < 0.5f || qlen2 > 1.5f) {
      quat[0] = quat[1] = quat[2] = 0.0f;
      quat[3] = 1.0f;
    }

    // Pre-compute the actor root rotation matrix once — used as the fallback
    // for prims that aren't bone-attached.
    float root_rot[9];
    quat_to_rot_mat3(quat, root_rot);

    for (const CollectedPrim& cp : prims) {
      u32 prim_ptr = cp.prim_ptr;
      u32 mesh_ptr = cp.mesh_ptr;  // 0 for sphere prims

      // Skip meshes we've already decided are broken. Sphere prims are
      // procedurally tessellated so they can't "break" in the extraction
      // sense — skip this check for them.
      if (cp.kind == CollectedPrim::Kind::Mesh && c.broken_meshes.count(mesh_ptr)) continue;

      // Key by (process-drawable, prim) so multiple prims inside one
      // prim-group that share a mesh template each get their own libsm64
      // surface object. Each bone-attached prim shows up as a distinct
      // entry — effectively `<actor>-1`, `<actor>-2`, etc. Sphere prims
      // slot into the same keyspace since prim_ptr is per-sphere.
      uint64_t key = make_actor_key(node, prim_ptr);
      auto& tracked = c.tracked_actors[key];
      tracked.seen_this_frame = true;

      // Resolve this prim's per-prim world transform. For prims attached to
      // a bone (transform_index >= 0) this walks the actor's node-list and
      // pulls the bone matrix; otherwise it falls back to the actor root.
      float prim_pos[3];
      float prim_rot[9];
      bool used_bone = false;
      if (!compute_prim_world_transform(c.ee_mem, c.mem_size, c.false_val, node,
                                         cp.transform_index, trans, root_rot, prim_pos,
                                         prim_rot, &used_bone)) {
        continue;
      }
      if (cp.transform_index >= 0) {
        c.result.bone_lookups_attempted++;
        if (used_bone) {
          c.result.bone_lookups_succeeded++;
        } else {
          c.result.bone_lookups_fell_back++;
        }
      }
      // Capture for tests/diagnostics. Bounded to keep production overhead low.
      if (c.result.captured_prims.size() < 256) {
        LibSM64Manager::TestSweepResult::CapturedPrim cap{};
        cap.pos[0] = prim_pos[0];
        cap.pos[1] = prim_pos[1];
        cap.pos[2] = prim_pos[2];
        cap.transform_index = cp.transform_index;
        cap.used_bone = used_bone;
        c.result.captured_prims.push_back(cap);
      }

      // Build the SM64ObjectTransform for this prim. For root-relative prims
      // this is the actor pose; for bone-attached prims it's the bone's world
      // pose pulled from the cspace-array. In either case we use the
      // sm64_surface_object_move flow (NOT destroy/recreate) so libsm64 can
      // compute platform velocity from the per-frame delta and carry Mario.
      SM64ObjectTransform xform{};
      xform.position[0] = prim_pos[0] * JAK_TO_SM64_SCALE;
      xform.position[1] = prim_pos[1] * JAK_TO_SM64_SCALE;
      xform.position[2] = prim_pos[2] * JAK_TO_SM64_SCALE;
      rot_mat3_to_zxy_euler_degrees(prim_rot, xform.eulerRotation);

      // Helper: refresh the world-space AABB on the tracked actor from its
      // local AABB and the prim's current world transform. Conservative — we
      // transform all 8 corners of the local AABB and take per-axis min/max,
      // which gives a tight fit for axis-aligned meshes and a loose-but-safe
      // fit for rotated ones.
      auto refresh_world_aabb = [&]() {
        if (!tracked.has_aabb) return;
        const float* lmin = tracked.local_aabb_min;
        const float* lmax = tracked.local_aabb_max;
        float wmin[3] = {std::numeric_limits<float>::max(),
                         std::numeric_limits<float>::max(),
                         std::numeric_limits<float>::max()};
        float wmax[3] = {-std::numeric_limits<float>::max(),
                         -std::numeric_limits<float>::max(),
                         -std::numeric_limits<float>::max()};
        for (int corner = 0; corner < 8; corner++) {
          float lv[3] = {(corner & 1) ? lmax[0] : lmin[0],
                         (corner & 2) ? lmax[1] : lmin[1],
                         (corner & 4) ? lmax[2] : lmin[2]};
          float wv[3];
          // wv = prim_rot * lv + prim_pos. prim_rot is row-major 3x3.
          for (int r = 0; r < 3; r++) {
            wv[r] = prim_rot[r * 3 + 0] * lv[0] + prim_rot[r * 3 + 1] * lv[1] +
                    prim_rot[r * 3 + 2] * lv[2] + prim_pos[r];
          }
          for (int k = 0; k < 3; k++) {
            if (wv[k] < wmin[k]) wmin[k] = wv[k];
            if (wv[k] > wmax[k]) wmax[k] = wv[k];
          }
        }
        std::memcpy(tracked.world_aabb_min, wmin, sizeof(wmin));
        std::memcpy(tracked.world_aabb_max, wmax, sizeof(wmax));
      };

      if (tracked.has_obj) {
        if (!c.dry_run) {
          sm64_surface_object_move(tracked.sm64_obj_id, &xform);
        }
        std::memcpy(tracked.last_trans, prim_pos, 12);
        refresh_world_aabb();
        continue;
      }

      if (created_this_frame >= MAX_ACTOR_SURFACE_OBJECTS) break;

      std::vector<SM64Surface> surfaces;
      float local_aabb_min[3];
      float local_aabb_max[3];
      if (cp.kind == CollectedPrim::Kind::Mesh) {
        if (!extract_mesh_local(c.ee_mem, c.mem_size, mesh_ptr, surfaces, local_aabb_min,
                                local_aabb_max)) {
          c.broken_meshes.insert(mesh_ptr);
          c.result.errors++;
          continue;
        }
        if (surfaces.empty()) {
          c.broken_meshes.insert(mesh_ptr);
          continue;
        }
      } else {
        // Sphere prim: tessellate into SM64Surfaces using the prim's
        // local-sphere center + radius. The bone/root transform computed
        // above is applied by libsm64 via sm64_surface_object_move, so we
        // only need the local-space geometry here.
        tessellate_sphere_local(cp.sphere_center_jak, cp.sphere_radius_jak, surfaces,
                                  local_aabb_min, local_aabb_max);
        if (surfaces.empty()) {
          // Should never happen at non-zero radius, but guard anyway.
          continue;
        }
      }

      c.result.meshes_found++;
      c.result.triangles_extracted += (int)surfaces.size();

      if (!c.dry_run) {
        SM64SurfaceObject obj{};
        obj.transform = xform;
        obj.surfaceCount = static_cast<uint32_t>(surfaces.size());
        obj.surfaces = surfaces.data();
        tracked.sm64_obj_id = sm64_surface_object_create(&obj);
        tracked.has_obj = true;
      }
      std::memcpy(tracked.last_trans, prim_pos, 12);
      std::memcpy(tracked.local_aabb_min, local_aabb_min, sizeof(local_aabb_min));
      std::memcpy(tracked.local_aabb_max, local_aabb_max, sizeof(local_aabb_max));
      tracked.has_aabb = true;
      refresh_world_aabb();
      created_this_frame++;
    }
  }

  // Reap actors that disappeared this frame.
  for (auto it = c.tracked_actors.begin(); it != c.tracked_actors.end();) {
    if (!it->second.seen_this_frame) {
      if (it->second.has_obj && !c.dry_run) {
        sm64_surface_object_delete(it->second.sm64_obj_id);
      }
      it = c.tracked_actors.erase(it);
    } else {
      ++it;
    }
  }
}

}  // namespace

void LibSM64Manager::update_actor_collision(u8* ee_mem) {
  if (!m_initialized || !dynamic_actor_collision || !ee_mem) return;

  u32 false_val = s7.offset;
  if (false_val == 0) return;

  // Lazily populate the type cache via find_symbol_from_c (read-only — does NOT
  // allocate new symbol slots). Any missing symbol → silently bail; we'll retry
  // next frame.
  if (!m_type_cache.ready) {
    auto pd = jak1::find_symbol_from_c("process-drawable");
    auto cs = jak1::find_symbol_from_c("collide-shape");
    auto pm = jak1::find_symbol_from_c("collide-shape-prim-mesh");
    auto pg = jak1::find_symbol_from_c("collide-shape-prim-group");
    auto ps = jak1::find_symbol_from_c("collide-shape-prim-sphere");
    auto ap = jak1::find_symbol_from_c("*active-pool*");
    if (pd.offset == 0 || cs.offset == 0 || pm.offset == 0 || pg.offset == 0 ||
        ps.offset == 0 || ap.offset == 0) {
      return;  // symbol table doesn't have one of our keys yet; retry next frame
    }
    u32 pd_val = pd->value;
    u32 cs_val = cs->value;
    u32 pm_val = pm->value;
    u32 pg_val = pg->value;
    u32 ps_val = ps->value;
    if (pd_val == 0 || pd_val == false_val || cs_val == 0 || cs_val == false_val ||
        pm_val == 0 || pm_val == false_val || pg_val == 0 || pg_val == false_val ||
        ps_val == 0 || ps_val == false_val) {
      return;  // types symbols exist but haven't been bound to Type structs yet
    }
    m_type_cache.process_drawable = pd_val;
    m_type_cache.collide_shape = cs_val;
    m_type_cache.prim_mesh = pm_val;
    m_type_cache.prim_group = pg_val;
    m_type_cache.prim_sphere = ps_val;
    m_type_cache.active_pool_sym = ap.offset;
    m_type_cache.ready = true;
    lg::info("[libsm64] Actor collision type cache ready: pd=0x{:X} cs=0x{:X} pm=0x{:X} pg=0x{:X} ps=0x{:X} ap_sym=0x{:X} false=0x{:X}",
             pd_val, cs_val, pm_val, pg_val, ps_val, ap.offset, false_val);
  }

  // Retry pov-camera / citadelcam lookup every frame until both are bound.
  // pov-camera lives in ENGINE so it's usually ready on the first successful
  // tick; citadelcam only shows up once the citadel level is loaded, and a
  // 0 here is fine — `type_is_descendant` with needle==0 returns false,
  // which disables that specific filter.
  if (m_type_cache.pov_camera == 0) {
    auto pc = jak1::find_symbol_from_c("pov-camera");
    if (pc.offset != 0) {
      u32 v = pc->value;
      if (v != 0 && v != false_val) {
        m_type_cache.pov_camera = v;
        lg::info("[libsm64] Actor collision: cached pov-camera type @0x{:X}", v);
      }
    }
  }
  if (m_type_cache.citadelcam == 0) {
    auto cc = jak1::find_symbol_from_c("citadelcam");
    if (cc.offset != 0) {
      u32 v = cc->value;
      if (v != 0 && v != false_val) {
        m_type_cache.citadelcam = v;
        lg::info("[libsm64] Actor collision: cached citadelcam type @0x{:X}", v);
      }
    }
  }

  // Resolve *target* every frame. Jak's process-drawable pointer changes
  // any time the player is re-spawned (e.g. death), and there's no upside
  // to caching it across frames — the symbol lookup is cheap. A 0 here
  // means *target* isn't bound yet, which disables the filter (the walker
  // will treat Jak as any other actor for one or two frames while the
  // kernel finishes wiring him up).
  u32 target_ptr_now = 0;
  {
    auto target_sym = jak1::find_symbol_from_c("*target*");
    if (target_sym.offset != 0) {
      u32 v = target_sym->value;
      if (v != 0 && v != false_val) {
        target_ptr_now = v;
      }
    }
  }

  TestSweepResult result;
  WalkCtx ctx{
      ee_mem,
      EE_MAIN_MEM_SIZE,
      false_val,
      m_type_cache.active_pool_sym,
      m_type_cache.process_drawable,
      m_type_cache.collide_shape,
      m_type_cache.prim_mesh,
      m_type_cache.prim_group,
      m_type_cache.prim_sphere,
      m_type_cache.pov_camera,
      m_type_cache.citadelcam,
      target_ptr_now,
      dynamic_actor_collision_dry_run,
      m_actor_diag_logs_remaining,
      m_is_process_drawable_cache,
      m_is_collide_shape_cache,
      m_is_pov_camera_cache,
      m_is_citadelcam_cache,
      m_tracked_actors,
      m_broken_meshes,
      result,
  };
  do_sweep(ctx);

  // ---- Ground-pound hit pass ----------------------------------------------
  // After the walker has refreshed every tracked actor's last_trans for this
  // frame, intersect each visible actor against Mario's ground-pound hitbox.
  // We just count hits for now (visualization in the debug GUI); applying the
  // attack to GOAL processes is the next step. Done OUTSIDE the walker so the
  // testable test_sweep path stays focused on collision-mesh extraction.
  {
    GroundPoundHitbox hb_snapshot;
    {
      std::lock_guard<std::mutex> lock(m_geo_mutex);
      hb_snapshot = m_gp_hitbox;
    }
    uint32_t hits = 0;
    if (hb_snapshot.active) {
      for (auto& [k, t] : m_tracked_actors) {
        if (!t.seen_this_frame) continue;
        // Prefer the world-space collide-mesh AABB if available — last_trans
        // is the prim anchor (often at the actor's base), so testing against
        // the prim point alone misses anything stacked above the pivot.
        bool hit;
        if (t.has_aabb) {
          hit = ground_pound_hitbox_overlaps_aabb(hb_snapshot, t.world_aabb_min, t.world_aabb_max);
        } else {
          constexpr float kActorPadRadius = 4096.0f;
          constexpr float kActorHalfHeight = 4096.0f;
          math::Vector3f actor_pos(t.last_trans[0], t.last_trans[1], t.last_trans[2]);
          hit = ground_pound_hitbox_overlaps(hb_snapshot, actor_pos, kActorPadRadius,
                                              kActorHalfHeight);
        }
        if (hit) {
          hits++;
          if (m_actor_diag_logs_remaining > 0) {
            m_actor_diag_logs_remaining--;
            lg::info(
                "[libsm64] ground-pound HIT actor pd=0x{:X} prim=0x{:X} aabb=({:.0f},{:.0f},{:.0f})-({:.0f},{:.0f},{:.0f})"
                " mario=({:.0f},{:.0f},{:.0f}) impact={}",
                static_cast<u32>(k >> 32), static_cast<u32>(k & 0xFFFFFFFFu),
                t.world_aabb_min[0], t.world_aabb_min[1], t.world_aabb_min[2],
                t.world_aabb_max[0], t.world_aabb_max[1], t.world_aabb_max[2],
                hb_snapshot.center.x(), hb_snapshot.center.y(), hb_snapshot.center.z(),
                hb_snapshot.impact_frame ? "YES" : "no");
          }
        }
      }
    }
    if (hits > 0 || hb_snapshot.active) {
      std::lock_guard<std::mutex> lock(m_geo_mutex);
      m_gp_hitbox.hits_this_frame = hits;
      m_gp_hitbox.total_hits += hits;
    }
  }

  m_actor_sync_frame++;
  if (m_actor_sync_frame == 1 || m_actor_sync_frame == 60 || m_actor_sync_frame == 600) {
    lg::info(
        "[libsm64] actor-collision frame {}: visited {} nodes, pd_seen {}, meshes {}, tris {}, errors {}, tracking {}, bone_attempts {}, bone_ok {}, bone_fellback {}",
        m_actor_sync_frame, result.process_tree_nodes_visited, result.process_drawables_seen,
        result.meshes_found, result.triangles_extracted, result.errors, m_tracked_actors.size(),
        result.bone_lookups_attempted, result.bone_lookups_succeeded,
        result.bone_lookups_fell_back);
  }
}

LibSM64Manager::TestSweepResult LibSM64Manager::test_sweep(u8* ee_mem,
                                                           u32 ee_mem_size,
                                                           u32 false_val,
                                                           u32 active_pool_sym,
                                                           u32 process_drawable_type,
                                                           u32 collide_shape_type,
                                                           u32 prim_mesh_type,
                                                           u32 prim_group_type,
                                                           bool dry_run,
                                                           u32 pov_camera_type,
                                                           u32 citadelcam_type,
                                                           u32 prim_sphere_type) {
  TestSweepResult result;
  int dummy_diag_remaining = 0;  // tests shouldn't spam lg::info
  std::unordered_map<u32, bool> pd_cache, cs_cache, pov_cache, citadel_cache;
  std::unordered_map<uint64_t, TrackedActor> tracked;
  std::unordered_set<u32> broken;

  WalkCtx ctx{
      ee_mem,
      ee_mem_size,
      false_val,
      active_pool_sym,
      process_drawable_type,
      collide_shape_type,
      prim_mesh_type,
      prim_group_type,
      prim_sphere_type,
      pov_camera_type,
      citadelcam_type,
      0,  // target_ptr: disabled in tests (synthetic buffers have no *target*)
      dry_run,
      dummy_diag_remaining,
      pd_cache,
      cs_cache,
      pov_cache,
      citadel_cache,
      tracked,
      broken,
      result,
  };
  do_sweep(ctx);
  return result;
}

// ============================================================================
// Yakow grab
// ============================================================================
//
// Lets Mario pick up Jak yakow actors with the punch/grab button (B), carry
// them around, and throw them with another B press. This mirrors the stock
// SM64 light-object carry flow: we use the libsm64 fake-held-object API to
// plant a sentinel into Mario's heldObj/usedObj slots and kick him into
// ACT_PICKING_UP; from there Mario's action machine runs the normal
// pickup → hold_idle → hold_walking → throw sequence under player control.
//
// While a yakow is "held" we walk the process tree each frame, read Mario's
// current world pos + face angle, compute a hand position (Mario + forward *
// yakow_hold_forward + up * yakow_hold_up, all in SM64 units converted to
// Jak), and overwrite the yakow process-drawable's root.trans in EE memory
// so the rendered yakow model teleports to Mario's hand. When SM64's action
// machine transitions heldObj back to NULL (natural throw / damage / fall),
// we detect it via sm64_mario_is_holding_fake() and release the yakow so
// its normal AI resumes.
//
// Note: we don't modify the yakow's quat or velocity — only trans. The yakow
// nav code runs every frame and may try to push the yakow back to its waypoint
// but our trans-write happens AFTER the Jak tick (from OpenGLRenderer) so we
// always win the race. Jitter from the yakow's physics resolving the glued
// position against floors/walls is expected v1 behavior.

// Walk the process tree to collect every live yakow actor. Returns a vector of
// (ee_addr, trans_jak[3]) pairs — trans is read from root+12 (same path as
// write_mario_pos_to_target). We reuse the existing ac::* helpers for pointer
// validation and type-chain walking.
namespace {

struct YakowRecord {
  u32 ee_addr;                 // process-drawable basic ptr
  float trans_jak[3];          // world position in Jak units
};

// Collect every yakow under *active-pool* into `out`. Bail cleanly on any
// invalid memory / missing type.
void collect_yakows(u8* ee_mem, u32 mem_size, u32 false_val, u32 active_pool_sym,
                     u32 yakow_type, std::unordered_map<u32, bool>& is_yakow_cache,
                     std::vector<YakowRecord>& out) {
  using namespace ac;
  out.clear();
  if (ee_mem == nullptr || mem_size < 1024 || false_val == 0 || yakow_type == 0 ||
      active_pool_sym == 0) {
    return;
  }

  u32 root_process;
  if (!read_u32(ee_mem, active_pool_sym, mem_size, root_process)) return;
  if (root_process == 0 || root_process == false_val) return;

  auto deref_ppointer = [&](u32 pp, u32& out_node) -> bool {
    if (pp == 0 || pp == false_val) return false;
    if (!valid_ee_addr(pp, 4, mem_size)) return false;
    u32 actual = 0;
    if (!read_u32(ee_mem, pp, mem_size, actual)) return false;
    if (actual == 0 || actual == false_val) return false;
    if (!valid_basic_ptr(actual, mem_size)) return false;
    out_node = actual;
    return true;
  };

  std::vector<u32> stack;
  stack.reserve(256);
  {
    u32 child_pp;
    if (read_u32(ee_mem, root_process + PTREE_CHILD_OFF, mem_size, child_pp)) {
      u32 first_child = 0;
      if (deref_ppointer(child_pp, first_child)) {
        stack.push_back(first_child);
      }
    }
  }

  std::unordered_set<u32> visited;
  visited.reserve(512);
  int nodes_visited = 0;

  while (!stack.empty() && nodes_visited < MAX_PROCESS_TREE_NODES) {
    u32 node = stack.back();
    stack.pop_back();
    if (node == 0 || node == false_val) continue;
    if (!visited.insert(node).second) continue;
    nodes_visited++;

    if (!valid_basic_ptr(node, mem_size)) continue;

    // Enqueue brother + child.
    u32 brother_pp = 0, child_pp = 0;
    read_u32(ee_mem, node + PTREE_BROTHER_OFF, mem_size, brother_pp);
    read_u32(ee_mem, node + PTREE_CHILD_OFF, mem_size, child_pp);
    u32 brother = 0, child = 0;
    if (deref_ppointer(brother_pp, brother) && visited.find(brother) == visited.end()) {
      stack.push_back(brother);
    }
    if (deref_ppointer(child_pp, child) && visited.find(child) == visited.end()) {
      stack.push_back(child);
    }

    // Is this node a yakow?
    u32 node_type;
    if (!read_basic_type(ee_mem, node, mem_size, node_type)) continue;
    if (node_type == node || !valid_basic_ptr(node_type, mem_size)) continue;
    if (!type_is_descendant(ee_mem, mem_size, node_type, yakow_type, is_yakow_cache)) {
      continue;
    }

    // Read root->trans (same path as write_mario_pos_to_target).
    u32 root;
    if (!read_u32(ee_mem, node + PDRAW_ROOT_OFF, mem_size, root)) continue;
    if (root == 0 || root == false_val) continue;
    if (!valid_basic_ptr(root, mem_size)) continue;

    float trans[4];
    if (!read_vec4(ee_mem, root + CSHAPE_TRANS_OFF, mem_size, trans)) continue;

    YakowRecord r;
    r.ee_addr = node;
    r.trans_jak[0] = trans[0];
    r.trans_jak[1] = trans[1];
    r.trans_jak[2] = trans[2];
    out.push_back(r);
  }
}

// Overwrite the trans field of a yakow process-drawable in EE memory. Keeps
// the w component at 1.0 (matches the vec4f layout Jak uses for positions).
bool write_yakow_trans(u8* ee_mem, u32 mem_size, u32 false_val, u32 pd_addr,
                       float x, float y, float z) {
  using namespace ac;
  if (ee_mem == nullptr || pd_addr == 0 || pd_addr == false_val) return false;
  if (!valid_basic_ptr(pd_addr, mem_size)) return false;
  u32 root;
  if (!read_u32(ee_mem, pd_addr + PDRAW_ROOT_OFF, mem_size, root)) return false;
  if (root == 0 || root == false_val) return false;
  if (!valid_basic_ptr(root, mem_size)) return false;
  u32 trans_addr = root + CSHAPE_TRANS_OFF;
  if (!valid_ee_addr(trans_addr, 16, mem_size)) return false;
  float trans[4] = {x, y, z, 1.0f};
  std::memcpy(ee_mem + trans_addr, trans, 16);
  return true;
}

}  // namespace

void LibSM64Manager::update_yakow_grab(u8* ee_mem) {
  if (!m_initialized || m_mario_id < 0 || !ee_mem) {
    clear_yakow_grab();
    return;
  }
  if (!yakow_grab) {
    clear_yakow_grab();
    return;
  }

  u32 false_val = s7.offset;
  if (false_val == 0) return;

  // Lazy yakow-type resolve. The symbol may not exist yet on the first few
  // frames (e.g. before the village1 level has loaded its code), so a failed
  // lookup is silent and retried next frame.
  if (m_yakow_type == 0) {
    auto y_sym = jak1::find_symbol_from_c("yakow");
    if (y_sym.offset == 0) return;
    u32 y_val = y_sym->value;
    if (y_val == 0 || y_val == false_val) return;
    m_yakow_type = y_val;
    lg::info("[libsm64] Yakow grab type cache ready: yakow=0x{:X}", y_val);
  }

  // We also need the active-pool symbol. Reuse m_type_cache if the actor
  // collision walker has already populated it, otherwise look it up ourselves.
  u32 active_pool_sym = m_type_cache.active_pool_sym;
  if (active_pool_sym == 0) {
    auto ap = jak1::find_symbol_from_c("*active-pool*");
    if (ap.offset == 0) return;
    active_pool_sym = ap.offset;
  }

  // Walk the process tree and collect all live yakows.
  std::vector<YakowRecord> yakows;
  collect_yakows(ee_mem, EE_MAIN_MEM_SIZE, false_val, active_pool_sym, m_yakow_type,
                  m_is_yakow_cache, yakows);

  // Current Mario state (Jak-unit position, radians face angle).
  MarioState state = get_state();

  // ---- Case 1: Mario is currently holding a yakow (via the fake-held API) --
  if (m_grabbed_yakow_ee != 0) {
    // Did SM64's action machine release our fake held object on its own
    // (natural throw / drop / damage)? If so, release the yakow too.
    int still_holding = 0;
    {
      std::scoped_lock lock(m_sm64_lock);
      still_holding = sm64_mario_is_holding_fake(m_mario_id);
    }
    if (!still_holding) {
      lg::info("[libsm64] yakow grab: SM64 released fake held object — freeing yakow 0x{:X}",
               m_grabbed_yakow_ee);
      m_grabbed_yakow_ee = 0;
      return;
    }

    // Is the held yakow process still alive (i.e. still in our discovered list)?
    bool still_alive = false;
    for (const auto& r : yakows) {
      if (r.ee_addr == m_grabbed_yakow_ee) {
        still_alive = true;
        break;
      }
    }
    if (!still_alive) {
      lg::info("[libsm64] yakow grab: held yakow 0x{:X} no longer in process tree — releasing",
               m_grabbed_yakow_ee);
      {
        std::scoped_lock lock(m_sm64_lock);
        sm64_mario_end_fake_hold(m_mario_id);
      }
      m_grabbed_yakow_ee = 0;
      return;
    }

    // Glue the yakow to Mario's hand. Hold position is mario_pos + forward *
    // yakow_hold_forward + up * yakow_hold_up, where forward = (sin(yaw), 0,
    // cos(yaw)) (SM64 yaw-forward convention, matching mario_throw_held_object).
    const float forward_jak = yakow_hold_forward_sm64 * SM64_TO_JAK_SCALE;
    const float up_jak = yakow_hold_up_sm64 * SM64_TO_JAK_SCALE;
    const float yaw = state.face_angle;
    const float sx = std::sin(yaw);
    const float sz = std::cos(yaw);
    const float hold_x = state.position.x() + sx * forward_jak;
    const float hold_y = state.position.y() + up_jak;
    const float hold_z = state.position.z() + sz * forward_jak;
    if (!write_yakow_trans(ee_mem, EE_MAIN_MEM_SIZE, false_val, m_grabbed_yakow_ee,
                            hold_x, hold_y, hold_z)) {
      // Write failed (stale ptr, etc) — release.
      lg::warn("[libsm64] yakow grab: write_yakow_trans failed for 0x{:X} — releasing",
               m_grabbed_yakow_ee);
      {
        std::scoped_lock lock(m_sm64_lock);
        sm64_mario_end_fake_hold(m_mario_id);
      }
      m_grabbed_yakow_ee = 0;
    }
    return;
  }

  // ---- Case 2: Not holding. Maybe start a grab this frame? -----------------
  // Only on the rising edge of button B. sm64_mario_tick has already consumed
  // this same press as a punch by now, but calling begin_fake_hold below
  // overrides the action.
  const bool b_just_pressed = m_cur_button_b && !m_prev_button_b;
  if (!b_just_pressed) return;

  // Be permissive about when we'll accept a grab. SM64's native
  // able_to_grab_object() is restrictive (only ACT_PUNCHING / ACT_DIVE /
  // etc), but we override the action directly via begin_fake_hold so we
  // can come out of almost any state. The only things we'd want to avoid
  // are hold-group actions (already holding) and cutscenes — but
  // begin_fake_hold's own end_fake_hold path handles the hold-group case,
  // and cutscenes aren't hit in practice. Reserved for future filtering.

  // Find the closest yakow within yakow_grab_radius_sm64 (SM64 units). We
  // compare in Jak units by scaling the threshold.
  const float radius_jak = yakow_grab_radius_sm64 * SM64_TO_JAK_SCALE;
  const float radius_sq_jak = radius_jak * radius_jak;
  u32 best_ee = 0;
  float best_d2 = radius_sq_jak;
  for (const auto& r : yakows) {
    float dx = r.trans_jak[0] - state.position.x();
    float dy = r.trans_jak[1] - state.position.y();
    float dz = r.trans_jak[2] - state.position.z();
    float d2 = dx * dx + dy * dy + dz * dz;
    if (d2 < best_d2) {
      best_d2 = d2;
      best_ee = r.ee_addr;
    }
  }
  if (best_ee == 0) return;

  // Commit the grab.
  lg::info("[libsm64] yakow grab: grabbing yakow 0x{:X} at dist {:.0f} Jak units",
           best_ee, std::sqrt(best_d2));
  {
    std::scoped_lock lock(m_sm64_lock);
    sm64_mario_begin_fake_hold(m_mario_id);
  }
  m_grabbed_yakow_ee = best_ee;
}

void LibSM64Manager::clear_yakow_grab() {
  if (m_grabbed_yakow_ee != 0) {
    if (m_initialized && m_mario_id >= 0) {
      std::scoped_lock lock(m_sm64_lock);
      sm64_mario_end_fake_hold(m_mario_id);
    }
    m_grabbed_yakow_ee = 0;
  }
}

// --------------------------------------------------------------------------
// Zoomer → shell state
// --------------------------------------------------------------------------
//
// Goal: when the player tries to hop on a zoomer (the hover-bike in Fire
// Canyon / Lava Tube / Misty Island / Rolling Hills / Ogre), we want Mario
// to enter ACT_RIDING_SHELL_GROUND so he surfs along on a Koopa shell, and
// we want Jak to NOT enter the target-racing-* states at all — the racer
// has a bad camera and loud engine SFX that we don't want fighting with
// the libsm64 experience.
//
// We do this with a two-symbol handshake defined in target-handler.gc:
//
//   *sm64-skip-zoomer*      — C++ raises this whenever libsm64 is live and
//                             zoomer_shell is on. target's 'racing
//                             change-mode event handler checks it and, if
//                             true, swallows the event (no `go
//                             target-racing-start`) — Jak stays in his
//                             current state with the normal camera.
//   *sm64-zoomer-requested* — target-handler sets this to #t whenever the
//                             swallowed event fired. C++ polls it each
//                             tick and, when set, puts Mario into the
//                             ground shell action and clears it back to
//                             #f for the next request.
//
// Benefits of this design:
//
//   1) Jak never enters the racer, so the bad camera and engine sounds
//      never play.
//   2) We don't need to poll or walk Jak's state.next-state field —
//      GOAL tells us directly when a zoomer was requested.
//   3) Mario inherits the riding-shell action flag, which gives him
//      native lava immunity via check_lava_boost (interaction.c:902).
//      We also skip our host-side lava kick in update_mario_water for
//      the same reason, so lava rocks in Fire Canyon / Lava Tube can't
//      burn Mario while he's on the shell.
//
// Graceful degradation: if neither symbol resolves (e.g. on an older GOAL
// build without the bridge defines), this function is a silent no-op.
// Rebuilding goal_src picks up the bridge.

void LibSM64Manager::update_zoomer_shell(u8* ee_mem) {
  if (!m_initialized || m_mario_id < 0 || !ee_mem) return;

  const u32 true_val = s7.offset + jak1_symbols::FIX_SYM_TRUE;
  const u32 false_val = s7.offset + jak1_symbols::FIX_SYM_FALSE;
  if (s7.offset == 0) return;

  // Keep *sm64-skip-zoomer* in sync with our toggle every frame — GOAL
  // reads it synchronously from the 'racing event handler, so writing it
  // each tick is the simplest way to ensure it's current. If the symbol
  // doesn't exist yet (target-handler not linked), do nothing.
  auto skip_sym = jak1::find_symbol_from_c("*sm64-skip-zoomer*");
  if (skip_sym.offset != 0) {
    const u32 desired = (zoomer_shell && has_mario()) ? true_val : false_val;
    if (skip_sym->value != desired) {
      skip_sym->value = desired;
    }
  }

  if (!zoomer_shell) return;

  // Check whether GOAL just swallowed a 'racing event — that's our cue
  // that the player tried to hop on a zoomer. Clear the flag and put
  // Mario into shell mode.
  auto req_sym = jak1::find_symbol_from_c("*sm64-zoomer-requested*");
  if (req_sym.offset == 0) return;                  // GOAL bridge not linked
  if (req_sym->value != true_val) return;           // no pending request

  // Clear first so subsequent requests (e.g. tapping attack again after
  // exiting shell) retrigger cleanly, even if we bail out below.
  req_sym->value = false_val;

  // Snapshot Mario's current action under the geo mutex. If he's already
  // in any shell sub-action (ground / jump / fall), don't re-issue the
  // set — re-applying ACT_RIDING_SHELL_GROUND every request would stomp
  // the natural jump/fall transitions inside act_riding_shell_ground
  // and pin him flat to the ground.
  uint32_t current_action = 0;
  {
    std::lock_guard<std::mutex> g(m_geo_mutex);
    current_action = m_state.action;
  }

  // ACT constants from libsm64/src/decomp/include/sm64.h
  constexpr uint32_t kActRidingShellGround = 0x20810446;
  constexpr uint32_t kActFlagRidingShell = 0x00010000;

  const bool mario_in_shell = (current_action & kActFlagRidingShell) != 0;
  if (mario_in_shell) return;

  std::scoped_lock lock(m_sm64_lock);
  sm64_set_mario_action(m_mario_id, kActRidingShellGround);
  lg::info("[libsm64] zoomer-shell: player tried to ride zoomer, "
           "Mario → ACT_RIDING_SHELL_GROUND (Jak stays out of racer)");
}

// --------------------------------------------------------------------------
// Launcher glue
// --------------------------------------------------------------------------
//
// Jak 1 launchers (spring pads) send a 'launch event that puts Jak into the
// target-launch → target-duck-high-jump → target-duck-high-jump-jump state
// chain.  During this time GOAL controls Jak's trajectory entirely.  If we
// let the normal Mario→Jak position sync run, it would overwrite Jak's
// launch position with Mario's (still on the ground), breaking the arc.
//
// We detect the launch by reading the target process's current state name
// from EE memory and comparing against the known launcher state chain,
// then glue Mario to Jak's position until the state exits.

bool LibSM64Manager::update_launcher_glue(u8* ee_mem) {
  if (!m_initialized || m_mario_id < 0 || !ee_mem) {
    m_in_launcher = false;
    return false;
  }
  if (!follow_mario) {
    m_in_launcher = false;
    return false;
  }

  const u32 false_val = s7.offset;
  if (false_val == 0) return false;

  auto target_sym = jak1::intern_from_c("*target*");
  if (target_sym.offset == 0) return false;
  u32 target_ptr = target_sym->value;
  if (target_ptr == 0 || target_ptr == false_val) return false;

  // process.state is at GOAL offset 56 → runtime offset 52.
  constexpr u32 STATE_RUNTIME_OFF = 52;
  if (target_ptr + STATE_RUNTIME_OFF + 4 > EE_MAIN_MEM_SIZE) return false;

  u32 state_ptr;
  std::memcpy(&state_ptr, ee_mem + target_ptr + STATE_RUNTIME_OFF, 4);
  if (state_ptr == 0 || state_ptr == false_val) return false;

  // Validate state_ptr is within EE memory before reading from it.
  if (state_ptr >= EE_MAIN_MEM_SIZE) return false;

  // stack-frame.name is a symbol at GOAL offset 4 → runtime offset 0.
  if (state_ptr + 4 > EE_MAIN_MEM_SIZE) return false;
  u32 state_name;
  std::memcpy(&state_name, ee_mem + state_ptr, 4);

  // Resolve the launcher state symbols. find_symbol_from_c is read-only
  // (won't create new symbols), safer than intern_from_c during state
  // transitions.
  auto sym_launch    = jak1::find_symbol_from_c("target-launch");
  auto sym_high_jump = jak1::find_symbol_from_c("target-high-jump");
  auto sym_duck_hj   = jak1::find_symbol_from_c("target-duck-high-jump");
  auto sym_duck_hj_j = jak1::find_symbol_from_c("target-duck-high-jump-jump");

  // If any symbol isn't linked yet, bail gracefully.
  if (sym_launch.offset == 0 || sym_high_jump.offset == 0 ||
      sym_duck_hj.offset == 0 || sym_duck_hj_j.offset == 0) {
    return false;
  }

  const bool jak_in_launcher =
      (state_name == sym_launch.offset) ||
      (state_name == sym_high_jump.offset) ||
      (state_name == sym_duck_hj.offset) ||
      (state_name == sym_duck_hj_j.offset);

  if (!jak_in_launcher) {
    if (m_in_launcher) {
      lg::info("[libsm64] Launcher ended — resuming normal sync");
    }
    m_in_launcher = false;
    return false;
  }

  // Jak is in a launcher state — read Jak's position and store it.
  // The actual Mario position override happens inside tick() after
  // sm64_mario_tick, within the existing sm64_lock scope.
  if (!m_in_launcher) {
    lg::info("[libsm64] Launcher detected — gluing Mario to Jak");
  }
  m_in_launcher = true;

  math::Vector3f jak_pos;
  if (read_target_transform(ee_mem, &jak_pos, nullptr)) {
    m_launcher_target_jak = jak_pos;
  }

  return true;  // caller should skip sync_jak_to_mario
}

}  // namespace sm64
