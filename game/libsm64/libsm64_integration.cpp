/*!
 * @file libsm64_integration.cpp
 * Implementation of the libsm64 integration manager.
 */

#include "libsm64_integration.h"

#include "sm64_audio.h"

#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <thread>

#ifdef _WIN32
// windows.h defines min/max as macros which clash with std::numeric_limits
// <T>::max() further down the file — NOMINMAX disables them.  WIN32_LEAN_
// AND_MEAN trims the kitchen-sink Windows include set down to what we
// actually need (HWND, FindWindowA, SetWindowPos, Sleep, CreateProcessA).
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#include <sys/wait.h>
#endif

#include "common/goal_constants.h"
#include "common/log/log.h"
#include "common/symbols.h"
#include "common/util/FileUtil.h"
#include "game/kernel/common/Ptr.h"
#include "game/kernel/common/Symbol4.h"
#include "game/kernel/common/kscheme.h"
#include "game/kernel/jak1/kscheme.h"
#include "game/kernel/jak2/kscheme.h"
#include "game/runtime.h"  // g_game_version
#include "common/versions/versions.h"  // GameVersion enum

#include "third-party/libtinyfiledialogs/tinyfiledialogs.h"

extern "C" {
#include "libsm64.h"
// load_surfaces.h transitively pulls in ultratypes.h which typedefs s64/u64
// as 'signed long long int'/'unsigned long long int'.  On Linux, int64_t
// (used by common_types.h for s64) is 'long', making these distinct types
// and producing a hard "type alias redefinition" error on clang.
// Redirect the conflicting typedefs to private throwaway aliases for the
// duration of this include.  The function declarations in load_surfaces.h
// itself don't use s64/u64 in their signatures, so this is safe.
#define s64 s64__libsm64_private_
#define u64 u64__libsm64_private_
#include "load_surfaces.h"
#undef s64
#undef u64
#include "decomp/tools/libmio0.h"
}

namespace sm64 {

// ---------------------------------------------------------------------------
// Version-aware symbol-table helpers.
//
// jak1 and jak2 have different Symbol layouts:
//   - jak1: Ptr<Symbol> with `value` field (u32)
//   - jak2: Ptr<Symbol4<u32>> with `value()` method
//
// Plus FIX_SYM_TRUE differs (0x8 in jak1, 0x4 in jak2/3/X).  These helpers
// pick the right path based on g_game_version so the caller can stay
// version-agnostic.  Returns 0 when the symbol doesn't exist yet (e.g.
// the watcher process hasn't linked mario.gc), or when s7 isn't ready,
// or when the version isn't supported here.  All callers were already
// guarding against the 0 case so this preserves their semantics.
//
// Same applies for find_symbol_from_c — it doesn't intern, so safe to use
// for "is this symbol defined?" checks.
// ---------------------------------------------------------------------------
[[maybe_unused]] static u32 sm64_get_symbol_value(const char* name) {
  if (s7.offset == 0) return 0;
  if (g_game_version == GameVersion::Jak1) {
    auto sym = jak1::intern_from_c(name);
    if (sym.offset == 0) return 0;
    return sym->value;
  } else if (g_game_version == GameVersion::Jak2) {
    auto sym = jak2::intern_from_c(name);
    if (sym.offset == 0) return 0;
    return sym->value();
  }
  return 0;
}

[[maybe_unused]] static u32 sm64_find_symbol_value(const char* name) {
  if (s7.offset == 0) return 0;
  if (g_game_version == GameVersion::Jak1) {
    auto sym = jak1::find_symbol_from_c(name);
    if (sym.offset == 0) return 0;
    return sym->value;
  } else if (g_game_version == GameVersion::Jak2) {
    auto sym = jak2::find_symbol_from_c(name);
    if (sym.offset == 0) return 0;
    return sym->value();
  }
  return 0;
}

// Set a symbol's value.  Returns true if the symbol existed.  Uses
// find_ rather than intern_ so we don't accidentally create a symbol
// in the table just because GOAL hasn't defined it yet.
[[maybe_unused]] static bool sm64_set_symbol_value(const char* name, u32 value) {
  if (s7.offset == 0) return false;
  if (g_game_version == GameVersion::Jak1) {
    auto sym = jak1::find_symbol_from_c(name);
    if (sym.offset == 0) return false;
    sym->value = value;
    return true;
  } else if (g_game_version == GameVersion::Jak2) {
    auto sym = jak2::find_symbol_from_c(name);
    if (sym.offset == 0) return false;
    sym->value() = value;
    return true;
  }
  return false;
}

// Address of the symbol itself in EE memory (NOT the symbol's value).
// Used when caching "where does this symbol live" for later quick reads.
// Returns 0 if the symbol doesn't exist or s7 isn't ready.
[[maybe_unused]] static u32 sm64_get_symbol_offset(const char* name) {
  if (s7.offset == 0) return 0;
  if (g_game_version == GameVersion::Jak1) {
    auto sym = jak1::intern_from_c(name);
    return sym.offset;
  } else if (g_game_version == GameVersion::Jak2) {
    auto sym = jak2::intern_from_c(name);
    return sym.offset;
  }
  return 0;
}

// find-only variant — does NOT intern a new symbol slot if the name
// isn't already in the table.  Use when comparing symbol identity
// (e.g. *master-mode* against 'movie) where a missing symbol means
// "not interesting" rather than "needs to exist for this code path".
[[maybe_unused]] static u32 sm64_find_symbol_offset(const char* name) {
  if (s7.offset == 0) return 0;
  if (g_game_version == GameVersion::Jak1) {
    auto sym = jak1::find_symbol_from_c(name);
    return sym.offset;
  } else if (g_game_version == GameVersion::Jak2) {
    auto sym = jak2::find_symbol_from_c(name);
    return sym.offset;
  }
  return 0;
}

// EE-memory offset of #t for the current game version (s7 + FIX_SYM_TRUE).
// Returns 0 if s7 isn't ready.
[[maybe_unused]] static u32 sm64_true_offset() {
  if (s7.offset == 0) return 0;
  return s7.offset + true_symbol_offset(g_game_version);
}

// ---------------------------------------------------------------------------
// Version-aware GOAL struct field offsets.
//
// process / process-drawable layouts diverge between jak 1 and jak 2.
// Jak 2 adds:
//   - process-tree: a `clock` field (4 bytes, between `mask` and `parent`)
//   - process: a `level` field (4 bytes, between `entity` and `state`)
//   - process: `pad-unknown-0` (uint32 2 = 8 bytes) before heap fields
// All three add +4 each → +16 bytes total, NOT +12 as the first attempt
// at this table assumed.  Missing the `clock` field made the runtime read
// 4 bytes early, which on the live test gave coords that LOOKED like
// Jak's position (because the GOAL heap stores adjacent pointers to
// related struct slots) but actually read into the connection-list
// inline, so Mario spawned at a "weird spot."
//
// All offsets are RUNTIME offsets (relative to the basic-pointer that
// `intern_from_c(name)->value()` returns — the basic header tag is at -4
// from this pointer).
//
// Layouts (computed by stepping through gkernel-h.gc + game-h.gc):
//
//                  jak1   jak2   delta
//   process.state    52     60    +8   (jak2: +clock @ pt + +level @ p)
//   pd.root         108    124   +16   (+8 above + 8-byte pad-unknown-0)
//   pd.node-list    112    128   +16
//   pd.water        152    168   +16
//   trsqv.trans      12     12    0    (engine struct, identical layout)
//   trsqv.quat       28     28    0
// ---------------------------------------------------------------------------
struct TargetOffsets {
  u32 process_state;               // process.state runtime offset
  u32 process_drawable_root;       // process-drawable.root runtime offset
  u32 process_drawable_node_list;  // process-drawable.node-list runtime offset
  u32 process_drawable_water;      // process-drawable.water runtime offset
                                   //   (root + 44, since water is the 11th
                                   //   field after root in both versions)
  u32 trsqv_trans;                 // trsqv.trans runtime offset
  u32 trsqv_quat;                  // trsqv.quat (overlay rot.x) runtime offset
};

[[maybe_unused]] static TargetOffsets sm64_target_offsets() {
  if (g_game_version == GameVersion::Jak2) {
    // jak 2: process-tree adds `clock` (+4), process adds `level` (+4) and
    // `pad-unknown-0` (+8).  Total +16 bytes vs jak 1.  Engine struct
    // trsqv is unchanged.  process-drawable's own fields shift by the
    // same +16 since they all sit after the process header.
    return TargetOffsets{
        /*process_state=*/60,
        /*process_drawable_root=*/124,
        /*process_drawable_node_list=*/128,
        /*process_drawable_water=*/168,  // root(124) + 44 (11th 4-byte field)
        /*trsqv_trans=*/12,
        /*trsqv_quat=*/28,
    };
  }
  // Jak 1 (and fallback for any version we haven't computed yet).
  return TargetOffsets{
      /*process_state=*/52,
      /*process_drawable_root=*/108,
      /*process_drawable_node_list=*/112,
      /*process_drawable_water=*/152,  // root(108) + 44
      /*trsqv_trans=*/12,
      /*trsqv_quat=*/28,
  };
}

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

  // Apply startup defaults for our fork extensions.
  // Wall extrusion is OFF by default (test_new_collide_toggle=false),
  // so start with 0.25 so libsm64 natively recognises Jak's tilted walls
  // without extra geometry.  When the user turns extrusion ON the GUI
  // drops it back to the vanilla SM64 threshold (0.01).
  sm64_set_wall_ny_threshold(0.25f);

  m_last_rom_path = rom_path;
  // Release-store: must be the LAST write in init().  Any thread that
  // sees m_initialized==true via the acquire-load in is_initialized()
  // is guaranteed to also see every write done before this line — the
  // sm64_global_init internals, audio engine setup, shell extraction,
  // wall_ny_threshold, m_last_rom_path.  This is the right correctness
  // fix even though init() now only runs at cold boot (single-threaded
  // context) — defensive against any future call site that runs init
  // while other threads are alive.
  m_initialized.store(true, std::memory_order_release);
  lg::info("[libsm64] Initialized successfully");
  return true;
}

bool LibSM64Manager::init_autodetect() {
  if (m_initialized) {
    return true;
  }
  // Uses the project's ghc::filesystem alias from FileUtil.h.

  // Search order:
  //   1. %APPDATA%/OpenGOAL/mario/ (Windows) or the equivalent user config
  //      dir — persistent across build-tree wipes and mod installer updates,
  //      so once a user picks a ROM they never have to re-pick.  This is
  //      where we copy ROMs on first pick (see the tinyfd path below).
  //   2. Directory next to gk.exe — lets power users drop a ROM in-place
  //      without hunting for the config dir.
  //   3. iso_data/mario/ under the project dir — legacy location, kept so
  //      existing dev setups keep working.
  // We pick the first .z64 whose size matches the expected US ROM size.
  std::vector<fs::path> search_dirs;
  try {
    fs::path user_cfg = file_util::get_user_config_dir();
    if (!user_cfg.empty()) {
      search_dirs.push_back(user_cfg / "mario");
    }
  } catch (...) {
    // non-fatal; lower-priority dirs below may still find the ROM
  }
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
    // SILENT failure — do NOT pop any OS-level prompt here.  The
    // GOAL-side ROM-required dialog (progress-screen mario-rom-required
    // in progress-pc.gc) handles user-facing prompting via the in-game
    // pause-style UI, fired from title-obs.gc's startup / ndi state
    // :code BEFORE the cinematic plays.  Going through GOAL keeps the
    // experience identical on Windows and Linux (no native MessageBox
    // platform differences) and stays in-engine for immersion.
    lg::warn("[libsm64] Auto-detect: no matching .z64 found in user config dir, next to gk, or in iso_data/mario "
             "(silent — GOAL boot prompt will trigger pc-sm64-prompt-for-rom when user-facing UI is ready)");
    return false;
  }
  lg::info("[libsm64] Auto-detected ROM: {}", picked.string());
  return init(picked.string());
}

// Spawn a fresh gk process with the same exe + cwd as the current one,
// then exit the current process.  Called after successfully copying a
// ROM to %APPDATA%/OpenGOAL/mario/ in prompt_for_rom_and_init — auto-
// restart so the new process's init_autodetect picks up the saved ROM
// at boot (where init() runs single-threaded and works cleanly), since
// initializing libsm64 mid-runtime crashes downstream of the
// m_initialized flip and we couldn't pin the exact cause.
//
// Marked [[noreturn]] but its return path is technically reachable on
// CreateProcessA / fork failure — those still fall through to a hard
// std::exit(1).  The user gets nothing useful in that case but at
// least gk doesn't hang.
[[noreturn]] static void relaunch_and_exit() {
  std::string exe = file_util::get_current_executable_path();
  if (exe.empty()) {
    lg::error("[libsm64] relaunch_and_exit: could not determine current exe path");
    std::exit(1);
  }
  lg::info("[libsm64] relaunch_and_exit: spawning new gk @ {}", exe);

#ifdef _WIN32
  STARTUPINFOA si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  // CreateProcessA may modify the lpCommandLine buffer; pass a writable
  // std::string buffer.  We don't forward argv — gk almost never takes
  // user-facing args (the few that exist are dev-only) and a simple
  // bare exe spawn matches the user's typical launch path.
  std::string cmd = exe;
  if (!CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, FALSE,
                      0, nullptr, nullptr, &si, &pi)) {
    lg::error("[libsm64] CreateProcess failed (err {})", GetLastError());
    std::exit(1);
  }
  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);
  lg::info("[libsm64] new gk spawned (pid {}); exiting current", pi.dwProcessId);
  // ExitProcess instead of std::exit: bypass the static destructor /
  // atexit chain because GOAL is mid-coroutine and the renderer thread
  // is still ticking — graceful std::exit can hang or crash trying to
  // join those.  The new gk has its own clean state.
  ExitProcess(0);
#else
  // POSIX: fork, child execv into the new gk, parent exits.
  pid_t child = fork();
  if (child < 0) {
    lg::error("[libsm64] fork failed; cannot auto-restart");
    std::exit(1);
  }
  if (child == 0) {
    // child — replace process image with fresh gk
    char* argv[] = {const_cast<char*>(exe.c_str()), nullptr};
    execv(exe.c_str(), argv);
    _exit(1);  // execv only returns on failure
  }
  lg::info("[libsm64] new gk spawned (pid {}); exiting current", child);
  _exit(0);
#endif
}

bool LibSM64Manager::prompt_for_rom_and_init() {
  // If we're already initialized, nothing to do.  Caller (the GOAL ROM-
  // required dialog OK handler) checks rom_loaded? before calling, but
  // belt-and-suspenders against double-prompt during a race.
  if (m_initialized) {
    return true;
  }

  // Native OS file picker.  This BLOCKS the calling thread until the
  // user picks a file or cancels.  That's fine — we're called from the
  // GOAL UI thread while the user is sitting on a paused dialog screen,
  // so the game loop is already waiting.
  //
  // tinyfd hardcodes hwndOwner=0 on Windows (see tinyfiledialogs.c:1317),
  // so the picker has no parent and Windows happily lets gk's window
  // cover it when the user clicks gk.  Workaround: spawn a watchdog
  // thread that polls for the picker's window class ("#32770" — the
  // standard Win32 dialog class) and forces it HWND_TOPMOST every 50 ms
  // while the picker is up.  TOPMOST is sticky against other normal
  // windows but Windows may briefly demote it on focus changes, so we
  // re-apply continuously for the lifetime of the picker.  Thread exits
  // immediately once tinyfd returns.
  static const char kPickerTitle[] = "Select SM64 US ROM (.z64)";
  char const* filter_patterns[] = {"*.z64", "*.Z64"};
#ifdef _WIN32
  std::atomic<bool> picker_active{true};
  std::thread topmost_watcher([&picker_active]() {
    while (picker_active.load(std::memory_order_acquire)) {
      HWND dlg = FindWindowA("#32770", kPickerTitle);
      if (dlg) {
        SetWindowPos(dlg, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
      }
      Sleep(50);
    }
  });
#endif
  char const* selection = tinyfd_openFileDialog(
      kPickerTitle, "", 2, filter_patterns, "SM64 ROM files (*.z64)", 0);
#ifdef _WIN32
  picker_active.store(false, std::memory_order_release);
  topmost_watcher.join();
#endif
  if (!selection) {
    lg::warn("[libsm64] User cancelled ROM file selection");
    return false;
  }

  fs::path selected_rom(selection);
  std::error_code ec;
  auto sz = fs::file_size(selected_rom, ec);
  if (ec || sz != kExpectedSm64RomSize) {
    lg::error("[libsm64] Selected ROM has wrong size ({} bytes, expected {})",
              ec ? 0 : static_cast<size_t>(sz), static_cast<size_t>(kExpectedSm64RomSize));
    return false;
  }

  // Copy the ROM to %APPDATA%/OpenGOAL/mario so future launches find it
  // automatically.  This is the persistent location — it survives
  // build-tree wipes, mod updates that nuke iso_data/, and reinstalls of
  // the launcher, so once the user picks a ROM they never have to re-pick.
  try {
    fs::path user_cfg = file_util::get_user_config_dir();
    fs::path dest_dir = user_cfg / "mario";
    fs::create_directories(dest_dir, ec);
    fs::path dest = dest_dir / selected_rom.filename();
    fs::copy_file(selected_rom, dest, fs::copy_options::overwrite_existing, ec);
    if (ec) {
      lg::warn("[libsm64] Could not copy ROM to {}: {}", dest.string(), ec.message());
      return false;
    }
    lg::info("[libsm64] Copied ROM to {}; auto-restarting gk", dest.string());
  } catch (const std::exception& e) {
    lg::error("[libsm64] Exception copying ROM: {}", e.what());
    return false;
  }

  // Auto-restart gk so init_autodetect picks up the saved ROM cleanly
  // at boot.  We deliberately do NOT call init() here mid-runtime —
  // even with m_initialized as std::atomic<bool> (release/acquire
  // ordering, the right correctness fix for the flag itself), mid-
  // runtime init still crashes the process: something downstream of
  // the flag flip (renderer first tick, audio worker, GOAL OK-handler
  // resume, or some interaction between them) terminates gk.  Diag
  // prints confirmed init() returns cleanly to the GOAL bridge — the
  // crash happens AFTER pc_sm64_prompt_for_rom returns and before any
  // subsequent log line lands, with both renderer and GOAL threads
  // going silent simultaneously.
  //
  // relaunch_and_exit() spawns a fresh gk with the saved ROM in its
  // search path and exits the current process.  The user sees a brief
  // window flicker; the new gk loads the ROM via init_autodetect on
  // its first frame (single-threaded, no race) and proceeds to the
  // logo / NDI cinematic and title menu as if Mario had been there
  // from the start.
  relaunch_and_exit();
  // not reached
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

// Source-of-truth for the Mario scale factor.  g_libsm64_mario_scale is
// the C-linkage storage in libsm64.c; set_mario_scale bumps it AND
// updates the derived Jak-side SM64_TO_JAK_SCALE / JAK_TO_SM64_SCALE
// inline-variables so every downstream read lands on a consistent value
// within the same frame.
void set_mario_scale(float scale) {
  if (!std::isfinite(scale)) scale = 50.0f;
  if (scale < 1.0f)   scale = 1.0f;
  if (scale > 500.0f) scale = 500.0f;
  // Bridge to libsm64 — this is what mario_actions_moving.c and
  // mario_actions_submerged.c read in their walk-speed caps.
  sm64_set_mario_scale(scale);
  // Keep the Jak-side scale constants in lock step.
  SM64_TO_JAK_SCALE = 4096.0f / scale;
  JAK_TO_SM64_SCALE = scale / 4096.0f;
}

float get_mario_scale() {
  return g_libsm64_mario_scale;
}

// Source-of-truth for the "no slippery Mario" toggle.  Flips the C++
// bool (which load_level_collision reads the next time a level is
// streamed) and mirrors it into the libsm64 C-linkage int (which
// mario.c reads every frame in mario_floor_is_slippery).  Keeping both
// in one call prevents the two halves from drifting out of sync.
void set_no_slippery_mario(bool enabled) {
  g_no_slippery_mario = enabled;
  sm64_set_no_slippery_mario(enabled ? 1 : 0);
  // Wall NY threshold is driven by the wall-extrusion toggle, not this one.
}

bool get_no_slippery_mario() {
  return g_no_slippery_mario;
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
  clear_tar_floor();
  m_in_launcher = false;
  m_post_glue_settle_frames = 0;
  m_all_static_surfaces.clear();
  m_surface_centroids.clear();
  m_stream_loaded = false;
  m_stream_loaded_count = 0;
  // Bump so any renderer watching for changes drops its stale copy.
  m_static_surfaces_version++;
  m_type_cache = {};
  m_is_process_drawable_cache.clear();
  m_is_collide_shape_cache.clear();
  m_yakow_type = 0;
  m_is_yakow_cache.clear();
  // Drop cached BG music state so a fresh init (or re-init on ROM swap)
  // doesn't think the previous session's track is still playing and
  // short-circuit the next play_music_from_goal call.
  m_current_bg_music_seq = 0;
  m_current_bg_music_forced = false;
  m_force_audio_unpaused = false;

  if (m_mario_id >= 0) {
    sm64_mario_delete(m_mario_id);
    m_mario_id = -1;
  }
  // Clear respawn_pending on shutdown — a fresh init shouldn't inherit
  // "Mario needs to come back" from the previous session.
  m_respawn_pending = false;

  sm64_global_terminate();
  // Release-store on shutdown: pairs with the acquire-load in is_
  // initialized() so threads observing the flip-to-false also see all
  // the cleanup writes above (mario delete, audio teardown, etc.).
  m_initialized.store(false, std::memory_order_release);
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
  clear_tar_floor();

  // Reset the lava-entry edge state so a respawn into a dry area doesn't
  // see a stale "was in lava" from the last Mario's death-in-lava frame.
  m_prev_in_lava = false;
  m_in_launcher = false;
  m_post_glue_settle_frames = 0;

  // Convert Jak coordinates to SM64 coordinates
  // Jak: Y-up, same as SM64, but different scale
  float sm64_x = x * JAK_TO_SM64_SCALE;
  float sm64_y = y * JAK_TO_SM64_SCALE;
  float sm64_z = z * JAK_TO_SM64_SCALE;

  // If collision streaming is active, do an immediate load around the spawn
  // position so sm64_mario_create can find a floor. Without this, the
  // streaming subset is empty and Mario fails to spawn.
  {
    std::scoped_lock lock(m_sm64_lock);
    update_streaming_collision(sm64_x, sm64_z);
  }

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
    // Drop the safety floor and tar floor so a later respawn starts clean.
    clear_safety_floor();
    clear_tar_floor();
    sm64_mario_delete(m_mario_id);
    m_mario_id = -1;
  }
}

void LibSM64Manager::tick(const MarioInputState& input) {
  if (!m_initialized || m_mario_id < 0) return;

  // Post-restore input window: SM64 ticks normally but player input is zeroed
  // for a few frames after a shell-preserve restore.  Prevents accidental
  // shell exit from a button held through the cutscene, without the old
  // 2-second tick-skip that caused a jarring camera stall.
  MarioInputState effective_input = input;
  if (m_post_restore_freeze_ticks > 0) {
    --m_post_restore_freeze_ticks;
    effective_input = MarioInputState{};
  }

  // Suppress SM64 ticking during fuel-cell clone-anim (unless we're also in
  // a scripted movie, which has its own teleport-Mario-to-Jak logic and must
  // keep ticking).  This mirrors the movie-mode suppression and is the core
  // fix for "Mario sometimes kicked off shell after power-cell cutscene":
  //
  //   Old path: snapshot at clone-anim start -> SM64 keeps ticking through
  //   the whole cutscene -> restore at the end -> ONE settle tick with live
  //   player input -> then freeze.  That one settle tick is a race: if the
  //   player is pressing a button SM64 treats as exit-shell, Mario leaves
  //   shell in that tick, bridge writes on_shell=0, and GOAL fires the
  //   falling edge.
  //
  //   New path: SM64 is frozen the moment clone-anim starts.  m_state.action
  //   never changes (still shell), *sm64-mario-on-shell* stays 1.0 the whole
  //   time.  The restore on the falling edge is a no-op.  The settle tick
  //   starts from the correct shell state -- safe.
  if (target_clone_anim && !target_in_movie) {
    return;
  }

  // Edge-detect the B (punch/grab) button for update_yakow_grab. Shift the
  // previous frame into _prev first so (_cur && !_prev) becomes a single-
  // frame "just pressed" pulse.
  m_prev_button_b = m_cur_button_b;
  m_cur_button_b = effective_input.button_b;

  SM64MarioInputs sm64_input{};
  sm64_input.camLookX = effective_input.cam_look_x;
  sm64_input.camLookZ = effective_input.cam_look_z;
  sm64_input.stickX = effective_input.stick_x;
  sm64_input.stickY = effective_input.stick_y;
  sm64_input.buttonA = effective_input.button_a ? 1 : 0;
  sm64_input.buttonB = effective_input.button_b ? 1 : 0;
  sm64_input.buttonZ = effective_input.button_z ? 1 : 0;

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
    // Stream in nearby collision surfaces before the tick so find_floor /
    // find_ceil / find_wall only iterate over a small subset.
    update_streaming_collision(m_state.position.x() * JAK_TO_SM64_SCALE,
                               m_state.position.z() * JAK_TO_SM64_SCALE);

    update_safety_floor(m_state.position.x() * JAK_TO_SM64_SCALE,
                        m_state.position.y() * JAK_TO_SM64_SCALE,
                        m_state.position.z() * JAK_TO_SM64_SCALE);
    update_tar_floor(m_state.position.x() * JAK_TO_SM64_SCALE,
                     m_state.position.z() * JAK_TO_SM64_SCALE);

    // printf("[tick] Mario input: stick=(%.2f, %.2f) camLook=(%.2f, %.2f) buttons=(A=%d B=%d Z=%d)\n",
    //              sm64_input.stickX, sm64_input.stickY,
    //              sm64_input.camLookX, sm64_input.camLookZ,
    //              sm64_input.buttonA, sm64_input.buttonB, sm64_input.buttonZ);

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
    // m_post_glue_settle_frames > 0 means the glue state just ended but we're
    // still pinning Mario to Jak while collision reloads after a level change.
    if (m_in_launcher || m_post_glue_settle_frames > 0) {
      float lx = m_launcher_target_jak.x() * JAK_TO_SM64_SCALE;
      float ly = m_launcher_target_jak.y() * JAK_TO_SM64_SCALE;
      float lz = m_launcher_target_jak.z() * JAK_TO_SM64_SCALE;

      sm64_set_mario_position(m_mario_id, lx, ly, lz);
      sm64_state.position[0] = lx;
      sm64_state.position[1] = ly;
      sm64_state.position[2] = lz;
    }

    // --- Debug hover: R2 in debug mode slowly lifts Mario upward ----------
    // Set by pc_sm64_hover_mario (called from GOAL every frame R2 is held).
    // Consumes the flag atomically so hover stops as soon as GOAL stops
    // calling (i.e. R2 released or debug mode left).
    if (m_hover_requested.load(std::memory_order_acquire)) {
      m_hover_requested.store(false, std::memory_order_release);
      // Rise: ~512 Jak units per SM64 tick (matches Jak's debug float rate).
      constexpr float kHoverRiseJak = 800.0f;
      float new_y_sm64 = sm64_state.position[1] + kHoverRiseJak * JAK_TO_SM64_SCALE;
      sm64_set_mario_position(m_mario_id,
                              sm64_state.position[0],
                              new_y_sm64,
                              sm64_state.position[2]);
      // Zero vertical velocity so gravity doesn't fight the hover each tick.
      sm64_set_mario_velocity(m_mario_id,
                              sm64_state.velocity[0],
                              0.0f,
                              sm64_state.velocity[2]);
      // Force freefall so grounded action handlers don't snap Mario back to
      // the floor on the next substep.
      constexpr uint32_t kActFreefall = 0x0100088C;  // ACT_FREEFALL
      sm64_set_mario_action(m_mario_id, kActFreefall);
      sm64_state.position[1] = new_y_sm64;
      sm64_state.velocity[1] = 0.0f;
    }

  }

  // Post-restore shell re-assertion: keep m_state.action shell-flagged for
  // the entire input-zero window so GOAL never fires the shell-exit falling
  // edge during the transition after a cell-pickup cutscene.
  //
  // Why SM64 might drop the shell action on the first tick after restore:
  // act_riding_shell_ground in native SM64 checks riddenObj != NULL.  Our
  // shell is a Jak collision object, not an SM64 object, so riddenObj may
  // be NULL.  The libsm64 fork may or may not have patched this check, but
  // we hedge regardless: if sm64_state.action lost the shell flag, silently
  // re-assert the saved action and override sm64_state before m_state is
  // written.  The input-zero prevents B-button from exiting during the window.
  if (m_post_restore_shell_action != 0) {
    const bool action_dropped = !(sm64_state.action & 0x00010000u);
    if (action_dropped) {
      std::scoped_lock relock(m_sm64_lock);
      sm64_set_mario_action(m_mario_id, m_post_restore_shell_action);
      sm64_state.action = m_post_restore_shell_action;
    }
    // Clear on the last frame of the window (freeze_ticks was already
    // decremented at the top of this function).
    if (m_post_restore_freeze_ticks == 0) {
      m_post_restore_shell_action = 0;
    }
  }

  // Copy results into our managed buffers (threadsafe)
  std::lock_guard<std::mutex> lock(m_geo_mutex);

  // Snapshot the outgoing tick's geometry+state so the renderer can lerp
  // between them on the intervening 60fps frame.
  m_prev_geometry = m_geometry;
  m_prev_state = m_state;

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

  // ---- Star dance timeout --------------------------------------------------
  // general_star_dance_handler is commented out in libsm64, so Mario never
  // transitions back to idle on his own.  We count 80 ticks (~2.6s at 30Hz,
  // matching the original handler's frame-80 exit) then force ACT_IDLE.
  if (m_star_dance_timer >= 0) {
    ++m_star_dance_timer;
    if (m_star_dance_timer >= 80) {
      sm64_set_mario_action(m_mario_id, 0x0C400201);  // ACT_IDLE
      m_star_dance_timer = -1;
    }
  }

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

// ---------------------------------------------------------------------------
// Collision streaming: only feed triangles near Mario to libsm64.
// Called inside tick() under m_sm64_lock, before sm64_mario_tick.
// ---------------------------------------------------------------------------
void LibSM64Manager::update_streaming_collision(float mario_x_sm64, float mario_z_sm64) {
  if (!collision_streaming || m_all_static_surfaces.empty())
    return;

  // Check if we need to reload: first load, or Mario moved far enough.
  if (m_stream_loaded) {
    float dx = mario_x_sm64 - m_stream_center_x;
    float dz = mario_z_sm64 - m_stream_center_z;
    float dist_sq = dx * dx + dz * dz;
    float thresh = collision_stream_reload_threshold;
    if (dist_sq < thresh * thresh)
      return;  // Mario hasn't moved far enough, skip
  }

  // Build the nearby subset.
  float r = collision_stream_radius;
  float r_sq = r * r;
  std::vector<SM64Surface> nearby;
  nearby.reserve(m_all_static_surfaces.size() / 4);  // rough estimate

  for (size_t i = 0; i < m_all_static_surfaces.size(); i++) {
    float dx = m_surface_centroids[i].x - mario_x_sm64;
    float dz = m_surface_centroids[i].z - mario_z_sm64;
    if (dx * dx + dz * dz <= r_sq) {
      nearby.push_back(m_all_static_surfaces[i]);
    }
  }

  if (nearby.empty()) {
    // No surfaces nearby — still load an empty set to clear stale data.
    // The safety floor will keep Mario from crashing.
    sm64_static_surfaces_load(nullptr, 0);
  } else {
    sm64_static_surfaces_load(nearby.data(), static_cast<uint32_t>(nearby.size()));
  }

  m_stream_center_x = mario_x_sm64;
  m_stream_center_z = mario_z_sm64;
  m_stream_loaded = true;
  m_stream_loaded_count = static_cast<int>(nearby.size());
  m_loaded_surface_count = m_stream_loaded_count;

  lg::info("[libsm64] Streaming collision: loaded {} / {} surfaces near ({:.0f}, {:.0f})",
           nearby.size(), m_all_static_surfaces.size(), mario_x_sm64, mario_z_sm64);
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

void LibSM64Manager::update_tar_floor(float mario_x_sm64, float mario_z_sm64) {
  if (!m_initialized || m_mario_id < 0) return;
  if (!m_in_tar_volume) {
    // Left tar — destroy the floor object if one exists.
    if (m_tar_floor_created) {
      sm64_surface_object_delete(m_tar_floor_id);
      m_tar_floor_id = 0;
      m_tar_floor_created = false;
    }
    return;
  }
  // Large quad anchored at Mario's entry XZ, never moved after creation.
  // Moving a surface object each tick gives it a derived platform velocity
  // in SM64's engine which gets applied to Mario (since he's standing on it),
  // causing runaway acceleration.  By creating it once and leaving it still,
  // there is no platform velocity.  4000 SM64u half-extent ≈ 80 Jak metres per
  // side — larger than any tar pool in the game, so Mario can't walk off the edge.
  if (m_tar_floor_created) return;  // already exists, don't touch it
  constexpr int32_t kTarHalfExtent = 4000;
  SM64Surface surfaces[2];
  std::memset(surfaces, 0, sizeof(surfaces));
  // CW from above → +Y normal (same winding as the safety floor).
  surfaces[0].type = 0x0000;
  surfaces[0].force = 0;
  surfaces[0].terrain = 0x0000;  // TERRAIN_GRASS — normal friction
  surfaces[0].vertices[0][0] = -kTarHalfExtent;
  surfaces[0].vertices[0][1] = 0;
  surfaces[0].vertices[0][2] = -kTarHalfExtent;
  surfaces[0].vertices[1][0] = -kTarHalfExtent;
  surfaces[0].vertices[1][1] = 0;
  surfaces[0].vertices[1][2] =  kTarHalfExtent;
  surfaces[0].vertices[2][0] =  kTarHalfExtent;
  surfaces[0].vertices[2][1] = 0;
  surfaces[0].vertices[2][2] = -kTarHalfExtent;
  surfaces[1].type = 0x0000;
  surfaces[1].force = 0;
  surfaces[1].terrain = 0x0000;
  surfaces[1].vertices[0][0] =  kTarHalfExtent;
  surfaces[1].vertices[0][1] = 0;
  surfaces[1].vertices[0][2] = -kTarHalfExtent;
  surfaces[1].vertices[1][0] = -kTarHalfExtent;
  surfaces[1].vertices[1][1] = 0;
  surfaces[1].vertices[1][2] =  kTarHalfExtent;
  surfaces[1].vertices[2][0] =  kTarHalfExtent;
  surfaces[1].vertices[2][1] = 0;
  surfaces[1].vertices[2][2] =  kTarHalfExtent;
  SM64SurfaceObject obj{};
  obj.transform.position[0] = mario_x_sm64;
  obj.transform.position[1] = m_tar_floor_y_sm64;
  obj.transform.position[2] = mario_z_sm64;
  obj.transform.eulerRotation[0] = 0.0f;
  obj.transform.eulerRotation[1] = 0.0f;
  obj.transform.eulerRotation[2] = 0.0f;
  obj.surfaceCount = 2;
  obj.surfaces = surfaces;
  m_tar_floor_id = sm64_surface_object_create(&obj);
  m_tar_floor_created = true;
}

void LibSM64Manager::clear_tar_floor() {
  if (!m_tar_floor_created) return;
  sm64_surface_object_delete(m_tar_floor_id);
  m_tar_floor_id = 0;
  m_tar_floor_created = false;
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
  // **PAT layout differs between jak 1 and jak 2.**  Jak 2 inserts 4 extra
  // flag bits (nogrind / nojak / noboard / nopilot) into the low byte,
  // shifting mode / material / event up by 4 bits.  See goal_src/jak{1,2}/
  // engine/collide/pat-h.gc for the canonical defs.
  //
  //                              jak1            jak2
  //   skip / flags low byte      bits 0-2 (3)    bits 0-6 (7)
  //   mode                       bits 3-5  (3)   bits 7-9   (3)
  //   material                   bits 6-11 (6)   bits 10-15 (6)
  //   nolineofsight / camera     bit 12          bit 16
  //   event                      bits 14-19 (6)  bits 18-23 (6)
  //
  // noentity at bit 0 is the same on both.
  //
  // Relevant pat-material values for hot surfaces:
  //   11 = hotcoals   (fire canyon warm rock, lavatube ledges)
  //   12 = lava       (actual magma in lavatube / firecanyon / citadel)
  //
  // Relevant pat-event values (pat-event enum in pat-h.gc):
  //   0 = none         (normal collision)
  //   1 = deadly
  //   2 = endlessfall  ← invisible "you fell off the map" plane
  //   3 = burn
  //   4 = deadlyup
  //   5 = burnup
  //   6 = melt
  //
  // The per-triangle PAT lives on every CollisionMesh::Vertex (all 3 verts
  // of a tri share the same value) — we check vertex 0 per tri.
  constexpr uint32_t PAT_NOENTITY_BIT = 0x1;
  constexpr uint32_t PAT_MATERIAL_MASK = 0x3F;
  constexpr uint32_t PAT_MAT_HOTCOALS = 11;
  constexpr uint32_t PAT_MAT_LAVA = 12;
  constexpr uint32_t PAT_EVENT_MASK = 0x3F;
  constexpr uint32_t PAT_EVT_ENDLESSFALL = 2;
  constexpr uint32_t PAT_MODE_MASK = 0x7;
  constexpr uint32_t PAT_MODE_GROUND = 0;
  constexpr uint32_t PAT_MODE_WALL = 1;
  constexpr uint32_t PAT_MODE_OBSTACLE = 2;
  // Per-version shifts.  This is the actual jak1 → jak2 fix — feeding the
  // wrong shifts on jak 2 makes the event field read junk bits, which
  // marks random tris as endlessfall and Mario falls through them.
  uint32_t PAT_MODE_SHIFT;
  uint32_t PAT_MATERIAL_SHIFT;
  uint32_t PAT_EVENT_SHIFT;
  if (g_game_version == GameVersion::Jak2) {
    PAT_MODE_SHIFT = 7;
    PAT_MATERIAL_SHIFT = 10;
    PAT_EVENT_SHIFT = 18;
  } else {
    PAT_MODE_SHIFT = 3;
    PAT_MATERIAL_SHIFT = 6;
    PAT_EVENT_SHIFT = 14;
  }

  // SM64 surface type that triggers the classic "burn your butt" launch —
  // when Mario touches a floor with this type, his butt catches fire and
  // SM64 bumps him into ACT_BURNING_JUMP / ACT_BURNING_FALL. See
  // third-party/libsm64/src/decomp/include/surface_terrains.h:6.
  constexpr int16_t SURFACE_BURNING_TYPE = 0x0001;

  // "No slippery Mario" mode — when `g_no_slippery_mario` is on, we
  // classify each tri by Jak pat-mode (wall / ground / obstacle) and
  // map to SM64 slipperiness surface types so the vanilla SM64 slope
  // thresholds match Jak's geometry better.  These constants are unused
  // when the toggle is off.
  constexpr int16_t SURFACE_VERY_SLIPPERY = 0x0013;
  constexpr int16_t SURFACE_SLIPPERY      = 0x0014;
  constexpr int16_t SURFACE_NOT_SLIPPERY  = 0x0015;
  constexpr int16_t TERRAIN_STONE_TYPE    = 0x0001;  // default terrain
  constexpr int16_t TERRAIN_SLIDE_TYPE    = 0x0006;  // slick (used on walls)
  // PAT_MODE_SHIFT, PAT_MODE_MASK, PAT_MODE_GROUND/WALL/OBSTACLE are
  // declared above next to PAT_MATERIAL_SHIFT etc — version-aware on
  // jak 2.  Don't redeclare here.

  size_t num_tris = vertices.size() / 3;
  std::vector<SM64Surface> surfaces;
  surfaces.reserve(num_tris);
  size_t skipped_noentity = 0;
  size_t burning_tris = 0;
  size_t skipped_degenerate = 0;
  size_t skipped_endlessfall = 0;  // pat-event=endlessfall planes — Mario falls through instead of standing on them
  size_t extruded_wall_tris = 0;   // steep pat-mode=WALL tris replaced by vertical quads (counts source tris)

  // libsm64's wall/floor classification threshold (surface_collision.c:104/180).
  // Anything above this counts as a floor; at-or-below counts as a wall.
  constexpr float kLibsm64WallNyCutoff = 0.01f;

  // Push one SM64Surface built from three Jak-space positions.  Used by
  // the single-tri emit at the bottom of the loop AND by the wall-
  // extrusion path that emits two replacement tris per source wall.
  auto push_jak_tri = [&](const float p[3][3], int16_t type, uint16_t terrain) {
    SM64Surface s;
    s.type = type;
    s.force = 0;
    s.terrain = terrain;
    for (int v = 0; v < 3; v++) {
      s.vertices[v][0] = static_cast<int32_t>(p[v][0] * JAK_TO_SM64_SCALE);
      s.vertices[v][1] = static_cast<int32_t>(p[v][1] * JAK_TO_SM64_SCALE);
      s.vertices[v][2] = static_cast<int32_t>(p[v][2] * JAK_TO_SM64_SCALE);
    }
    surfaces.push_back(s);
  };

  for (size_t i = 0; i < num_tris; i++) {
    const auto& v0 = vertices[i * 3 + 0];
    if (v0.pat & PAT_NOENTITY_BIT) {
      // Camera-only collision — Mario ignores it, same as Jak does.
      skipped_noentity++;
      continue;
    }

    // pat-event=endlessfall is the "you fell off the map" plane Jak uses
    // to teleport the player back to the last continue point when they
    // fall below the world.  In Jak's world these tris aren't walked on
    // — the collide-handler fires the endlessfall event on touch — but
    // libsm64 has no such event machinery, so any tri tagged endlessfall
    // just becomes a regular floor Mario can stand on (visibly in the
    // air, over the void).  Drop them from the SM64 collision set so
    // find_floor never returns one and Mario falls through as intended.
    const uint32_t pat_event = (v0.pat >> PAT_EVENT_SHIFT) & PAT_EVENT_MASK;
    if (pat_event == PAT_EVT_ENDLESSFALL) {
      skipped_endlessfall++;
      continue;
    }

    SM64Surface surf;
    const uint32_t material = (v0.pat >> PAT_MATERIAL_SHIFT) & PAT_MATERIAL_MASK;
    const uint32_t pat_mode = (v0.pat >> PAT_MODE_SHIFT) & PAT_MODE_MASK;

    // Pull the three Jak vertex positions once so both the normal test
    // and the SM64-unit emit below can share them.
    const auto& jv0 = vertices[i * 3 + 0];
    const auto& jv1 = vertices[i * 3 + 1];
    const auto& jv2 = vertices[i * 3 + 2];

    // Compute the triangle normal (Jak units — only the SIGN and the
    // normalised |ny| matter here, so no scale conversion needed).
    // This is the same normal libsm64 will compute on its side, since
    // integer truncation to SM64 units preserves the sign of ny for
    // anything non-degenerate.
    float ny_abs = 0.0f;
    bool is_degenerate = false;
    {
      float e1x = jv1.x - jv0.x, e1y = jv1.y - jv0.y, e1z = jv1.z - jv0.z;
      float e2x = jv2.x - jv0.x, e2y = jv2.y - jv0.y, e2z = jv2.z - jv0.z;
      float nx = e1y * e2z - e1z * e2y;
      float ny = e1z * e2x - e1x * e2z;
      float nz = e1x * e2y - e1y * e2x;
      float l = std::sqrt(nx * nx + ny * ny + nz * nz);
      if (l <= 1e-3f) {
        is_degenerate = true;
      } else {
        ny_abs = std::abs(ny) / l;
      }
    }

    // New path: filter degenerate tris AND classify by geometry as well
    // as pat-mode.  Without this, Jak-labelled walls that aren't actually
    // vertical (~95 % of them) get tagged SURFACE_VERY_SLIPPERY but
    // libsm64 sees them as steep floors, snapping Mario into the slide
    // action whenever he touches one.
    if (test_new_collide_toggle && is_degenerate) {
      skipped_degenerate++;
      continue;
    }

    const bool is_hot = (material == PAT_MAT_HOTCOALS || material == PAT_MAT_LAVA);

    // ---- Emit the source tri with legacy classification ----------------
    // The tagging here matches the pre-test_new_collide_toggle behaviour
    // exactly — same VERY_SLIPPERY/NOT_SLIPPERY/SLIPPERY mapping driven by
    // Jak pat-mode, same TERRAIN_SLIDE on walls.  This preserves Mario's
    // existing "slide on a steep wall's surface" behaviour, since
    // libsm64's find_floor will still pick this tri up when Mario's XZ
    // lands on it.
    if (is_hot) {
      surf.type = SURFACE_BURNING_TYPE;
      burning_tris++;
    } else if (g_no_slippery_mario) {
      switch (pat_mode) {
        case PAT_MODE_WALL:     surf.type = SURFACE_VERY_SLIPPERY; break;
        case PAT_MODE_GROUND:   surf.type = SURFACE_NOT_SLIPPERY;  break;
        case PAT_MODE_OBSTACLE: surf.type = SURFACE_SLIPPERY;      break;
        default:                surf.type = 0x0000;                break;  // SURFACE_DEFAULT
      }
    } else {
      surf.type = 0x0000;    // SURFACE_DEFAULT
    }
    surf.force = 0;
    surf.terrain = (g_no_slippery_mario && pat_mode == PAT_MODE_WALL)
                       ? TERRAIN_SLIDE_TYPE
                       : TERRAIN_STONE_TYPE;

    for (int v = 0; v < 3; v++) {
      const auto& vert = vertices[i * 3 + v];
      // Jak positions are in meters, SM64 expects its own units (43x scale)
      surf.vertices[v][0] = static_cast<int32_t>(vert.x * JAK_TO_SM64_SCALE);
      surf.vertices[v][1] = static_cast<int32_t>(vert.y * JAK_TO_SM64_SCALE);
      surf.vertices[v][2] = static_cast<int32_t>(vert.z * JAK_TO_SM64_SCALE);
    }
    surfaces.push_back(surf);

    // ---- Steep-wall extrusion (ADDITIONAL, only with test_new_collide_toggle)
    // pat-mode=WALL tris whose normal isn't vertical enough for libsm64's
    // find_wall_collisions (|ny|>0.01) are invisible to wall queries, so
    // Mario tunnels through them at speed — each sub-step's XZ jumps
    // past the footprint entirely and there's no wall hit.  To fix this
    // *without* losing the legacy slide-on-surface behaviour, we ALSO
    // emit a pair of perfectly-vertical triangles forming a wall quad
    // over the source tri's longest XZ edge and Y range.  The quads have
    // ny=0 exactly, so find_wall_collisions picks them up; the original
    // tilted tri above still feeds find_floor, so Mario slides on it
    // when his XZ lands there.  Winding is chosen so the quad's outward
    // normal matches the source tri's outward XZ direction.
    //
    // The `|ny| <= wall_extrusion_ny_max` upper bound gates this off for
    // anything too close to a slope — extruding a 30-degree ramp into a
    // vertical quad would create a tall false wall.  Default cap of 0.30
    // (~72° slope) is tunable via the ImGui slider.
    if (test_new_collide_toggle && g_no_slippery_mario && !is_hot &&
        pat_mode == PAT_MODE_WALL &&
        ny_abs > kLibsm64WallNyCutoff && ny_abs <= wall_extrusion_ny_max) {
      float d01sq = (jv1.x - jv0.x) * (jv1.x - jv0.x) + (jv1.z - jv0.z) * (jv1.z - jv0.z);
      float d02sq = (jv2.x - jv0.x) * (jv2.x - jv0.x) + (jv2.z - jv0.z) * (jv2.z - jv0.z);
      float d12sq = (jv2.x - jv1.x) * (jv2.x - jv1.x) + (jv2.z - jv1.z) * (jv2.z - jv1.z);
      float ax, az, bx, bz;
      if (d01sq >= d02sq && d01sq >= d12sq) {
        ax = jv0.x; az = jv0.z; bx = jv1.x; bz = jv1.z;
      } else if (d02sq >= d12sq) {
        ax = jv0.x; az = jv0.z; bx = jv2.x; bz = jv2.z;
      } else {
        ax = jv1.x; az = jv1.z; bx = jv2.x; bz = jv2.z;
      }

      const float y_raw_min = std::min(std::min(jv0.y, jv1.y), jv2.y);
      const float y_raw_max = std::max(std::max(jv0.y, jv1.y), jv2.y);
      const float y_cen = (y_raw_min + y_raw_max) * 0.5f;
      const float half_cap = std::min((y_raw_max - y_raw_min) * 0.5f,
                                       wall_extrusion_height_cap * 0.5f);
      const float y_min = y_cen - half_cap;
      const float y_max = y_cen + half_cap;

      float e1x = jv1.x - jv0.x, e1y = jv1.y - jv0.y, e1z = jv1.z - jv0.z;
      float e2x = jv2.x - jv0.x, e2y = jv2.y - jv0.y, e2z = jv2.z - jv0.z;
      float src_nx = e1y * e2z - e1z * e2y;
      float src_nz = e1x * e2y - e1y * e2x;
      float cand_nx = -(bz - az);
      float cand_nz = (bx - ax);
      const bool flip = (src_nx * cand_nx + src_nz * cand_nz) < 0.0f;

      // Vertical wall quad gets the same VERY_SLIPPERY + SLIDE tagging
      // the source WALL tri already had.  Since the quad is geometrically
      // vertical, libsm64 only queries it via find_wall_collisions, so
      // the tag affects glance-off physics but not standing.
      constexpr int16_t wall_type = SURFACE_VERY_SLIPPERY;
      constexpr uint16_t wall_terrain = TERRAIN_SLIDE_TYPE;

      float A[3] = {ax, y_min, az};
      float B[3] = {bx, y_min, bz};
      float C[3] = {bx, y_max, bz};
      float D[3] = {ax, y_max, az};

      if (!flip) {
        float tri1[3][3] = {{A[0], A[1], A[2]}, {B[0], B[1], B[2]}, {C[0], C[1], C[2]}};
        float tri2[3][3] = {{A[0], A[1], A[2]}, {C[0], C[1], C[2]}, {D[0], D[1], D[2]}};
        push_jak_tri(tri1, wall_type, wall_terrain);
        push_jak_tri(tri2, wall_type, wall_terrain);
      } else {
        float tri1[3][3] = {{A[0], A[1], A[2]}, {C[0], C[1], C[2]}, {B[0], B[1], B[2]}};
        float tri2[3][3] = {{A[0], A[1], A[2]}, {D[0], D[1], D[2]}, {C[0], C[1], C[2]}};
        push_jak_tri(tri1, wall_type, wall_terrain);
        push_jak_tri(tri2, wall_type, wall_terrain);
      }
      extruded_wall_tris++;
    }
  }

  if (surfaces.empty()) {
    lg::warn("[libsm64] All {} level triangles were noentity — nothing to load", num_tris);
    m_all_static_surfaces.clear();
    m_surface_centroids.clear();
    return;
  }

  // Store all surfaces and pre-compute XZ centroids for streaming.
  m_all_static_surfaces = std::move(surfaces);
  m_surface_centroids.resize(m_all_static_surfaces.size());
  for (size_t i = 0; i < m_all_static_surfaces.size(); i++) {
    auto& s = m_all_static_surfaces[i];
    m_surface_centroids[i].x = (s.vertices[0][0] + s.vertices[1][0] + s.vertices[2][0]) / 3.0f;
    m_surface_centroids[i].z = (s.vertices[0][2] + s.vertices[1][2] + s.vertices[2][2]) / 3.0f;
  }

  // Reset streaming state so the next tick does an immediate reload around Mario.
  m_stream_loaded = false;
  m_stream_loaded_count = 0;
  // Bump version so SM64CollisionRenderer / the dump button see the new set.
  m_static_surfaces_version++;

  if (!collision_streaming) {
    // Streaming off — load everything at once (old behavior).
    std::scoped_lock lock(m_sm64_lock);
    sm64_static_surfaces_load(m_all_static_surfaces.data(),
                              static_cast<uint32_t>(m_all_static_surfaces.size()));
    m_loaded_surface_count = static_cast<int>(m_all_static_surfaces.size());
    m_stream_loaded = true;
    m_stream_loaded_count = m_loaded_surface_count;
  }

  lg::info(
      "[libsm64] Stored {} collision surfaces from level geometry ({} noentity skipped, {} endlessfall skipped, {} degenerate skipped, {} burning, {} source tris extruded into vertical wall quads, streaming={}, new_classify={})",
      m_all_static_surfaces.size(), skipped_noentity, skipped_endlessfall, skipped_degenerate, burning_tris,
      extruded_wall_tris, collision_streaming, test_new_collide_toggle);
}

std::vector<CollisionTriSnapshot> LibSM64Manager::snapshot_static_surfaces() {
  // Hold m_sm64_lock while copying — load_level_collision takes the same
  // lock while swapping m_all_static_surfaces, so this is the cheap way
  // to avoid tearing.  The copy is linear in tri count; expected sizes
  // are ~20–100k in a loaded level, which is still microseconds.
  std::vector<CollisionTriSnapshot> out;
  std::scoped_lock lock(m_sm64_lock);
  out.reserve(m_all_static_surfaces.size());
  for (const auto& s : m_all_static_surfaces) {
    CollisionTriSnapshot t;
    for (int v = 0; v < 3; v++) {
      t.verts[v][0] = static_cast<float>(s.vertices[v][0]);
      t.verts[v][1] = static_cast<float>(s.vertices[v][1]);
      t.verts[v][2] = static_cast<float>(s.vertices[v][2]);
    }
    t.type = s.type;
    t.force = s.force;
    t.terrain = s.terrain;
    out.push_back(t);
  }
  return out;
}

bool LibSM64Manager::write_mario_pos_to_target(u8* ee_mem,
                                                u32 ee_mem_size,
                                                u32 false_val,
                                                u32 target_ptr,
                                                const math::Vector3f& mario_pos) {
  if (!ee_mem || target_ptr == 0 || target_ptr == false_val) return false;

  // process-drawable.root: jak1 runtime 108, jak2 runtime 120 (jak2 process
  // has +12 extra fields).  See sm64_target_offsets() comment block for the
  // full layout breakdown.
  const auto offs = sm64_target_offsets();
  const u32 ROOT_RUNTIME_OFF = offs.process_drawable_root;
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

  // trsqv.trans: 16-byte vector (x,y,z,w floats).  Engine struct, same
  // runtime offset (12) on both jak 1 and jak 2.
  const u32 TRANS_RUNTIME_OFF = offs.trsqv_trans;
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

  u32 target_sym_off = sm64_get_symbol_offset("*target*");
  if (target_sym_off != 0) {
    m_cached_target_sym_offset = target_sym_off;
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
  u32 target_ptr = sm64_get_symbol_value("*target*");
  if (target_ptr == 0 || target_ptr == false_val) return;

  auto mario_pos = get_state().position;
  write_mario_pos_to_target(ee_mem, EE_MAIN_MEM_SIZE, false_val, target_ptr, mario_pos);
}

// ---------------------------------------------------------------------------
// Write Mario state to GOAL-side bridge vectors so the sm64-mario-col process
// (defined in mario.gc) can track Mario and spawn touch-tracker attacks.
// ---------------------------------------------------------------------------
void LibSM64Manager::write_mario_bridge_data(u8* ee_mem) {
  if (!m_initialized || m_mario_id < 0 || !ee_mem) return;

  u32 false_val = s7.offset;
  if (false_val == 0) return;

  auto state = get_state();

  // ---- *sm64-mario-pos*: vector with x,y,z = position, w = face angle (GOAL degrees) ----
  {
    u32 vec_ptr = sm64_get_symbol_value("*sm64-mario-pos*");
    if (vec_ptr != 0 && vec_ptr != false_val && vec_ptr + 16 <= EE_MAIN_MEM_SIZE) {
      // Convert radians → GOAL angle units (65536 = full revolution).
      float goal_angle = state.face_angle * (65536.0f / (2.0f * 3.14159265358979f));
      float data[4] = {state.position.x(), state.position.y(), state.position.z(), goal_angle};
      std::memcpy(ee_mem + vec_ptr, data, 16);
    }
  }

  // ---- *sm64-mario-info*: x = attacking (1.0 / 0.0) ----
  {
    u32 info_ptr = sm64_get_symbol_value("*sm64-mario-info*");
    if (info_ptr != 0 && info_ptr != false_val && info_ptr + 16 <= EE_MAIN_MEM_SIZE) {
      // ACT_FLAG_ATTACKING = (1 << 23) is set on all punch/kick/dive/ground-pound actions.
      constexpr uint32_t ACT_FLAG_ATTACKING = 0x00800000;
      constexpr uint32_t ACT_FLAG_DIVING = 0x00080000;
      constexpr uint32_t ACT_GROUND_POUND_LAND = 0x0080023C;
      constexpr uint32_t ACT_GROUND_POUND = 0x008008A9;
      constexpr uint32_t ACT_SLIDE_KICK = 0x018008AA;
      constexpr uint32_t ACT_SLIDE_KICK_SLIDE = 0x0080045A;
      constexpr uint32_t ACT_JUMP_KICK = 0x018008AC;
      constexpr uint32_t ACT_BUTT_SLIDE = 0x00840452;
      constexpr uint32_t ACT_BUTT_SLIDE_STOP = 0x00840453;
      constexpr uint32_t ACT_BUTT_SLIDE_AIR = 0x0300088E;
      bool is_diving = (state.action & ACT_FLAG_DIVING) != 0;
      bool is_gp = (state.action == ACT_GROUND_POUND) || (state.action == ACT_GROUND_POUND_LAND);
      bool is_slide_kick = (state.action == ACT_SLIDE_KICK) || (state.action == ACT_SLIDE_KICK_SLIDE);
      bool is_jump_kick = (state.action == ACT_JUMP_KICK);
      bool is_butt_slide = (state.action == ACT_BUTT_SLIDE) || (state.action == ACT_BUTT_SLIDE_STOP) || (state.action == ACT_BUTT_SLIDE_AIR);
      // x = punching/kicking (not dive/gp/slide-kick/jump-kick/butt-slide), y = ground pound impact, z = ground pound falling, w = diving
      constexpr uint32_t MARIO_PUNCHING = 0x00100000;
      constexpr uint32_t MARIO_KICKING  = 0x00200000;
      constexpr uint32_t MARIO_TRIPPING = 0x00400000;  // set during crouch-kick (breakdance) hit frames
      bool is_punch_action = (state.action & ACT_FLAG_ATTACKING) != 0 && !is_diving && !is_gp && !is_slide_kick && !is_jump_kick && !is_butt_slide;
      bool hit_flag_active = (state.flags & (MARIO_PUNCHING | MARIO_KICKING | MARIO_TRIPPING)) != 0;
      static bool s_prev_hit_flag = false;
      bool is_attacking = is_punch_action && hit_flag_active && !s_prev_hit_flag;
      s_prev_hit_flag = is_punch_action && hit_flag_active;
      bool gp_impact = (state.action == ACT_GROUND_POUND_LAND);
      bool gp_falling = (state.action == ACT_GROUND_POUND);
      float info_data[4] = {is_attacking ? 1.0f : 0.0f,
                            gp_impact ? 1.0f : 0.0f,
                            gp_falling ? 1.0f : 0.0f,
                            is_diving ? 1.0f : 0.0f};
      std::memcpy(ee_mem + info_ptr, info_data, 16);
    }
  }

  // ---- *sm64-mario-health*: x = wedge count (0.0–8.0, integer steps) ----
  {
    u32 health_ptr = sm64_get_symbol_value("*sm64-mario-health*");
    if (health_ptr != 0 && health_ptr != false_val && health_ptr + 16 <= EE_MAIN_MEM_SIZE) {
      // SM64 health: 0xHHSS — high byte low nibble = wedge count.
      float wedges = static_cast<float>((static_cast<uint16_t>(state.health) >> 8) & 0xF);
      float health_data[4] = {wedges, 0.0f, 0.0f, 0.0f};
      std::memcpy(ee_mem + health_ptr, health_data, 16);
    }
  }

  // ---- *sm64-mario-damage*: damage-state info for the health sync ----
  // x = mode (0=none, 1=lava, 2=drown, 3=endlessfall, 4=generic) — derived
  //     from Mario's current libsm64 action so GOAL knows what kind of
  //     damage to propagate to Jak when Mario's wedges drop.
  // y = invincibility (1.0 if libsm64's invincTimer is running)
  // z = air supply (m->health, 0..2176) for the underwater air HUD
  // w = submerged (1.0 if Mario is in a submerged action)
  {
    u32 dmg_ptr = sm64_get_symbol_value("*sm64-mario-damage*");
    if (dmg_ptr != 0 && dmg_ptr != false_val && dmg_ptr + 16 <= EE_MAIN_MEM_SIZE) {
      constexpr uint32_t kActLavaBoost          = 0x010208B7;
      constexpr uint32_t kActDrowning           = 0x300032C4;
      constexpr uint32_t kActForwardAirKB       = 0x010208B1;
      constexpr uint32_t kActBackwardAirKB      = 0x010208B0;
      constexpr uint32_t kActSoftBackwardGroundKB = 0x00020464;
      constexpr uint32_t kActQuicksandDeath     = 0x00021312;
      // ACT_FLAG_SWIMMING is set on every fully-submerged action.  But
      // user can also be "at the water" while in ACT_WATER_JUMP — the
      // breach-out-of-water arc — which is technically airborne (no
      // SWIMMING flag) yet shouldn't trigger an airborne knockback
      // animation either.  Catch both: SWIMMING flag OR water-jump action.
      constexpr uint32_t ACT_FLAG_SWIMMING_F    = 0x00002000;
      constexpr uint32_t kActWaterJump_W        = 0x01000889;
      constexpr uint32_t kActHoldWaterJump_W    = 0x010008A3;
      float mode = 0.0f;
      switch (state.action) {
        case kActLavaBoost:               mode = 1.0f; break;
        case kActDrowning:                mode = 2.0f; break;
        case kActForwardAirKB:
        case kActBackwardAirKB:           mode = 3.0f; break;  // OOB-style flying KB
        case kActSoftBackwardGroundKB:
        case kActQuicksandDeath:          mode = 4.0f; break;  // generic dying
        default:                          mode = 0.0f; break;
      }
      // Submerged: SWIMMING flag set, OR Mario is in a water-jump arc.
      bool submerged = (state.action & ACT_FLAG_SWIMMING_F) != 0
                       || state.action == kActWaterJump_W
                       || state.action == kActHoldWaterJump_W;
      // DEBUG: throttled print to verify the swim detection.  Remove once
      // the GOAL-side knockback gate is confirmed working.
      {
        static uint32_t s_last_logged_action = 0xFFFFFFFFu;
        static bool s_last_logged_submerged = false;
        if (state.action != s_last_logged_action || submerged != s_last_logged_submerged) {
          lg::info("[sm64-debug] action=0x{:08X} submerged={}", state.action, submerged);
          s_last_logged_action = state.action;
          s_last_logged_submerged = submerged;
        }
      }
      // Invincibility: read from gMarioState — exposed via state if we add it,
      // but for now infer from action (knockback actions all have ACT_FLAG_
      // INVULNERABLE = 0x02000000 set).
      constexpr uint32_t ACT_FLAG_INVULNERABLE = 0x02000000;
      bool invinc = (state.action & ACT_FLAG_INVULNERABLE) != 0;
      float dmg_data[4] = {mode,
                           invinc ? 1.0f : 0.0f,
                           static_cast<float>(state.health),
                           submerged ? 1.0f : 0.0f};
      std::memcpy(ee_mem + dmg_ptr, dmg_data, 16);
    }
  }

  // ---- *sm64-mario-on-shell*: x = 1.0 if riding shell, 0.0 otherwise
  //                              y = 1.0 if shell-protect window is active
  //                                  (m_clone_anim_snapshot_valid: Mario is on
  //                                   shell OR is in a cell-pickup cutscene
  //                                   that started while on shell) ----
  {
    u32 shell_ptr = sm64_get_symbol_value("*sm64-mario-on-shell*");
    if (shell_ptr != 0 && shell_ptr != false_val && shell_ptr + 16 <= EE_MAIN_MEM_SIZE) {
      constexpr uint32_t ACT_FLAG_RIDING_SHELL = 0x00010000;
      bool on_shell = (state.action & ACT_FLAG_RIDING_SHELL) != 0;
      float shell_data[4] = {on_shell ? 1.0f : 0.0f,
                             m_clone_anim_snapshot_valid ? 1.0f : 0.0f,
                             0.0f, 0.0f};
      std::memcpy(ee_mem + shell_ptr, shell_data, 16);
    }
  }

  // ---- *sm64-mario-velocity*: x = forward velocity, y = slide-kick, z = jump-kick, w = butt-slide ----
  {
    u32 vel_ptr = sm64_get_symbol_value("*sm64-mario-velocity*");
    if (vel_ptr != 0 && vel_ptr != false_val && vel_ptr + 16 <= EE_MAIN_MEM_SIZE) {
      // Reuse the action booleans computed above in the info block.
      constexpr uint32_t ACT_SLIDE_KICK_V = 0x018008AA;
      constexpr uint32_t ACT_SLIDE_KICK_SLIDE_V = 0x0080045A;
      constexpr uint32_t ACT_JUMP_KICK_V = 0x018008AC;
      constexpr uint32_t ACT_BUTT_SLIDE_V = 0x00840452;
      constexpr uint32_t ACT_BUTT_SLIDE_STOP_V = 0x00840453;
      constexpr uint32_t ACT_BUTT_SLIDE_AIR_V = 0x0300088E;
      bool vel_slide_kick = (state.action == ACT_SLIDE_KICK_V) || (state.action == ACT_SLIDE_KICK_SLIDE_V);
      bool vel_jump_kick = (state.action == ACT_JUMP_KICK_V);
      bool vel_butt_slide = (state.action == ACT_BUTT_SLIDE_V) || (state.action == ACT_BUTT_SLIDE_STOP_V) || (state.action == ACT_BUTT_SLIDE_AIR_V);
      float vel_data[4] = {state.forward_velocity,
                           vel_slide_kick ? 1.0f : 0.0f,
                           vel_jump_kick ? 1.0f : 0.0f,
                           vel_butt_slide ? 1.0f : 0.0f};
      std::memcpy(ee_mem + vel_ptr, vel_data, 16);
    }
  }

  // ---- *sm64-mario-hit*: read x, if > 0.5 Mario was struck by Jak, clear it ----
  {
    u32 hit_ptr = sm64_get_symbol_value("*sm64-mario-hit*");
    if (hit_ptr != 0 && hit_ptr != false_val && hit_ptr + 16 <= EE_MAIN_MEM_SIZE) {
      float hit_flag;
      std::memcpy(&hit_flag, ee_mem + hit_ptr, 4);
      if (hit_flag > 0.5f) {
        lg::info("[libsm64] Mario was struck by Jak!");
        // Clear the flag.
        float zero = 0.0f;
        std::memcpy(ee_mem + hit_ptr, &zero, 4);
        // Damage is now handled directly from GOAL via pc-sm64-damage-mario.
      }
    }
  }
}

// ---------------------------------------------------------------------------
// GOAL-callable damage: registered as "pc-sm64-damage-mario".
// ---------------------------------------------------------------------------
void LibSM64Manager::damage_mario_from_goal() {
  if (!m_initialized || m_mario_id < 0) return;
  auto state = get_state();
  // Impact position slightly in front of Mario so knockback pushes him backward.
  float front_x = state.position.x() + std::sin(state.face_angle) * 100.0f;
  float front_z = state.position.z() + std::cos(state.face_angle) * 100.0f;
  float sm64_x = front_x * JAK_TO_SM64_SCALE;
  float sm64_y = state.position.y() * JAK_TO_SM64_SCALE;
  float sm64_z = front_z * JAK_TO_SM64_SCALE;
  {
    std::scoped_lock lock(m_sm64_lock);
    sm64_mario_take_damage(m_mario_id, 2, 0, sm64_x, sm64_y, sm64_z);
  }
}

u64 pc_sm64_damage_mario() {
  LibSM64Manager::instance().damage_mario_from_goal();
  return 0;
}

void LibSM64Manager::heal_mario_from_goal() {
  if (!m_initialized || m_mario_id < 0) return;
  auto state = get_state();
  uint16_t current = static_cast<uint16_t>(state.health);
  uint16_t new_health = std::min<uint16_t>(current + 0x0200, 0x0880);
  {
    std::scoped_lock lock(m_sm64_lock);
    sm64_set_mario_health(m_mario_id, new_health);
  }
}

void LibSM64Manager::full_heal_mario_from_goal() {
  if (!m_initialized || m_mario_id < 0) return;
  {
    std::scoped_lock lock(m_sm64_lock);
    sm64_set_mario_health(m_mario_id, 0x0880);
  }
}

u64 pc_sm64_heal_mario() {
  LibSM64Manager::instance().heal_mario_from_goal();
  return 0;
}

u64 pc_sm64_full_heal_mario() {
  LibSM64Manager::instance().full_heal_mario_from_goal();
  return 0;
}

// ---------------------------------------------------------------------------
// Health sync bridges — see mario.gc "Health synchronization" block for the
// design.  Each routes Mario through a real libsm64 special state so the
// proper SM64 hurt animation + knockback plays.
// ---------------------------------------------------------------------------

void LibSM64Manager::set_mario_wedges_from_goal(int wedges) {
  if (!m_initialized || m_mario_id < 0) return;
  if (wedges > 8) wedges = 8;
  if (wedges < 0) wedges = 0;
  uint16_t health = (wedges == 0) ? 0xFF
                                  : static_cast<uint16_t>((wedges << 8) | 0x80);
  std::scoped_lock lock(m_sm64_lock);
  sm64_set_mario_health(m_mario_id, health);
}

void LibSM64Manager::knockback_mario_from_goal(int wedges) {
  if (!m_initialized || m_mario_id < 0) return;
  auto state = get_state();
  float front_x = state.position.x() + std::sin(state.face_angle) * 100.0f;
  float front_z = state.position.z() + std::cos(state.face_angle) * 100.0f;
  float sm64_x = front_x * JAK_TO_SM64_SCALE;
  float sm64_y = state.position.y() * JAK_TO_SM64_SCALE;
  float sm64_z = front_z * JAK_TO_SM64_SCALE;
  std::scoped_lock lock(m_sm64_lock);
  sm64_mario_take_damage(m_mario_id, static_cast<uint32_t>(wedges), 0,
                         sm64_x, sm64_y, sm64_z);
}

void LibSM64Manager::burn_mario_from_goal(int wedges) {
  if (!m_initialized || m_mario_id < 0) return;
  constexpr uint32_t kActLavaBoost = 0x010208B7;
  constexpr uint32_t kActFlagRidingShell = 0x00010000;
  auto st = get_state();
  // Shell provides lava immunity — match native SM64's check_lava_boost which
  // explicitly skips the burn when ACT_FLAG_RIDING_SHELL is set.
  if (st.action & kActFlagRidingShell) {
    return;
  }
  uint16_t cur = static_cast<uint16_t>(st.health);
  uint16_t cur_wedges = (cur >> 8) & 0xF;
  uint16_t new_wedges = (cur_wedges > static_cast<uint16_t>(wedges))
                            ? (cur_wedges - static_cast<uint16_t>(wedges))
                            : 0;
  uint16_t new_health = (new_wedges == 0) ? 0xFF
                                          : static_cast<uint16_t>((new_wedges << 8) | 0x80);
  std::scoped_lock lock(m_sm64_lock);
  sm64_set_mario_action(m_mario_id, kActLavaBoost);
  sm64_set_mario_health(m_mario_id, new_health);
}

void LibSM64Manager::drown_mario_from_goal() {
  if (!m_initialized || m_mario_id < 0) return;
  constexpr uint32_t kActDrowning = 0x300032C4;
  std::scoped_lock lock(m_sm64_lock);
  sm64_set_mario_action(m_mario_id, kActDrowning);
  sm64_set_mario_health(m_mario_id, 0xFF);
}

void LibSM64Manager::endlessfall_mario_from_goal() {
  if (!m_initialized || m_mario_id < 0) return;
  constexpr uint32_t kActForwardAirKB = 0x010208B1;
  std::scoped_lock lock(m_sm64_lock);
  sm64_set_mario_action(m_mario_id, kActForwardAirKB);
  sm64_set_mario_health(m_mario_id, 0xFF);
}

void LibSM64Manager::kill_mario_from_goal() {
  if (!m_initialized || m_mario_id < 0) return;
  constexpr uint32_t kActSoftBackwardGroundKB = 0x00020464;
  std::scoped_lock lock(m_sm64_lock);
  sm64_set_mario_action(m_mario_id, kActSoftBackwardGroundKB);
  sm64_set_mario_health(m_mario_id, 0xFF);
}

int LibSM64Manager::get_mario_air_from_goal() {
  if (!m_initialized || m_mario_id < 0) return 0;
  // libsm64 uses Mario's health field as the air gauge while submerged
  // (no separate timer — the health number is "air").  GOAL renders this
  // as an air bar when Mario's in a submerged action.  Range 0..2176.
  auto st = get_state();
  return static_cast<int>(static_cast<uint16_t>(st.health));
}

u64 pc_sm64_set_mario_wedges(u32 wedges) {
  LibSM64Manager::instance().set_mario_wedges_from_goal(static_cast<int>(wedges));
  return 0;
}
u64 pc_sm64_knockback_mario(u32 wedges) {
  LibSM64Manager::instance().knockback_mario_from_goal(static_cast<int>(wedges));
  return 0;
}
u64 pc_sm64_burn_mario(u32 wedges) {
  LibSM64Manager::instance().burn_mario_from_goal(static_cast<int>(wedges));
  return 0;
}
u64 pc_sm64_drown_mario() {
  LibSM64Manager::instance().drown_mario_from_goal();
  return 0;
}
u64 pc_sm64_endlessfall_mario() {
  LibSM64Manager::instance().endlessfall_mario_from_goal();
  return 0;
}
u64 pc_sm64_kill_mario() {
  LibSM64Manager::instance().kill_mario_from_goal();
  return 0;
}
u64 pc_sm64_stomp_bounce_mario() { // potentilly look at trampoline bounce instead
  auto& mgr = LibSM64Manager::instance();
  if (!mgr.has_mario()) return 0;
  // Fixed bounce velocity — independent of how fast Mario was falling.
  // sm64_mario_attack's fake_interact_bounce_top adds to the incoming vertical
  // velocity, so stomping from a high jump would produce a much taller bounce
  // than stomping from a low one. We call sm64_mario_attack purely to set the
  // correct action state and animation, then unconditionally override Y velocity
  // so the bounce height is always the same regardless of fall speed.
  float bounce_vel = 20.0f * (g_libsm64_mario_scale / 43.0f);
  auto state = mgr.get_state();
  // Convert Jak-unit position to SM64 units for the fake attack object.
  float mx = state.position.x() * JAK_TO_SM64_SCALE;
  float my = state.position.y() * JAK_TO_SM64_SCALE;
  float mz = state.position.z() * JAK_TO_SM64_SCALE;
  sm64_mario_attack(mgr.get_mario_id(), mx, my - 100.0f, mz, 100.0f);
  // Force ACT_FREEFALL so no action handler overrides vel[1] on the next tick.
  constexpr uint32_t kActFreefall = 0x0100088C;
  sm64_set_mario_action(mgr.get_mario_id(), kActFreefall);
  // Set fixed bounce velocity; pass XZ velocity in SM64 units.
  auto cur = mgr.get_state();
  float cur_vx = cur.velocity.x() * JAK_TO_SM64_SCALE;
  float cur_vz = cur.velocity.z() * JAK_TO_SM64_SCALE;
  sm64_set_mario_velocity(mgr.get_mario_id(), cur_vx, bounce_vel, cur_vz);
  return 0;
}
u64 pc_sm64_hover_mario() {
  auto& mgr = LibSM64Manager::instance();
  if (!mgr.has_mario()) return 0;
  mgr.request_hover();
  return 0;
}

u64 pc_sm64_get_mario_air() {
  return static_cast<u64>(LibSM64Manager::instance().get_mario_air_from_goal());
}

u64 pc_sm64_delete_mario() {
  auto& mgr = LibSM64Manager::instance();
  if (mgr.has_mario()) {
    lg::info("[libsm64] Mario deleted from GOAL (death chain)");
    mgr.delete_mario(mgr.get_mario_id());
  }
  // Flag that a respawn should follow immediately.  Keeps Merc2 hiding
  // eichar-lod0 during the no-Mario window (so save-load doesn't flash
  // Jak's model) and tells auto-spawn to skip the target-not-ready
  // cooldown so Mario comes back the instant *target* is alive.
  mgr.set_respawn_pending(true);
  return 0;
}

// ---------------------------------------------------------------------------
// Mario "corpses" — append-only list of frozen mesh snapshots, one per death.
// Persist across level transitions / save-load.  Renderer rebuilds its GPU
// meshes when corpse_version() changes (capture or clear).
// ---------------------------------------------------------------------------
void LibSM64Manager::capture_mario_corpse() {
  // Copy the latest geometry under the geo lock so we don't tear a half-
  // updated frame.  The geometry is in world space (libsm64 transforms it
  // during sm64_mario_tick), so the corpse renders correctly without any
  // per-frame transform — it just sits exactly where Mario was when this
  // call fired.
  MarioGeometry snapshot;
  {
    std::lock_guard<std::mutex> g(m_geo_mutex);
    snapshot = m_geometry;
  }
  if (snapshot.num_triangles == 0) {
    lg::warn("[libsm64] capture_mario_corpse: live geometry empty, skipping");
    return;
  }
  size_t new_count = 0;
  {
    std::lock_guard<std::mutex> lock(m_corpse_mutex);
    m_corpse_geometries.push_back(std::move(snapshot));
    new_count = m_corpse_geometries.size();
  }
  m_corpse_count.store(new_count, std::memory_order_release);
  // New corpse starts as pending (invisible) — must be finalized before
  // it shows up in the visible_corpse_count the renderer iterates.
  m_last_corpse_pending.store(true, std::memory_order_release);
  m_corpse_version.fetch_add(1, std::memory_order_release);
  lg::info("[libsm64] Captured Mario corpse #{} (pending)", new_count);
}

void LibSM64Manager::finalize_last_mario_corpse() {
  // Promote the pending corpse to visible.  No-op if there isn't one.
  bool was_pending = m_last_corpse_pending.exchange(false, std::memory_order_acq_rel);
  if (was_pending) {
    // Bump version so the renderer's visible_corpse_count reading + GPU
    // mesh list pick up the newly-visible entry on the next frame.
    m_corpse_version.fetch_add(1, std::memory_order_release);
    lg::info("[libsm64] Finalized Mario corpse #{}", m_corpse_count.load());
  }
}

void LibSM64Manager::update_last_mario_corpse() {
  // Replace the last corpse's geometry with Mario's current geo.  Used by
  // the rolling-update during Jak's death animation — see the public
  // header comment for the full lifecycle.  No-op if no corpse exists yet
  // (rising-edge capture failed, level-clear happened mid-death, etc.).
  MarioGeometry snapshot;
  {
    std::lock_guard<std::mutex> g(m_geo_mutex);
    snapshot = m_geometry;
  }
  if (snapshot.num_triangles == 0) {
    return;  // nothing to capture
  }
  bool replaced = false;
  {
    std::lock_guard<std::mutex> lock(m_corpse_mutex);
    if (!m_corpse_geometries.empty()) {
      m_corpse_geometries.back() = std::move(snapshot);
      replaced = true;
    }
  }
  if (replaced) {
    // Bump version so the renderer re-uploads the (now-updated) last
    // corpse's GPU buffers.  Don't change m_corpse_count — the slot
    // count is unchanged.
    m_corpse_version.fetch_add(1, std::memory_order_release);
  }
}

void LibSM64Manager::clear_mario_corpses() {
  {
    std::lock_guard<std::mutex> lock(m_corpse_mutex);
    m_corpse_geometries.clear();
    m_corpse_geometries.shrink_to_fit();
  }
  m_corpse_count.store(0, std::memory_order_release);
  m_last_corpse_pending.store(false, std::memory_order_release);
  m_corpse_version.fetch_add(1, std::memory_order_release);
}

std::vector<MarioGeometry> LibSM64Manager::get_mario_corpses() {
  std::lock_guard<std::mutex> lock(m_corpse_mutex);
  return m_corpse_geometries;  // copy
}

u64 pc_sm64_capture_mario_corpse() {
  LibSM64Manager::instance().capture_mario_corpse();
  return 0;
}

u64 pc_sm64_update_last_mario_corpse() {
  LibSM64Manager::instance().update_last_mario_corpse();
  return 0;
}

u64 pc_sm64_finalize_last_mario_corpse() {
  LibSM64Manager::instance().finalize_last_mario_corpse();
  return 0;
}

u64 pc_sm64_clear_mario_corpse() {
  LibSM64Manager::instance().clear_mario_corpses();
  return 0;
}

// ---------------------------------------------------------------------------
// Mario red-fabric color presets — the shader hue-rotates Mario's red
// vertex color toward the hue of u_tint while preserving sat + value.  Only
// the HUE of these RGBs matters; sat/val from the original red pixel is
// kept.  Preset 0 = vanilla red so "Red" leaves Mario unchanged.
//
// Add new colors here AND in the debug-menu Colors submenu (mario-debug-
// menu.gc).  Indices ARE persisted to disk via mario-settings, so DON'T
// reorder existing entries — only append.
// ---------------------------------------------------------------------------
namespace {
constexpr std::array<std::array<float, 3>, 10> kMarioColorPresets = {{
    {{1.0f, 0.0f, 0.0f}},   // 0 = red (vanilla)
    {{1.0f, 0.5f, 0.0f}},   // 1 = orange
    {{1.0f, 1.0f, 0.0f}},   // 2 = yellow
    {{0.5f, 1.0f, 0.0f}},   // 3 = lime
    {{0.0f, 1.0f, 0.0f}},   // 4 = green
    {{0.0f, 1.0f, 1.0f}},   // 5 = cyan
    {{0.0f, 0.5f, 1.0f}},   // 6 = blue
    {{0.6f, 0.0f, 1.0f}},   // 7 = purple
    {{1.0f, 0.0f, 1.0f}},   // 8 = magenta
    {{1.0f, 0.5f, 0.7f}},   // 9 = pink
}};
}  // namespace

void LibSM64Manager::set_mario_color_preset(int preset) {
  if (preset < 0 || preset >= static_cast<int>(kMarioColorPresets.size())) {
    preset = 0;
  }
  m_mario_color_preset.store(preset, std::memory_order_release);
}

std::array<float, 3> LibSM64Manager::get_mario_tint() const {
  int p = m_mario_color_preset.load(std::memory_order_acquire);
  if (p < 0 || p >= static_cast<int>(kMarioColorPresets.size())) p = 0;
  return kMarioColorPresets[p];
}

u64 pc_sm64_set_mario_color(u32 preset) {
  LibSM64Manager::instance().set_mario_color_preset(static_cast<int>(preset));
  return 0;
}

u64 pc_sm64_set_corpse_render_enabled(u32 enabled) {
  LibSM64Manager::instance().set_corpse_render_enabled(enabled != 0);
  return 0;
}

// GOAL #t/#f from a C++ bool.  Mirrors common/kmachine.cpp's bool_to_
// symbol but is local to libsm64 so we don't need to expose a kernel
// helper just for this one bridge pair.  Anchored to GameVersion::Jak1
// since this whole project is jak1.
static inline u64 sm64_bool_to_symbol(bool val) {
  return val ? static_cast<u64>(s7.offset) + true_symbol_offset(GameVersion::Jak1)
             : static_cast<u64>(s7.offset);
}

u64 pc_sm64_rom_loaded() {
  return sm64_bool_to_symbol(LibSM64Manager::instance().is_initialized());
}

u64 pc_sm64_prompt_for_rom() {
  return sm64_bool_to_symbol(LibSM64Manager::instance().prompt_for_rom_and_init());
}

// ---------------------------------------------------------------------------
// GOAL-callable star dance: registered as "pc-sm64-star-dance-mario".
// ---------------------------------------------------------------------------
void LibSM64Manager::star_dance_mario_from_goal(float face_angle_rad) {
  if (!m_initialized || m_mario_id < 0) return;
  {
    std::scoped_lock lock(m_sm64_lock);
    // ACT_STAR_DANCE_NO_EXIT — plays the celebration animation without exiting the level.
    sm64_set_mario_action(m_mario_id, 0x00001307);
    // Pin Mario's facing yaw so the dance plays toward the camera.  GOAL
    // computes the angle from Mario→camera and passes it in radians; we
    // forward to libsm64's faceangle setter (same convention used by
    // teleport_mario_to_jak / read_target_transform — radians,
    // 0 = facing the +Z hemisphere of Jak/SM64-shared world space).
    // Pass NaN from GOAL to leave the angle untouched (legacy behavior).
    if (std::isfinite(face_angle_rad)) {
      sm64_set_mario_faceangle(m_mario_id, face_angle_rad);
    }
    // SOUND_MENU_STAR_SOUND — the iconic star jingle.
    // SOUND_ARG_LOAD(7, 0, 0x1E, 0xFF, 8) = 0x701EFF81
    sm64_play_sound_global(0x701EFF81);
    // SOUND_MARIO_HERE_WE_GO — Mario's voice line.
    // SOUND_ARG_LOAD(2, 4, 0x0C, 0x80, 8) = 0x240C8081
    sm64_play_sound_global(0x240C8081);
  }
  m_star_dance_timer = 0;
}

u64 pc_sm64_star_dance_mario(u32 face_angle_bits) {
  // GOAL passes the float-encoded face angle in an integer register slot.
  float yaw;
  std::memcpy(&yaw, &face_angle_bits, 4);
  LibSM64Manager::instance().star_dance_mario_from_goal(yaw);
  return 0;
}

// ---------------------------------------------------------------------------
// GOAL-callable sound player: registered as "pc-sm64-play-sound".
// Takes a 32-bit SM64 sound-bits value and plays it globally.
// ---------------------------------------------------------------------------
void LibSM64Manager::play_sound_from_goal(int32_t sound_bits) {
  if (!m_initialized) return;
  std::scoped_lock lock(m_sm64_lock);
  sm64_play_sound_global(sound_bits);
}

u64 pc_sm64_play_sound(u64 sound_bits) {
  LibSM64Manager::instance().play_sound_from_goal(static_cast<int32_t>(sound_bits));
  return 0;
}

// ---------------------------------------------------------------------------
// GOAL-callable music player: registered as "pc-sm64-play-music".
// Always stops the currently playing background music first so you don't end
// up layering two tracks when GOAL switches songs, then starts the new track
// on player 0 with no fade.  Clears the "force unpaused" flag so subsequent
// pause transitions actually mute audio again (important when switching
// from title-screen music to gameplay music).
// ---------------------------------------------------------------------------
void LibSM64Manager::play_music_from_goal(uint8_t seq_id) {
  if (!m_initialized) return;
  std::scoped_lock lock(m_sm64_lock);

  // Short-circuit: this exact track is already playing in the same
  // (non-forced) mode.  Skip the stop+start churn so calls from GOAL
  // that fire every frame (e.g. an update-mario-music! tick that keeps
  // asking for the level theme) don't restart the song from the top.
  if (seq_id != 0 && seq_id == m_current_bg_music_seq && !m_current_bg_music_forced) {
    return;
  }

  // Same track, just flipping out of forced-unpaused mode — keep the
  // cursor alive and only update the pause behaviour.
  if (seq_id != 0 && seq_id == m_current_bg_music_seq && m_current_bg_music_forced) {
    m_current_bg_music_forced = false;
    m_force_audio_unpaused = false;
    if (m_audio) m_audio->set_paused(m_game_paused);
    return;
  }

  // Different track (or a real start / stop-then-start).  Fall through
  // to the full stop+play path.
  sm64_stop_background_music(sm64_get_current_background_music());
  if (seq_id != 0) {
    sm64_play_music(0, seq_id, 0);
  }
  m_current_bg_music_seq = seq_id;
  m_current_bg_music_forced = false;
  // Back to pause-responsive mode, and re-sync audio player to the current
  // game-pause state so if the player is paused right now (rare — GOAL
  // usually doesn't fire this call mid-pause) audio still mutes correctly.
  m_force_audio_unpaused = false;
  if (m_audio) m_audio->set_paused(m_game_paused);
}

// Plays a track that ignores Jak's pause state — the audio worker is held
// unpaused regardless of master-mode.  Use for title / menu / save-select
// music where `master-mode` is already outside 'game and the default
// pause-responsive behavior would wrongly mute the menu theme.
void LibSM64Manager::play_music_forced_from_goal(uint8_t seq_id) {
  if (!m_initialized) return;
  std::scoped_lock lock(m_sm64_lock);

  // Short-circuit: same track, already in forced-unpaused mode.
  if (seq_id != 0 && seq_id == m_current_bg_music_seq && m_current_bg_music_forced) {
    return;
  }

  // Same track, just flipping INTO forced-unpaused mode — keep playing
  // and only flip the flag + un-mute.
  if (seq_id != 0 && seq_id == m_current_bg_music_seq && !m_current_bg_music_forced) {
    m_current_bg_music_forced = true;
    m_force_audio_unpaused = true;
    if (m_audio) m_audio->set_paused(false);
    return;
  }

  sm64_stop_background_music(sm64_get_current_background_music());
  if (seq_id != 0) {
    sm64_play_music(0, seq_id, 0);
  }
  m_current_bg_music_seq = seq_id;
  m_current_bg_music_forced = true;
  m_force_audio_unpaused = true;
  if (m_audio) m_audio->set_paused(false);  // immediately un-mute
}

void LibSM64Manager::stop_music_from_goal() {
  if (!m_initialized) return;
  std::scoped_lock lock(m_sm64_lock);
  // Already stopped — nothing to do.
  if (m_current_bg_music_seq == 0) {
    // Still ensure pause mode is consistent with m_force_audio_unpaused
    // being cleared, in case it somehow drifted.
    m_force_audio_unpaused = false;
    m_current_bg_music_forced = false;
    if (m_audio) m_audio->set_paused(m_game_paused);
    return;
  }
  sm64_stop_background_music(sm64_get_current_background_music());
  m_current_bg_music_seq = 0;
  m_current_bg_music_forced = false;
  // Stopping music returns audio to the default pause-responsive mode.
  m_force_audio_unpaused = false;
  if (m_audio) m_audio->set_paused(m_game_paused);
}

// Called each frame from the renderer BEFORE the pause early-return so we
// see both pause→unpause and unpause→pause edges.  Instead of stopping and
// restarting the track (which resets the sequence cursor), we flip a flag
// on the cubeb audio worker: while paused, its fill() callback outputs
// silence and never calls sm64_audio_tick().  The N64 audio engine — and
// therefore the active music position, SFX, reverb tail, everything — stays
// exactly where it was.  Unpausing resumes playback seamlessly mid-bar.
//
// When `m_force_audio_unpaused` is set (by play_music_forced_from_goal),
// the audio player stays unpaused regardless of what the game master-mode
// says — that's how title-screen / menu music keeps playing despite the
// master-mode not being 'game.
void LibSM64Manager::update_music_pause_state(bool is_paused) {
  if (!m_initialized || !m_audio) return;
  m_game_paused = is_paused;
  const bool want_audio_paused = is_paused && !m_force_audio_unpaused;
  if (want_audio_paused != m_audio->is_paused()) {
    m_audio->set_paused(want_audio_paused);
    // sm64_audio.cpp's fill() no longer reads m_paused; the freeze
    // happens at the N64 sequence-player layer so SFX still play while
    // music stays pinned.  Toggle the music + jingle players (IDs 0 &
    // 1) under the shared libsm64 lock — SFX player (2) stays active
    // so `sm64_play_sound_global` calls made during pause (the menu
    // pause chime, menu nav blips) audibly reach the speakers.
    std::scoped_lock lock(m_sm64_lock);
    sm64_set_music_paused(want_audio_paused ? 1 : 0);
  }

  // Credits / ending loop maintenance.  Some ROM sequences are
  // non-looping by design (seqId 0x1A = credits, 0x20 = ending), so
  // when we map them to a Jak level (e.g. `beach` → 'credits in
  // mario-music.gc) the track plays once and goes silent.  Poll the
  // level sequence player every frame; if we think a track should be
  // playing (`m_current_bg_music_seq != 0`) but libsm64 reports the
  // player has shut itself off, re-queue the same seq so the track
  // restarts from the top — an artificial loop.  Skip while paused
  // because the player is intentionally disabled then (and enabled
  // would come back naturally on unpause via the saved state in
  // sm64_set_music_paused).
  if (m_current_bg_music_seq != 0 && !want_audio_paused) {
    std::scoped_lock lock(m_sm64_lock);
    if (!sm64_bg_music_is_active()) {
      sm64_play_music(0, m_current_bg_music_seq, 0);
    }
  }
}

u64 pc_sm64_play_music(u64 seq_id) {
  LibSM64Manager::instance().play_music_from_goal(static_cast<uint8_t>(seq_id));
  return 0;
}

// GOAL-callable volume: registered as "pc-sm64-set-music-volume".
// Accepts a float in the 0.0..100.0 range (GOAL's standard volume slider
// range); clamps and forwards to SM64AudioPlayer::set_volume which
// applies to every sample coming out of sm64_audio_tick (music + SFX).
u64 pc_sm64_set_music_volume(u32 vol_bits) {
  float vol;
  std::memcpy(&vol, &vol_bits, 4);
  if (!std::isfinite(vol)) vol = 100.0f;
  if (vol < 0.0f) vol = 0.0f;
  if (vol > 100.0f) vol = 100.0f;
  LibSM64Manager::instance().set_audio_volume(static_cast<int>(vol));
  return 0;
}

u64 pc_sm64_play_music_forced(u64 seq_id) {
  LibSM64Manager::instance().play_music_forced_from_goal(static_cast<uint8_t>(seq_id));
  return 0;
}

u64 pc_sm64_stop_music() {
  LibSM64Manager::instance().stop_music_from_goal();
  return 0;
}

// ---------------------------------------------------------------------------
// GOAL-callable teleport: registered as "pc-sm64-teleport-mario".
// Takes x, y, z as GOAL floats (Jak units) packed into u32, converts to SM64
// scale, and calls sm64_set_mario_position.
// ---------------------------------------------------------------------------
void LibSM64Manager::teleport_mario_from_goal(float x, float y, float z) {
  if (!m_initialized || m_mario_id < 0) return;
  float sm64_x = x * JAK_TO_SM64_SCALE;
  float sm64_y = y * JAK_TO_SM64_SCALE;
  float sm64_z = z * JAK_TO_SM64_SCALE;
  {
    std::scoped_lock lock(m_sm64_lock);
    sm64_set_mario_position(m_mario_id, sm64_x, sm64_y, sm64_z);
  }
}

u64 pc_sm64_teleport_mario(u32 x_bits, u32 y_bits, u32 z_bits) {
  float x, y, z;
  memcpy(&x, &x_bits, 4);
  memcpy(&y, &y_bits, 4);
  memcpy(&z, &z_bits, 4);
  LibSM64Manager::instance().teleport_mario_from_goal(x, y, z);
  return 0;
}

// ---------------------------------------------------------------------------
// pc-sm64-spawn-mario-at-jak — read Jak's position from EE memory and call
// create_mario.  No-op if libsm64 isn't ready, *target* doesn't exist yet,
// or Mario already exists (avoid double-spawn — caller should pc-sm64-
// delete-mario first if they want to relocate).  Lifts the inline logic
// from sm64_debug_gui's "Spawn at Jak" button so both UIs share one path.
// ---------------------------------------------------------------------------
u64 pc_sm64_spawn_mario_at(u32 x_bits, u32 y_bits, u32 z_bits) {
  auto& mgr = LibSM64Manager::instance();
  if (!mgr.is_initialized()) {
    lg::warn("[libsm64] spawn-at: libsm64 not initialized");
    return 0;
  }
  if (mgr.has_mario()) {
    lg::info("[libsm64] spawn-at: Mario already spawned (id={}); ignoring",
             mgr.get_mario_id());
    return 0;
  }
  float x, y, z;
  std::memcpy(&x, &x_bits, 4);
  std::memcpy(&y, &y_bits, 4);
  std::memcpy(&z, &z_bits, 4);
  int32_t id = mgr.create_mario(x, y, z);
  if (id < 0) {
    lg::warn("[libsm64] spawn-at: create_mario rejected ({:.1f}, {:.1f}, {:.1f}) "
             "— no floor under target?", x, y, z);
    return 0;
  }
  lg::info("[libsm64] spawn-at: Mario spawned at explicit ({:.1f}, {:.1f}, {:.1f}) id={}",
           x, y, z, id);
  return 1;
}

u64 pc_sm64_log_mario_state() {
  auto& mgr = LibSM64Manager::instance();
  if (!mgr.is_initialized()) {
    lg::info("[sm64] state: libsm64 not initialised");
    return 0;
  }
  if (!mgr.has_mario()) {
    lg::info("[sm64] state: no Mario alive (m_mario_id < 0)");
    return 0;
  }
  auto state = mgr.get_state();
  lg::info("[sm64] state: pos=({:.1f}, {:.1f}, {:.1f}) yaw={:.2f}rad "
           "action=0x{:08X} health=0x{:04X} vel-fwd={:.2f}",
           state.position.x(), state.position.y(), state.position.z(),
           state.face_angle, state.action,
           static_cast<uint16_t>(state.health), state.forward_velocity);
  return 0;
}

u64 pc_sm64_spawn_mario_at_jak() {
  auto& mgr = LibSM64Manager::instance();
  // Throttled bail diagnostics — when the user calls this from the REPL or
  // the debug menu and gets back 0, a once-per-second diag log on the gk
  // side tells them why.  Counters reset on success.
  static int diag_uninit = 0;
  static int diag_has_mario = 0;
  static int diag_no_target = 0;
  if (!mgr.is_initialized() || !g_ee_main_mem) {
    diag_uninit++;
    if (diag_uninit == 1 || diag_uninit % 60 == 0) {
      lg::info("[sm64] spawn-at-jak bail: libsm64 not initialised (or no ee_mem) "
               "— total {}", diag_uninit);
    }
    return 0;
  }
  if (mgr.has_mario()) {
    diag_has_mario++;
    if (diag_has_mario == 1 || diag_has_mario % 60 == 0) {
      lg::info("[sm64] spawn-at-jak bail: Mario already alive (id={}, total {})",
               mgr.get_mario_id(), diag_has_mario);
    }
    return 0;
  }
  math::Vector3f jak_pos;
  if (!mgr.read_target_transform(g_ee_main_mem, &jak_pos, nullptr)) {
    diag_no_target++;
    if (diag_no_target == 1 || diag_no_target % 60 == 0) {
      lg::info("[sm64] spawn-at-jak bail: read_target_transform failed "
               "(*target* not bound or trans unreachable) — total {}",
               diag_no_target);
    }
    return 0;
  }
  // Reset bail counters on the happy path so a future failure starts fresh.
  diag_uninit = diag_has_mario = diag_no_target = 0;
  // create_mario takes Jak units directly — it does the JAK_TO_SM64_SCALE
  // conversion internally.
  int32_t id = mgr.create_mario(jak_pos.x(), jak_pos.y(), jak_pos.z());
  if (id < 0) {
    lg::warn("[libsm64] spawn-at-jak: create_mario failed");
    return 0;
  }
  lg::info("[libsm64] spawn-at-jak: Mario spawned at ({:.1f}, {:.1f}, {:.1f}) id={}",
           jak_pos.x(), jak_pos.y(), jak_pos.z(), id);
  return 1;
}

// ---------------------------------------------------------------------------
// GOAL-callable "shove Mario" (knockback only, no HP loss).  Registered as
// "pc-sm64-shove-mario".  Takes the shove SOURCE point in Jak units (the
// thing Mario should be knocked away from — e.g. the snow-bumper's root
// trans) and forwards to sm64_mario_take_damage with damage=0, which
// skips the hurtCounter math and the attacked-sound but still picks an
// appropriate knockback action via fake_determine_knockback_action.
// ---------------------------------------------------------------------------
void LibSM64Manager::shove_mario_from_goal(float src_x, float src_y, float src_z) {
  if (!m_initialized || m_mario_id < 0) return;
  const float sm64_x = src_x * JAK_TO_SM64_SCALE;
  const float sm64_y = src_y * JAK_TO_SM64_SCALE;
  const float sm64_z = src_z * JAK_TO_SM64_SCALE;
  std::scoped_lock lock(m_sm64_lock);
  sm64_mario_take_damage(m_mario_id, /*damage=*/0, /*subtype=*/0, sm64_x, sm64_y, sm64_z);
}

u64 pc_sm64_shove_mario(u32 x_bits, u32 y_bits, u32 z_bits) {
  float x, y, z;
  memcpy(&x, &x_bits, 4);
  memcpy(&y, &y_bits, 4);
  memcpy(&z, &z_bits, 4);
  LibSM64Manager::instance().shove_mario_from_goal(x, y, z);
  return 0;
}

bool LibSM64Manager::read_target_transform(u8* ee_mem,
                                           math::Vector3f* out_pos,
                                           float* out_yaw_rad) {
  if (!ee_mem) return false;
  u32 false_val = s7.offset;
  if (false_val == 0) return false;

  u32 target_ptr = sm64_get_symbol_value("*target*");
  if (target_ptr == 0 || target_ptr == false_val) return false;

  // Pull version-aware offsets — jak 2's process is 12 bytes larger than
  // jak 1's (extra `level` ptr + `pad-unknown-0` uint32[2]), so root /
  // node-list shift down accordingly.  trsqv.trans and quat are engine
  // struct fields and stay put.
  const auto offs = sm64_target_offsets();
  const u32 ROOT_RUNTIME_OFF = offs.process_drawable_root;
  const u32 TRANS_RUNTIME_OFF = offs.trsqv_trans;
  const u32 QUAT_RUNTIME_OFF = offs.trsqv_quat;
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

void LibSM64Manager::read_target_flags(u8* ee_mem) {
  target_grabbed = false;
  target_periscope = false;
  target_clone_anim = false;
  target_in_movie = false;
  target_dying = false;
  if (!ee_mem) return;
  u32 false_val = s7.offset;
  if (false_val == 0) return;
  const u32 true_val = sm64_true_offset();

  u32 ptr = sm64_get_symbol_value("*sm64-target-flags*");
  if (ptr != 0 && ptr != false_val && ptr + 16 <= EE_MAIN_MEM_SIZE) {
    float data[4];
    std::memcpy(data, ee_mem + ptr, 16);
    target_grabbed = data[0] > 0.5f;
    target_periscope = data[1] > 0.5f;
    target_clone_anim = data[2] > 0.5f;
    target_in_movie = data[3] > 0.5f;
  }

  // *sm64-jak-dying* is a plain symbol holding #t/#f — a separate channel from
  // the float-flags vector so it's robust to the vector being zero-initialized
  // before mario.gc's watcher loop has run for the first time.  Use the
  // helper so the symbol-value access stays version-agnostic; 0 means
  // "symbol doesn't exist yet" → keep target_dying=false.
  if (true_val != 0) {
    target_dying = (sm64_find_symbol_value("*sm64-jak-dying*") == true_val);
  }
}

MarioInputState LibSM64Manager::read_mario_input_from_goal(u8* /*ee_mem*/) {
  // Return the latest values latched by pc-sm64-set-input + pc-sm64-set-
  // camera-look.  Atomics make this lock-free across the GOAL kernel thread
  // (writer) and GL thread (reader); torn reads of the floats don't matter
  // because both sides write-then-read in tight loops every frame.
  MarioInputState input{};
  input.stick_x = m_input_stick_x.load(std::memory_order_acquire);
  input.stick_y = m_input_stick_y.load(std::memory_order_acquire);
  // Camera forward vector — defaults to (0, 1) world-Z when the GOAL side
  // hasn't called pc-sm64-set-camera-look yet, which preserves the legacy
  // "world-relative stick" feel during the pre-spawn / no-camera window.
  input.cam_look_x = m_input_cam_look_x.load(std::memory_order_acquire);
  input.cam_look_z = m_input_cam_look_z.load(std::memory_order_acquire);
  uint32_t b = m_input_buttons.load(std::memory_order_acquire);
  input.button_a = (b & 1u) != 0;
  input.button_b = (b & 2u) != 0;
  input.button_z = (b & 4u) != 0;
  return input;
}

u64 pc_sm64_set_input(u32 stick_x_bits, u32 stick_y_bits, u32 buttons) {
  float sx, sy;
  std::memcpy(&sx, &stick_x_bits, 4);
  std::memcpy(&sy, &stick_y_bits, 4);
  LibSM64Manager::instance().set_input_from_goal(sx, sy, buttons);
  return 0;
}

// ---------------------------------------------------------------------------
// pc-sm64-set-camera-look — push the camera's world-XZ forward vector to
// libsm64 so Mario's stick rotates with the camera.  GOAL passes float bits
// packed into u32 (same ABI as teleport / shove).  Manager-side normalises
// the vector and falls back to (0, 1) world-Z if magnitude is too small.
// ---------------------------------------------------------------------------
u64 pc_sm64_set_camera_look(u32 cam_x_bits, u32 cam_z_bits) {
  float cx, cz;
  std::memcpy(&cx, &cam_x_bits, 4);
  std::memcpy(&cz, &cam_z_bits, 4);
  LibSM64Manager::instance().set_camera_look_from_goal(cx, cz);
  return 0;
}

bool LibSM64Manager::is_game_paused(u8* ee_mem) {
  if (!ee_mem) return false;
  u32 false_val = s7.offset;
  if (false_val == 0) return false;

  // Get *master-mode* symbol (contains current game state mode)
  u32 master_mode_ptr = sm64_get_symbol_value("*master-mode*");
  if (master_mode_ptr == 0 || master_mode_ptr > EE_MAIN_MEM_SIZE) return false;

  // Get the game mode symbol to compare against (normal gameplay).  We want
  // its symbol-table address (sym.offset), not its value.
  u32 game_ptr = sm64_get_symbol_offset("game");
  if (game_ptr == 0) return false;

  // Mario should freeze when NOT in 'game mode (pause, menu, freeze, progress, etc.)
  return master_mode_ptr != game_ptr;
}

bool LibSM64Manager::is_progress_screen_paused(u8* ee_mem) {
  if (!ee_mem) return false;
  u32 false_val = s7.offset;
  if (false_val == 0) return false;

  u32 master_mode_ptr = sm64_get_symbol_value("*master-mode*");
  if (master_mode_ptr == 0 || master_mode_ptr > EE_MAIN_MEM_SIZE) return false;

  u32 progress_off = sm64_get_symbol_offset("progress");
  if (progress_off == 0) return false;

  return master_mode_ptr == progress_off;
}

bool LibSM64Manager::is_in_movie(u8* ee_mem) {
  if (!ee_mem) return false;
  u32 false_val = s7.offset;
  if (false_val == 0) return false;

  u32 master_mode_ptr = sm64_get_symbol_value("*master-mode*");
  if (master_mode_ptr == 0 || master_mode_ptr > EE_MAIN_MEM_SIZE) return false;

  // Compare *master-mode* against the 'movie symbol.  Symbol pointers in
  // GOAL live in the symbol table relative to s7, same pattern as the
  // 'game comparison in is_game_paused.
  u32 movie_off = sm64_get_symbol_offset("movie");
  if (movie_off == 0) return false;

  return master_mode_ptr == movie_off;
}

bool LibSM64Manager::read_cutscene_track_position(u8* ee_mem,
                                                  math::Vector3f* out_pos,
                                                  int* out_used_bone) {
  if (out_used_bone) *out_used_bone = -1;
  if (!ee_mem) return false;
  u32 false_val = s7.offset;
  if (false_val == 0) return false;

  u32 target_ptr = sm64_get_symbol_value("*target*");
  if (target_ptr == 0 || target_ptr == false_val) return false;

  // Version-aware process-drawable offsets.  cspace-array is engine struct
  // and identical between versions.
  const auto offs = sm64_target_offsets();
  const u32 ROOT_RUNTIME_OFF      = offs.process_drawable_root;
  const u32 NODE_LIST_RUNTIME_OFF = offs.process_drawable_node_list;
  const u32 TRANS_RUNTIME_OFF     = offs.trsqv_trans;
  constexpr u32 CSPACE_ARRAY_DATA_OFF = 12;
  constexpr u32 CSPACE_SIZE           = 32;
  constexpr u32 CSPACE_BONE_OFF       = 16;

  // ---- Try the bone path first (mirrors teleport_mario_to_jak) -------
  if (g_cutscene_track_bone >= 0 &&
      target_ptr + NODE_LIST_RUNTIME_OFF + 4 <= EE_MAIN_MEM_SIZE) {
    u32 node_list = 0;
    std::memcpy(&node_list, ee_mem + target_ptr + NODE_LIST_RUNTIME_OFF, 4);
    if (node_list != 0 && node_list != false_val &&
        (node_list & 0x7) == 4 &&
        node_list + 4 <= EE_MAIN_MEM_SIZE) {
      u32 cspace_len = 0;
      std::memcpy(&cspace_len, ee_mem + node_list, 4);
      if (cspace_len > 0 && cspace_len < 1024 &&
          static_cast<u32>(g_cutscene_track_bone) < cspace_len) {
        u32 cspace_addr = node_list + CSPACE_ARRAY_DATA_OFF +
                          static_cast<u32>(g_cutscene_track_bone) * CSPACE_SIZE;
        if (cspace_addr + CSPACE_SIZE <= EE_MAIN_MEM_SIZE) {
          u32 bone_ptr = 0;
          std::memcpy(&bone_ptr, ee_mem + cspace_addr + CSPACE_BONE_OFF, 4);
          if (bone_ptr != 0 && bone_ptr != false_val &&
              (bone_ptr & 0xF) == 0 &&
              bone_ptr + 64 <= EE_MAIN_MEM_SIZE) {
            float m[16];
            std::memcpy(m, ee_mem + bone_ptr, 64);
            bool ok = true;
            for (int i = 0; i < 16; i++) if (!std::isfinite(m[i])) { ok = false; break; }
            if (ok) {
              for (int c = 0; c < 3; c++) {
                if (std::abs(m[3 * 4 + c]) > 1.0e8f) { ok = false; break; }
              }
            }
            if (ok) {
              if (out_pos) *out_pos = math::Vector3f(m[12], m[13], m[14]);
              if (out_used_bone) *out_used_bone = g_cutscene_track_bone;
              return true;
            }
          }
        }
      }
    }
  }

  // ---- Fallback: root.trans ------------------------------------------
  if (target_ptr + ROOT_RUNTIME_OFF + 4 > EE_MAIN_MEM_SIZE) return false;
  u32 root_ptr;
  std::memcpy(&root_ptr, ee_mem + target_ptr + ROOT_RUNTIME_OFF, 4);
  if (root_ptr == 0 || root_ptr == false_val) return false;
  if (root_ptr + TRANS_RUNTIME_OFF + 16 > EE_MAIN_MEM_SIZE) return false;
  float trans[4];
  std::memcpy(trans, ee_mem + root_ptr + TRANS_RUNTIME_OFF, 16);
  if (out_pos) *out_pos = math::Vector3f(trans[0], trans[1], trans[2]);
  if (out_used_bone) *out_used_bone = -1;
  return true;
}

void LibSM64Manager::teleport_mario_to_jak(u8* ee_mem) {
  if (!m_initialized || m_mario_id < 0 || !ee_mem) return;
  u32 false_val = s7.offset;
  if (false_val == 0) return;

  u32 target_ptr = sm64_get_symbol_value("*target*");
  if (target_ptr == 0 || target_ptr == false_val) return;

  ++m_teleport_call_count;  // diagnostic — see teleport_call_count()

  // Field offsets — version-aware (jak2 process is +12 bytes vs jak1).
  // See sm64_target_offsets() for the per-version table.
  const auto offs = sm64_target_offsets();
  const u32 ROOT_RUNTIME_OFF      = offs.process_drawable_root;
  const u32 NODE_LIST_RUNTIME_OFF = offs.process_drawable_node_list;
  const u32 TRANS_RUNTIME_OFF     = offs.trsqv_trans;
  const u32 QUAT_RUNTIME_OFF      = offs.trsqv_quat;
  // cspace-array layout (same as update_actor_collision uses):
  //   data starts at runtime offset 12 (GOAL :offset 16 minus 4 basic tag)
  //   cspace entries are 32 bytes each
  //   cspace.bone (a basic ptr) is at offset 16 within the cspace struct
  constexpr u32 CSPACE_ARRAY_DATA_OFF = 12;
  constexpr u32 CSPACE_SIZE           = 32;
  constexpr u32 CSPACE_BONE_OFF       = 16;

  // Helper: read `node-list[idx].bone.transform` into a 16-float matrix.
  // Returns false and leaves m unchanged on any failure.
  auto read_bone_matrix = [&](int idx, float m[16]) -> bool {
    if (idx < 0) return false;
    if (target_ptr + NODE_LIST_RUNTIME_OFF + 4 > EE_MAIN_MEM_SIZE) return false;
    u32 node_list = 0;
    std::memcpy(&node_list, ee_mem + target_ptr + NODE_LIST_RUNTIME_OFF, 4);
    if (node_list == 0 || node_list == false_val) return false;
    if ((node_list & 0x7) != 4) return false;
    if (node_list + 4 > EE_MAIN_MEM_SIZE) return false;
    u32 cspace_len = 0;
    std::memcpy(&cspace_len, ee_mem + node_list, 4);
    if (cspace_len == 0 || cspace_len >= 1024) return false;
    if (static_cast<u32>(idx) >= cspace_len) return false;
    u32 cspace_addr =
        node_list + CSPACE_ARRAY_DATA_OFF + static_cast<u32>(idx) * CSPACE_SIZE;
    if (cspace_addr + CSPACE_SIZE > EE_MAIN_MEM_SIZE) return false;
    u32 bone_ptr = 0;
    std::memcpy(&bone_ptr, ee_mem + cspace_addr + CSPACE_BONE_OFF, 4);
    if (bone_ptr == 0 || bone_ptr == false_val) return false;
    if ((bone_ptr & 0xF) != 0) return false;
    if (bone_ptr + 64 > EE_MAIN_MEM_SIZE) return false;
    std::memcpy(m, ee_mem + bone_ptr, 64);
    for (int i = 0; i < 16; i++) if (!std::isfinite(m[i])) return false;
    for (int c = 0; c < 3; c++) if (std::abs(m[3 * 4 + c]) > 1.0e8f) return false;
    return true;
  };

  // Final outputs, in Jak world units + radians.
  float pos[3]   = {0, 0, 0};
  float pitch    = 0.0f;
  float yaw      = 0.0f;
  float roll     = 0.0f;
  bool  have_pos = false;
  bool  have_rot = false;

  // ---- Position source -------------------------------------------------
  // g_cutscene_track_bone > -1 → read that bone's translation (column 3
  // of the bone matrix).  Default 29 (Lankle) since its world position
  // follows the animation even when *target*'s root.trans is frozen.
  {
    float m[16];
    if (read_bone_matrix(g_cutscene_track_bone, m)) {
      pos[0] = m[3 * 4 + 0];
      pos[1] = m[3 * 4 + 1];
      pos[2] = m[3 * 4 + 2];
      have_pos = true;
    }
  }

  // ---- Rotation source -------------------------------------------------
  // g_cutscene_track_rot_bone > -1 → extract Euler angles from that
  // bone's rotation matrix.  Default 1 (align), which is the character
  // root-align joint — its forward axis tracks the body's facing
  // cleanly.  Lankle (the default *position* bone) is a poor rotation
  // source because its Z basis is the foot's forward vector, which
  // wobbles every step.
  //
  // Matrix → XYZ Euler (pitch/yaw/roll) assuming +Y-up, +Z-forward:
  //   yaw   = atan2(z_basis.x, z_basis.z)
  //   pitch = atan2(-z_basis.y, sqrt(z_basis.x² + z_basis.z²))
  //   roll  = atan2(-x_basis.y, y_basis.y)
  {
    float m[16];
    if (read_bone_matrix(g_cutscene_track_rot_bone, m)) {
      const float zx = m[2 * 4 + 0];
      const float zy = m[2 * 4 + 1];
      const float zz = m[2 * 4 + 2];
      yaw   = std::atan2(zx, zz);
      pitch = std::atan2(-zy, std::sqrt(zx * zx + zz * zz));
      const float xy = m[0 * 4 + 1];
      const float yy = m[1 * 4 + 1];
      roll  = std::atan2(-xy, yy);
      have_rot = true;
    }
  }

  // ---- Root-trans / root-quat fallback for whichever axis missed -------
  // If either bone read bailed (skeleton uninitialized, index too big,
  // user set the slider to -1), use the process's plain trsqv instead.
  if (!have_pos || !have_rot) {
    if (target_ptr + ROOT_RUNTIME_OFF + 4 > EE_MAIN_MEM_SIZE) return;
    u32 root_ptr;
    std::memcpy(&root_ptr, ee_mem + target_ptr + ROOT_RUNTIME_OFF, 4);
    if (root_ptr == 0 || root_ptr == false_val) return;
    if (root_ptr + QUAT_RUNTIME_OFF + 16 > EE_MAIN_MEM_SIZE) return;
    if (!have_pos) {
      float trans[4];
      std::memcpy(trans, ee_mem + root_ptr + TRANS_RUNTIME_OFF, 16);
      pos[0] = trans[0];
      pos[1] = trans[1];
      pos[2] = trans[2];
    }
    if (!have_rot) {
      float q[4];
      std::memcpy(q, ee_mem + root_ptr + QUAT_RUNTIME_OFF, 16);
      const float qx = q[0], qy = q[1], qz = q[2], qw = q[3];
      pitch = std::atan2(2.0f * (qw * qx - qy * qz),
                         1.0f - 2.0f * (qx * qx + qy * qy));
      yaw   = std::atan2(2.0f * (qw * qy + qx * qz),
                         1.0f - 2.0f * (qy * qy + qx * qx));
      float sr = 2.0f * (qw * qz + qx * qy);
      if (sr >  1.0f) sr =  1.0f;
      if (sr < -1.0f) sr = -1.0f;
      roll = std::asin(sr);
    }
  }

  const float sm64_x = pos[0] * JAK_TO_SM64_SCALE;
  const float sm64_y = pos[1] * JAK_TO_SM64_SCALE;
  const float sm64_z = pos[2] * JAK_TO_SM64_SCALE;

  {
    std::scoped_lock lock(m_sm64_lock);
    sm64_set_mario_position(m_mario_id, sm64_x, sm64_y, sm64_z);
    sm64_set_mario_faceangle(m_mario_id, yaw);
    // sm64_set_mario_angle writes marioObj->header.gfx.angle — the
    // rendered rotation, distinct from face_angle which is physics yaw.
    sm64_set_mario_angle(m_mario_id, pitch, yaw, roll);
  }
}

void LibSM64Manager::debug_glue_mario_to_jak(u8* ee_mem) {
  if (!m_initialized || m_mario_id < 0 || !ee_mem) return;
  math::Vector3f jak_pos;
  float jak_yaw;
  if (!read_target_transform(ee_mem, &jak_pos, &jak_yaw)) return;

  float sm64_x = jak_pos.x() * JAK_TO_SM64_SCALE;
  float sm64_y = jak_pos.y() * JAK_TO_SM64_SCALE;
  float sm64_z = jak_pos.z() * JAK_TO_SM64_SCALE;
  {
    std::scoped_lock lock(m_sm64_lock);
    sm64_set_mario_position(m_mario_id, sm64_x, sm64_y, sm64_z);
    sm64_set_mario_velocity(m_mario_id, 0.0f, 0.0f, 0.0f);
    sm64_set_mario_forward_velocity(m_mario_id, 0.0f);
    sm64_set_mario_action(m_mario_id, 0x0100088C);  // ACT_FREEFALL
    sm64_set_mario_faceangle(m_mario_id, jak_yaw);
  }
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

  // Skip water sync while the fishing minigame is active — same effect as
  // toggling water_sync off.  *sm64-fishing* is a jak1-only symbol from the
  // jungle level; on jak2 the find returns 0 (not-defined) → fall through.
  {
    u32 fishing_val = sm64_find_symbol_value("*sm64-fishing*");
    if (fishing_val != 0 && fishing_val != false_val) return;
  }

  u32 target_ptr = sm64_get_symbol_value("*target*");
  if (target_ptr == 0 || target_ptr == false_val) return;

  // process-drawable.water — runtime offset is root + 44 (water is the
  // 11th 4-byte field after root, identical layout in jak 1 and jak 2;
  // only the base shifts because of jak 2's larger process header).
  const u32 WATER_FIELD_RUNTIME_OFF = sm64_target_offsets().process_drawable_water;
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
  // wt17 (bit 17) is set by swamp-tar water-vols (e.g. the dark eco pools in
  // Boggy Swamp).  wt18 (bit 18) is set by misty mud (mud.gc:45).  Both are
  // treated identically: suppress the SM64 water level so Mario never enters
  // ACT_WATER_IDLE / swim state, and place a surface object so he can run
  // across the top with a floor under him.
  const bool is_tar_volume = (flags & ((1u << 17) | (1u << 18))) != 0;

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
  // Also treat the post-restore window as shell-riding: SM64 may briefly drop
  // the shell action while re-stabilising after a clone-anim cutscene.
  const bool riding_shell = (current_action & kActFlagRidingShell) != 0 ||
                             m_post_restore_shell_action != 0;

  // Decide the SM64 water level to feed libsm64 this tick.
  int sm64_water_level;
  if (in_water && !is_lava_volume && !is_tar_volume) {
    float water_y_jak;
    std::memcpy(&water_y_jak, ee_mem + water_ctrl_ptr + WC_HEIGHT_OFF, 4);
    // libsm64 stores waterLevel in SM64 units, same space as Mario's position.
    sm64_water_level = static_cast<int>(water_y_jak * JAK_TO_SM64_SCALE);
    // Publish for the post-tick shell-over-water correction in tick().
    m_in_water_volume = true;
    m_water_level_sm64 = water_y_jak * JAK_TO_SM64_SCALE;
    m_in_tar_volume = false;
  } else {
    m_in_water_volume = false;
    m_in_tar_volume = in_water && is_tar_volume;
    if (m_in_tar_volume) {
      // Tar: keep SM64 water level far below so Mario never enters swim state.
      // The actual floor comes from a surface object in update_tar_floor().
      // 1 Jak metre = 4096 units; * JAK_TO_SM64_SCALE (50/4096) = 50 SM64 units.
      constexpr float kTarWadeDepthSM64 = 50.0f;
      float water_y_jak;
      std::memcpy(&water_y_jak, ee_mem + water_ctrl_ptr + WC_HEIGHT_OFF, 4);
      m_tar_floor_y_sm64 = water_y_jak * JAK_TO_SM64_SCALE - kTarWadeDepthSM64;
    }
    // Dry, lava, or tar: keep SM64 water level far below Mario.
    sm64_water_level = -100000;
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
  // If blend < 1 and both snapshots have the same (non-zero) triangle count,
  // linearly interpolate vertex positions between the previous and current tick.
  if (render_blend < 1.0f &&
      m_prev_geometry.num_triangles > 0 &&
      m_prev_geometry.num_triangles == m_geometry.num_triangles) {
    MarioGeometry blended = m_geometry;  // copy colors/uvs/normals as-is
    const float t = render_blend;
    const float s = 1.0f - t;
    int n = m_geometry.num_triangles * 3;
    blended.position.resize(n * 3);
    for (int i = 0; i < n * 3; ++i) {
      blended.position[i] = s * m_prev_geometry.position[i] + t * m_geometry.position[i];
    }
    return blended;
  }
  return m_geometry;
}

MarioState LibSM64Manager::get_state() {
  std::lock_guard<std::mutex> lock(m_geo_mutex);
  return m_state;
}

MarioState LibSM64Manager::get_render_state() {
  std::lock_guard<std::mutex> lock(m_geo_mutex);
  if (render_blend >= 1.0f || m_prev_geometry.num_triangles == 0) {
    return m_state;
  }
  const float t = render_blend;
  const float s = 1.0f - t;
  MarioState blended = m_state;
  blended.position = math::Vector3f(
      s * m_prev_state.position.x() + t * m_state.position.x(),
      s * m_prev_state.position.y() + t * m_state.position.y(),
      s * m_prev_state.position.z() + t * m_state.position.z());
  // Lerp face angle via cos/sin to avoid discontinuity at the ±π wrap.
  // Use the unsuffixed std::cos/sin/atan2 — they're properly overloaded
  // for float in <cmath>, and unlike std::cosf/sinf/atan2f, they work on
  // libstdc++ (Linux clang).  MSVC's STL exposes the `f`-suffixed names
  // in std:: as an extension; libstdc++ doesn't.
  float ca = s * std::cos(m_prev_state.face_angle) + t * std::cos(m_state.face_angle);
  float sa = s * std::sin(m_prev_state.face_angle) + t * std::sin(m_state.face_angle);
  blended.face_angle = std::atan2(sa, ca);
  return blended;
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
constexpr u32 CSHAPE_SCALE_OFF = 44;  // trs::scale, declared offset 48, runtime 48-4=44
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
// Offsets of `pid` and `entity` fields in the `process` struct.
// process inherits from process-tree (28 bytes: name/mask/parent/brother/
// child/ppointer/self), then process adds pool/status before `pid`.
// Layout (all 4-byte fields, runtime offset = GOAL declared offset - 4):
//   pool(28), status(32), pid(36), main-thread(40), top-thread(44), entity(48)
// See gkernel-h.gc deftype process.
constexpr u32 PROCESS_PID_OFF    = 36;  // process.pid  (int32)
constexpr u32 PROCESS_ENTITY_OFF = 48;  // process.entity (entity-actor*)

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
  // Currently held (grabbed) actor process-drawable pointer, or 0 if not
  // holding anything. We skip this node so Mario doesn't collide with the
  // object he's carrying — otherwise the held actor's collide-shape pushes
  // Mario away every frame.
  u32 grabbed_actor_ptr;
  // Bouncy trampoline types. 0 means level not loaded — disables the check.
  u32 springbox_type;
  u32 spiderwebs_type;
  u32 teetertotter_type;
  u32 cavetrapdoor_type;
  // Our own GOAL hitbox process type — skip it so its collide-shape doesn't
  // get mirrored into libsm64 as a surface object.
  u32 sm64_mario_col_type;
  // touch-tracker: temporary attack/eco hitbox spheres. Not solid geometry —
  // must not become SM64 surfaces. Blue eco spawns an 18m-radius touch-tracker
  // that was shoving Mario out of bounds.
  u32 touch_tracker_type;
  // projectile (base type): temporary flying attack effects (eco shots, etc.).
  // Not solid geometry — projectile-blue spawns 5 spheres near Jak when blue
  // eco activates a platform, shoving Mario for a frame before they fly away.
  u32 projectile_type;
  bool dry_run;
  int& diag_logs_remaining;
  std::unordered_map<u32, bool>& is_process_drawable_cache;
  std::unordered_map<u32, bool>& is_collide_shape_cache;
  std::unordered_map<u32, bool>& is_pov_camera_cache;
  std::unordered_map<u32, bool>& is_citadelcam_cache;
  std::unordered_map<u32, bool>& is_projectile_cache;
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
    if (c.grabbed_actor_ptr != 0 && node == c.grabbed_actor_ptr) continue;

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

    // Skip our own GOAL-side hitbox process so its collide-shape doesn't get
    // mirrored into libsm64 as a surface object.
    if (c.sm64_mario_col_type != 0 && node_type == c.sm64_mario_col_type) continue;
    if (c.touch_tracker_type != 0 && node_type == c.touch_tracker_type) continue;

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
    if (type_is_descendant(c.ee_mem, c.mem_size, node_type, c.projectile_type,
                            c.is_projectile_cache)) {
      continue;
    }

    // Check if this actor is a bouncy trampoline type (springbox, spiderwebs)
    // or a teetertotter (seesaw). Direct pointer comparison — these are
    // concrete types with no subtypes.
    bool is_bouncy_actor =
        (c.springbox_type != 0 && node_type == c.springbox_type) ||
        (c.spiderwebs_type != 0 && node_type == c.spiderwebs_type);
    bool is_teetertotter_actor =
        (c.teetertotter_type != 0 && node_type == c.teetertotter_type);
    // cavetrapdoor rotates when it falls open; zeroing its Euler rotation
    // prevents libsm64's platform_displacement from spinning Mario.
    bool is_no_rotate_actor =
        (c.cavetrapdoor_type != 0 && node_type == c.cavetrapdoor_type);

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

    // NOTE: per-prim action/flag filtering happens further down per-prim in
    // the collect_mesh_prims loop below — we keep the root-level check here
    // permissive so we can still walk an actor whose root is solid but whose
    // individual children toggle solid on/off (e.g. plant-boss: alive body
    // spheres are solid, on death those get cleared and the stem/head
    // meshes become solid instead — both states need per-prim filtering).

    std::vector<CollectedPrim> prims;
    collect_mesh_prims(c.ee_mem, c.mem_size, root_prim, c.false_val, c.prim_mesh_type,
                       c.prim_group_type, c.prim_sphere_type, prims);
    if (prims.empty()) continue;

    // Read the actor's world transform and per-instance scale.
    float trans[4], quat[4], root_scale[4];
    if (!read_vec4(c.ee_mem, root + CSHAPE_TRANS_OFF, c.mem_size, trans)) continue;
    if (!read_vec4(c.ee_mem, root + CSHAPE_QUAT_OFF, c.mem_size, quat)) continue;
    // trs::scale at runtime offset 44. Default to (1,1,1) if unreadable or degenerate.
    if (!read_vec4(c.ee_mem, root + CSHAPE_SCALE_OFF, c.mem_size, root_scale)) {
      root_scale[0] = root_scale[1] = root_scale[2] = 1.0f;
    }
    for (int k = 0; k < 3; k++) {
      if (!std::isfinite(root_scale[k]) || root_scale[k] <= 0.0f) root_scale[k] = 1.0f;
    }

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

      // Per-prim "solid" action check.  Within one actor's prim tree,
      // children can flip solid on/off independently of the root — e.g.
      // plant-boss-dead clears the body spheres (prim-id 8/16) to non-solid
      // while simultaneously making the stem/head death-prim meshes solid.
      // Without this filter, Mario bumped into the old body sphere that GOAL
      // had already disabled (appearing to Mario as an invisible wall in the
      // boss's head area) AND still physically interacted with meshes/spheres
      // whose state is "disabled".  Skipping children that lack the solid
      // action bit follows exactly how Jak's own physics treats them.  Pure
      // trigger prims (offense=no-offense, action clear) with no solid bit
      // are interaction-only — enemy hurtboxes, detection volumes, etc. —
      // and should never be a wall for Mario.
      {
        constexpr uint32_t kCollideActionSolid = 1u << 0;
        u32 prim_action = 0;
        if (!read_u32(c.ee_mem, prim_ptr + PRIM_CORE_ACTION_OFF, c.mem_size, prim_action)) continue;
        if (!(prim_action & kCollideActionSolid)) continue;
      }

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
        // Guard against process-slot recycling: if a GOAL process dies and a
        // new process of the same type is allocated at the same EE address, its
        // prim_ptr is also the same → same key → we'd "move" the old surface
        // object from actor A's position to actor B's position in a single frame.
        // libsm64 interprets that delta as platform displacement and teleports
        // Mario.
        //
        // Detection layer 1: compare the process PID (process.pid,
        // PROCESS_PID_OFF=36). Every GOAL process.spawn() increments a global
        // counter and stamps the PID, so two successive processes at the same
        // EE slot always get different PIDs — regardless of entity pointer or
        // proximity. Guarded with both-non-zero to avoid false positives from
        // read failures.
        u32 cur_pid = 0;
        read_u32(c.ee_mem, node + PROCESS_PID_OFF, c.mem_size, cur_pid);
        bool slot_recycled =
            (tracked.pid != 0 && cur_pid != 0 && cur_pid != tracked.pid);
        // Detection layer 2: implausibly large single-frame jump (> 50 m)
        // catches the remaining edge case where both PID reads return 0.
        if (!slot_recycled) {
          constexpr float kRecycleThresholdSq = 204800.0f * 204800.0f;  // 50 m
          float dx = prim_pos[0] - tracked.last_trans[0];
          float dy = prim_pos[1] - tracked.last_trans[1];
          float dz = prim_pos[2] - tracked.last_trans[2];
          slot_recycled = (dx * dx + dy * dy + dz * dz) > kRecycleThresholdSq;
        }
        if (slot_recycled) {
          if (!c.dry_run) sm64_surface_object_delete(tracked.sm64_obj_id);
          tracked.has_obj = false;
          tracked.pid = 0;  // cleared so creation path stores the new pid
          // Fall through to the creation path below.
        } else {
          if (!c.dry_run) {
            sm64_surface_object_move(tracked.sm64_obj_id, &xform);
            // For no-rotate actors (e.g. cavetrapdoor), zero the angular velocity
            // fields that platform_displacement uses to yaw/pitch/roll Mario.
            // The position and face-angle are still updated correctly so the
            // collision geometry sits at the right world pose.
            if (is_no_rotate_actor) {
              auto* t = surfaces_object_get_transform_ptr(tracked.sm64_obj_id);
              if (t) {
                t->aAngleVelPitch = 0;
                t->aAngleVelYaw   = 0;
                t->aAngleVelRoll  = 0;
              }
            }
          }
          std::memcpy(tracked.last_trans, prim_pos, 12);
          refresh_world_aabb();
          continue;
        }
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
        // Bake per-instance root scale into the local-space mesh geometry.
        // libsm64 surface objects have no scale field. Mesh-prim actors like
        // citb-plat share a single mesh template authored at scale=1, with
        // per-instance scale stored in trs::scale (offset 44). Sphere-prim
        // actors (enemies etc.) have their collision radius already authored
        // at the correct world size, so scale is NOT applied to that path.
        if (root_scale[0] != 1.0f || root_scale[1] != 1.0f || root_scale[2] != 1.0f) {
          for (auto& s : surfaces) {
            for (int v = 0; v < 3; v++) {
              s.vertices[v][0] = static_cast<int32_t>(s.vertices[v][0] * root_scale[0]);
              s.vertices[v][1] = static_cast<int32_t>(s.vertices[v][1] * root_scale[1]);
              s.vertices[v][2] = static_cast<int32_t>(s.vertices[v][2] * root_scale[2]);
            }
          }
          for (int k = 0; k < 3; k++) {
            local_aabb_min[k] *= root_scale[k];
            local_aabb_max[k] *= root_scale[k];
            if (local_aabb_min[k] > local_aabb_max[k])
              std::swap(local_aabb_min[k], local_aabb_max[k]);
          }
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

      // Tag upward-facing (floor) surfaces for special actor types.
      // libsm64 classifies surfaces with normal.y > 0.01 as floors, so we
      // match that threshold. Side/wall triangles keep SURFACE_DEFAULT.
      if (is_bouncy_actor || is_teetertotter_actor) {
        constexpr int16_t SURFACE_BOUNCY_TYPE = 0x00FE;
        constexpr int16_t SURFACE_TEETERTOTTER_TYPE = 0x00FD;
        int16_t tag = is_bouncy_actor ? SURFACE_BOUNCY_TYPE : SURFACE_TEETERTOTTER_TYPE;
        for (auto& s : surfaces) {
          // Compute the triangle normal Y component to determine if it's a floor.
          // Cross product of (v1-v0) x (v2-v0), we only need the Y component:
          //   ny = (v1.z - v0.z)*(v2.x - v0.x) - (v1.x - v0.x)*(v2.z - v0.z)
          // Positive ny = upward-facing = floor.
          float e1x = (float)(s.vertices[1][0] - s.vertices[0][0]);
          float e1z = (float)(s.vertices[1][2] - s.vertices[0][2]);
          float e2x = (float)(s.vertices[2][0] - s.vertices[0][0]);
          float e2z = (float)(s.vertices[2][2] - s.vertices[0][2]);
          float ny = e1z * e2x - e1x * e2z;
          if (ny > 0.0f) {
            s.type = tag;
          }
        }
      }

      if (!c.dry_run) {
        SM64SurfaceObject obj{};
        obj.transform = xform;
        obj.surfaceCount = static_cast<uint32_t>(surfaces.size());
        obj.surfaces = surfaces.data();
        tracked.sm64_obj_id = sm64_surface_object_create(&obj);
        tracked.has_obj = true;
      }
      // Cache the PID so the recycling check next frame can detect
      // same-address-but-different-process actor replacements.
      {
        u32 new_pid = 0;
        read_u32(c.ee_mem, node + PROCESS_PID_OFF, c.mem_size, new_pid);
        tracked.pid = new_pid;
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
    u32 pd_val = sm64_find_symbol_value("process-drawable");
    u32 cs_val = sm64_find_symbol_value("collide-shape");
    u32 pm_val = sm64_find_symbol_value("collide-shape-prim-mesh");
    u32 pg_val = sm64_find_symbol_value("collide-shape-prim-group");
    u32 ps_val = sm64_find_symbol_value("collide-shape-prim-sphere");
    // For *active-pool* we want the symbol-table address (the value field
    // points to the active-pool object, which we then chase later).  Keep
    // the offset around so the walker can re-read its value freshly each
    // frame (the GOAL-level *active-pool* binding may change as processes
    // come and go).  find_ rather than intern_ because if *active-pool*
    // hasn't been bound yet we want to retry next frame, not allocate a
    // dangling slot.
    u32 ap_off = sm64_find_symbol_offset("*active-pool*");
    if (pd_val == 0 || pd_val == false_val || cs_val == 0 || cs_val == false_val ||
        pm_val == 0 || pm_val == false_val || pg_val == 0 || pg_val == false_val ||
        ps_val == 0 || ps_val == false_val || ap_off == 0) {
      return;  // type symbols exist but haven't been bound to Type structs yet,
               // OR *active-pool* isn't defined — retry next frame
    }
    m_type_cache.process_drawable = pd_val;
    m_type_cache.collide_shape = cs_val;
    m_type_cache.prim_mesh = pm_val;
    m_type_cache.prim_group = pg_val;
    m_type_cache.prim_sphere = ps_val;
    m_type_cache.active_pool_sym = ap_off;
    m_type_cache.ready = true;
    lg::info("[libsm64] Actor collision type cache ready: pd=0x{:X} cs=0x{:X} pm=0x{:X} pg=0x{:X} ps=0x{:X} ap_sym=0x{:X} false=0x{:X}",
             pd_val, cs_val, pm_val, pg_val, ps_val, ap_off, false_val);
  }

  // Retry the per-type lookups every frame until each is bound.  These
  // lookups are level-specific (most don't exist until the relevant level
  // is loaded), so a missing symbol → leave the cache slot 0 → that filter
  // is silently disabled by `type_is_descendant`.
  auto cache_type = [&](u32& slot, const char* name) {
    if (slot != 0) return;
    u32 v = sm64_find_symbol_value(name);
    if (v != 0 && v != false_val) {
      slot = v;
      lg::info("[libsm64] Actor collision: cached {} type @0x{:X}", name, v);
    }
  };
  cache_type(m_type_cache.pov_camera,    "pov-camera");
  cache_type(m_type_cache.citadelcam,    "citadelcam");
  cache_type(m_type_cache.springbox,     "springbox");
  cache_type(m_type_cache.spiderwebs,    "spiderwebs");
  cache_type(m_type_cache.teetertotter,  "teetertotter");
  cache_type(m_type_cache.sm64_mario_col,"sm64-mario-col");
  cache_type(m_type_cache.touch_tracker, "touch-tracker");
  cache_type(m_type_cache.projectile,    "projectile");
  cache_type(m_type_cache.cavetrapdoor,  "cavetrapdoor");

  // Resolve *target* every frame. Jak's process-drawable pointer changes
  // any time the player is re-spawned (e.g. death), and there's no upside
  // to caching it across frames — the symbol lookup is cheap. A 0 here
  // means *target* isn't bound yet, which disables the filter (the walker
  // will treat Jak as any other actor for one or two frames while the
  // kernel finishes wiring him up).
  u32 target_ptr_now = sm64_find_symbol_value("*target*");
  if (target_ptr_now == false_val) target_ptr_now = 0;

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
      m_grabbed_yakow_ee,
      m_type_cache.springbox,
      m_type_cache.spiderwebs,
      m_type_cache.teetertotter,
      m_type_cache.cavetrapdoor,
      m_type_cache.sm64_mario_col,
      m_type_cache.touch_tracker,
      m_type_cache.projectile,
      dynamic_actor_collision_dry_run,
      m_actor_diag_logs_remaining,
      m_is_process_drawable_cache,
      m_is_collide_shape_cache,
      m_is_pov_camera_cache,
      m_is_citadelcam_cache,
      m_is_projectile_cache,
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
  std::unordered_map<u32, bool> pd_cache, cs_cache, pov_cache, citadel_cache, proj_cache;
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
      0,  // grabbed_actor_ptr: no grab state in tests
      0,  // springbox_type: not needed in tests
      0,  // spiderwebs_type: not needed in tests
      0,  // teetertotter_type: not needed in tests
      0,  // cavetrapdoor_type: not needed in tests
      0,  // sm64_mario_col_type: not needed in tests
      0,  // touch_tracker_type: not needed in tests
      0,  // projectile_type: not needed in tests
      dry_run,
      dummy_diag_remaining,
      pd_cache,
      cs_cache,
      pov_cache,
      citadel_cache,
      proj_cache,
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
  u32 entity_addr;             // entity-actor pointer (stable across level reloads)
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

    // Read process->entity (used for cross-level rebind).  0 / #f / out-of-
    // range is fine — we just won't be able to rebind to this yakow if the
    // current grab needs to.
    u32 entity_addr = 0;
    read_u32(ee_mem, node + PROCESS_ENTITY_OFF, mem_size, entity_addr);
    if (entity_addr == false_val) entity_addr = 0;

    YakowRecord r;
    r.ee_addr = node;
    r.entity_addr = entity_addr;
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

  // Yakow grab is a jak 1-only feature — `yakow` actor type doesn't exist
  // in jak 2.  Bail before the symbol-table thrash on other versions.
  if (g_game_version != GameVersion::Jak1) return;

  // Lazy yakow-type resolve. The symbol may not exist yet on the first few
  // frames (e.g. before the village1 level has loaded its code), so a failed
  // lookup is silent and retried next frame.
  if (m_yakow_type == 0) {
    u32 y_val = sm64_find_symbol_value("yakow");
    if (y_val == 0 || y_val == false_val) return;
    m_yakow_type = y_val;
    lg::info("[libsm64] Yakow grab type cache ready: yakow=0x{:X}", y_val);
  }

  // We also need the active-pool symbol. Reuse m_type_cache if the actor
  // collision walker has already populated it, otherwise look it up ourselves.
  u32 active_pool_sym = m_type_cache.active_pool_sym;
  if (active_pool_sym == 0) {
    active_pool_sym = sm64_find_symbol_offset("*active-pool*");
    if (active_pool_sym == 0) return;
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
      // Log the action so we can see WHY libsm64 released — e.g.
      // 0x0080088A (ACT_THROWING), 0x10880C0 (ACT_BACKWARD_AIR_KB),
      // 0x008008A8 (ACT_PLACING_DOWN), or any drop_and_set_mario_action
      // path.  See third-party/libsm64/src/decomp/include/sm64.h for the
      // ACT_* table.
      lg::info("[libsm64] yakow grab: SM64 released fake held object — freeing yakow 0x{:X} "
               "(mario action=0x{:08X})",
               m_grabbed_yakow_ee, state.action);
      m_grabbed_yakow_ee = 0;
      return;
    }

    // Verify the held yakow is still a valid yakow process at the stored EE
    // address, with a grace window before actually releasing.  An OpenGOAL
    // process can briefly look "not a yakow" mid-transition:
    //   - deactivate moves it between pools and may overwrite type-tag bits
    //   - dead-pool reuse can leave a frame with a partially-initialized type
    //   - inter-state init code can transiently null fields
    // Releasing on the first failed frame chases ghosts; we accumulate
    // m_yakow_missing_frames and only release once the failure has held
    // for kYakowGraceFrames consecutive frames.  Reset to 0 on any
    // successful check.
    // 8 seconds at the 30Hz Mario tick rate (update_yakow_grab is gated to
    // tick frames by the OpenGLRenderer pipeline early-return).
    constexpr int kYakowGraceFrames = 240;
    using namespace ac;
    bool still_yakow = false;
    if (valid_basic_ptr(m_grabbed_yakow_ee, EE_MAIN_MEM_SIZE)) {
      u32 t = 0;
      if (read_basic_type(ee_mem, m_grabbed_yakow_ee, EE_MAIN_MEM_SIZE, t) &&
          t != m_grabbed_yakow_ee &&
          valid_basic_ptr(t, EE_MAIN_MEM_SIZE) &&
          type_is_descendant(ee_mem, EE_MAIN_MEM_SIZE, t, m_yakow_type, m_is_yakow_cache)) {
        still_yakow = true;
      }
    }
    if (still_yakow) {
      m_yakow_missing_frames = 0;
    } else {
      // Try to rebind: a level reload destroys the yakow process and
      // spawns a fresh one at a new EE address, but the entity-actor
      // record (in *entity-pool*, NOT the level heap) keeps the same
      // address.  If we captured an entity at grab time, look for any
      // currently-live yakow whose entity matches and re-bind to it.
      u32 rebound_ee = 0;
      if (m_grabbed_yakow_entity != 0) {
        for (const auto& r : yakows) {
          if (r.entity_addr == m_grabbed_yakow_entity && r.ee_addr != m_grabbed_yakow_ee) {
            rebound_ee = r.ee_addr;
            break;
          }
        }
      }
      if (rebound_ee != 0) {
        lg::info("[libsm64] yakow grab: rebinding 0x{:X} -> 0x{:X} via entity 0x{:X} "
                 "(level reload, mario action=0x{:08X})",
                 m_grabbed_yakow_ee, rebound_ee, m_grabbed_yakow_entity, state.action);
        m_grabbed_yakow_ee = rebound_ee;
        m_yakow_missing_frames = 0;
        // Fall through to the trans-write below — we now have a valid
        // yakow at the new address.
      } else {
        m_yakow_missing_frames++;
        if (m_yakow_missing_frames >= kYakowGraceFrames) {
          lg::info("[libsm64] yakow grab: held yakow 0x{:X} not a yakow for {} frames — releasing "
                   "(entity=0x{:X}, mario action=0x{:08X}, {} other yakows in active-pool)",
                   m_grabbed_yakow_ee, m_yakow_missing_frames, m_grabbed_yakow_entity,
                   state.action, yakows.size());
          {
            std::scoped_lock lock(m_sm64_lock);
            sm64_mario_end_fake_hold(m_mario_id);
          }
          m_grabbed_yakow_ee = 0;
          m_grabbed_yakow_entity = 0;
          m_yakow_missing_frames = 0;
          return;
        }
        // During the grace window, skip the trans-write below — the bytes at
        // that EE address aren't a yakow right now, so writing the trans
        // field would corrupt whatever process IS occupying the slot.
        // Throttle the log: first frame and then every second (30 ticks)
        // so a held grace window is one or two lines, not 240.
        if (m_yakow_missing_frames == 1 || (m_yakow_missing_frames % 30) == 0) {
          lg::info("[libsm64] yakow grab: held yakow 0x{:X} type-tag mismatch ({}/{}), "
                   "no entity rebind found, skipping trans-write",
                   m_grabbed_yakow_ee, m_yakow_missing_frames, kYakowGraceFrames);
        }
        return;
      }
    }
    // Bonus diagnostic: log if the held yakow is alive at its address but
    // was NOT found in the active-pool walk.  Means it's been deactivated /
    // moved between pools — we keep holding either way.  Throttled.
    bool in_active_pool = false;
    for (const auto& r : yakows) {
      if (r.ee_addr == m_grabbed_yakow_ee) {
        in_active_pool = true;
        break;
      }
    }
    if (!in_active_pool) {
      static int s_offpool_log_throttle = 0;
      if ((s_offpool_log_throttle++ % 30) == 0) {
        lg::info("[libsm64] yakow grab: held yakow 0x{:X} alive but not in *active-pool* walk "
                 "(mario action=0x{:08X}) — keeping hold",
                 m_grabbed_yakow_ee, state.action);
      }
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
      lg::warn("[libsm64] yakow grab: write_yakow_trans failed for 0x{:X} — releasing "
               "(mario action=0x{:08X})",
               m_grabbed_yakow_ee, state.action);
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
  u32 best_entity = 0;
  float best_d2 = radius_sq_jak;
  for (const auto& r : yakows) {
    float dx = r.trans_jak[0] - state.position.x();
    float dy = r.trans_jak[1] - state.position.y();
    float dz = r.trans_jak[2] - state.position.z();
    float d2 = dx * dx + dy * dy + dz * dz;
    if (d2 < best_d2) {
      best_d2 = d2;
      best_ee = r.ee_addr;
      best_entity = r.entity_addr;
    }
  }
  if (best_ee == 0) return;

  // Commit the grab.
  lg::info("[libsm64] yakow grab: grabbing yakow 0x{:X} (entity=0x{:X}) at dist {:.0f} Jak units",
           best_ee, best_entity, std::sqrt(best_d2));
  {
    std::scoped_lock lock(m_sm64_lock);
    sm64_mario_begin_fake_hold(m_mario_id);
  }
  m_grabbed_yakow_ee = best_ee;
  m_grabbed_yakow_entity = best_entity;
  m_yakow_missing_frames = 0;
}

void LibSM64Manager::clear_yakow_grab() {
  if (m_grabbed_yakow_ee != 0) {
    if (m_initialized && m_mario_id >= 0) {
      std::scoped_lock lock(m_sm64_lock);
      sm64_mario_end_fake_hold(m_mario_id);
    }
    m_grabbed_yakow_ee = 0;
  }
  m_grabbed_yakow_entity = 0;
  m_yakow_missing_frames = 0;
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

// Cell-pickup state preservation.  target_clone_anim is set by the GOAL
// side whenever Jak enters target-clone-anim (fuel-cell pickup, blue-eco
// door/bridge cutscene, etc.).  During that window the normal Mario↔Jak
// sync is skipped (OpenGLRenderer.cpp's teleport gate) so Mario stays
// where he was — but sm64_mario_tick keeps running, so gravity, action
// timeouts, and similar will drift his state.  Most visibly, he gets
// booted off a shell he was riding when the cell was grabbed.
//
// Fix: snapshot the full MarioState at the cutscene's rising edge, then
// restore position / velocity / face angle / forward velocity / action
// verbatim at the falling edge.  This puts Mario back exactly where he
// started, still on the shell (or whatever he was doing), moving in the
// same direction at the same speed.
//
// Implementation note: m_state stores values in Jak units (position /
// velocity / forward_velocity all scaled by SM64_TO_JAK_SCALE).  libsm64
// setters take SM64 units, so we multiply back by JAK_TO_SM64_SCALE on
// restore.  face_angle stays in radians on both sides.
void LibSM64Manager::update_shell_preserve_across_cell_grab() {
  if (!m_initialized || m_mario_id < 0) return;

  const bool now_clone  = target_clone_anim;
  const bool prev_clone = m_prev_target_clone_anim;

  // Only preserve Mario's state if he's actually riding a shell.
  // ACT_FLAG_RIDING_SHELL = 0x00010000 (1 << 16)
  const bool mario_on_shell = (m_state.action & 0x00010000) != 0;

  // Rolling snapshot: update the on-shell state every frame Mario is on the
  // shell and clone-anim is NOT yet active.  This fires before the rising-edge
  // check below so the snapshot is always from the last good tick before the
  // cutscene begins — even for directly-placed cells where GOAL writes the
  // clone-anim flag one frame late.
  //
  // Why "one frame late"?  mario.gc writes *sm64-target-flags*.z inside the
  // GOAL process tick, before the fuel-cell process fires.  When Jak touches a
  // free-standing cell the fuel-cell process sends 'clone-anim and Jak enters
  // target-clone-anim in the SAME GOAL frame that mario.gc already wrote 0.
  // C++ therefore reads target_clone_anim=0 on the contact frame, runs
  // sm64_mario_tick with live player input (possibly triggering shell exit),
  // and only sees the rising edge one frame later.  By this point m_state may
  // already reflect the "not on shell" action, so the old rising-edge snapshot
  // captured the wrong state.  The rolling approach sidesteps this entirely:
  // the snapshot is always from the frame BEFORE the contact frame.
  if (mario_on_shell && !now_clone) {
    std::lock_guard<std::mutex> g(m_geo_mutex);
    m_clone_anim_snapshot = m_state;
    m_clone_anim_snapshot_valid = true;
  }

  if (!now_clone && prev_clone) {
    // Falling edge — cutscene just ended.  Replay the snapshot into
    // libsm64.  We issue the position, velocity, and action writes
    // under the sm64 lock (serializes against the audio worker).
    if (m_clone_anim_snapshot_valid) {
      const auto& s = m_clone_anim_snapshot;
      const float sm64_x = s.position.x() * JAK_TO_SM64_SCALE;
      const float sm64_y = s.position.y() * JAK_TO_SM64_SCALE;
      const float sm64_z = s.position.z() * JAK_TO_SM64_SCALE;
      const float sm64_vx = s.velocity.x() * JAK_TO_SM64_SCALE;
      const float sm64_vy = s.velocity.y() * JAK_TO_SM64_SCALE;
      const float sm64_vz = s.velocity.z() * JAK_TO_SM64_SCALE;
      const float sm64_fwd = s.forward_velocity * JAK_TO_SM64_SCALE;
      {
        std::scoped_lock lock(m_sm64_lock);
        // Setting action FIRST means libsm64's action-entry hook runs
        // with the restored position/velocity in place (some actions
        // latch position on entry).  Then we apply position/velocity
        // explicitly in case the action-entry clobbered them.
        sm64_set_mario_action(m_mario_id, s.action);
        sm64_set_mario_position(m_mario_id, sm64_x, sm64_y, sm64_z);
        sm64_set_mario_velocity(m_mario_id, sm64_vx, sm64_vy, sm64_vz);
        sm64_set_mario_forward_velocity(m_mario_id, sm64_fwd);
        sm64_set_mario_faceangle(m_mario_id, s.face_angle);
      }
      lg::info("[libsm64] cell-pickup cutscene ended — restored Mario to "
               "pre-cutscene on-shell state (action=0x{:08X})", s.action);
      // Short input-zero window: SM64 still ticks (no freeze) but player
      // input is suppressed so a button held through the cutscene can't
      // accidentally exit the shell.  Camera follows Mario naturally.
      // Also save the shell action: tick() re-asserts it after each
      // sm64_mario_tick in case SM64 drops it (e.g. null riddenObj →
      // ACT_FREEFALL), keeping m_state.action shell-flagged so GOAL never
      // sees the shell-exit falling edge during the transition window.
      m_post_restore_shell_action = (s.action & 0x00010000u) ? s.action : 0u;
      m_post_restore_freeze_ticks  = kPostRestoreInputZeroTicks;
    }
    m_clone_anim_snapshot_valid = false;
  }

  // Clear the snapshot flag on genuine shell dismount — prevents
  // m_clone_anim_snapshot_valid from granting false lava immunity after
  // Mario voluntarily exits the shell outside of a cutscene.
  // Conditions:
  //   !mario_on_shell  — Mario is not currently on shell
  //   !now_clone       — we are not inside a cutscene
  //   !prev_clone      — this is NOT the falling-edge frame (on the falling
  //                      edge prev_clone=true; the restore + clear above
  //                      already ran, so we must not clobber the flag again
  //                      before the restore check — or rather, it's already
  //                      false, but the restore must be allowed to fire first)
  if (!mario_on_shell && !now_clone && !prev_clone) {
    m_clone_anim_snapshot_valid = false;
  }

  m_prev_target_clone_anim = now_clone;
}

void LibSM64Manager::update_zoomer_shell(u8* ee_mem) {
  if (!m_initialized || m_mario_id < 0 || !ee_mem) return;

  const u32 true_val = sm64_true_offset();
  const u32 false_val = s7.offset;  // FIX_SYM_FALSE is 0 on every version
  if (s7.offset == 0) return;

  // Keep *sm64-skip-zoomer* in sync with our toggle every frame — GOAL
  // reads it synchronously from the 'racing event handler, so writing it
  // each tick is the simplest way to ensure it's current. If the symbol
  // doesn't exist yet (target-handler not linked), do nothing.
  {
    const u32 desired = (zoomer_shell && has_mario()) ? true_val : false_val;
    u32 cur = sm64_find_symbol_value("*sm64-skip-zoomer*");
    if (cur != desired) {
      // Only write if the symbol exists.  set_symbol_value silently
      // no-ops when the symbol isn't defined.
      sm64_set_symbol_value("*sm64-skip-zoomer*", desired);
    }
  }

  if (!zoomer_shell) return;

  // Check whether GOAL just swallowed a 'racing event — that's our cue
  // that the player tried to hop on a zoomer. Clear the flag and put
  // Mario into shell mode.
  u32 req_val = sm64_find_symbol_value("*sm64-zoomer-requested*");
  if (req_val == 0) return;                  // GOAL bridge not linked
  if (req_val != true_val) return;           // no pending request

  // Clear first so subsequent requests (e.g. tapping attack again after
  // exiting shell) retrigger cleanly, even if we bail out below.
  sm64_set_symbol_value("*sm64-zoomer-requested*", false_val);

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
// Target-tube slide
// --------------------------------------------------------------------------
//
// When Jak rides one of the Sunken-Temple transparent tubes he enters the
// target-tube-* family:
//   target-tube-start — entering the tube
//   target-tube       — sliding through it
//   target-tube-jump  — jumping off it (still counts as "on the tube")
//   target-tube-hit   — damaged while sliding
// target-tube-death (Jak died mid-slide) is deliberately excluded so the
// slide ends when Jak dies.
//
// Mario's side mirrors this: on the rising edge we force him into
// ACT_BUTT_SLIDE (the sit-down pose used on Cool, Cool Mountain's slide
// and Princess's Secret Slide) so he visually matches Jak's slide
// posture.  A GOAL bridge symbol
// `*sm64-in-tube-slide*` tracks the current state so mario-music.gc can
// swap the track for 'slide (SM64's slide theme) while Jak is riding.
// Both are cleared on the falling edge so the previous level track and
// Mario's normal action resume.

void LibSM64Manager::update_target_tube(u8* ee_mem) {
  if (!m_initialized || m_mario_id < 0 || !ee_mem) {
    m_prev_in_tube_slide = false;
    return;
  }

  const u32 true_val = sm64_true_offset();
  const u32 false_val = s7.offset;
  if (false_val == 0) return;

  // target-tube state family is jak 1-only (no equivalent target state in
  // jak 2's state machine).  Skip the whole detection on other versions —
  // saves a few symbol-table thrashes per frame and keeps in_tube=false.
  if (g_game_version != GameVersion::Jak1) {
    m_prev_in_tube_slide = false;
    return;
  }

  // Read *target*'s state.name — same pattern as update_launcher_glue.
  u32 target_ptr = sm64_get_symbol_value("*target*");
  bool in_tube = false;
  if (target_ptr != 0 && target_ptr != false_val) {
    constexpr u32 STATE_RUNTIME_OFF = 52;  // process.state, basic-deref offset
    if (target_ptr + STATE_RUNTIME_OFF + 4 <= EE_MAIN_MEM_SIZE) {
      u32 state_ptr;
      std::memcpy(&state_ptr, ee_mem + target_ptr + STATE_RUNTIME_OFF, 4);
      if (state_ptr != 0 && state_ptr != false_val && state_ptr < EE_MAIN_MEM_SIZE) {
        // stack-frame.name is the first field — symbol at offset 0.
        if (state_ptr + 4 <= EE_MAIN_MEM_SIZE) {
          u32 state_name;
          std::memcpy(&state_name, ee_mem + state_ptr, 4);

          // Resolved lazily each tick — cheap hash-table reads; the syms
          // might not exist until sunken's DGO is loaded, in which case
          // the lookup returns 0 and the compare short-circuits to false.
          const u32 sym_tube       = sm64_get_symbol_offset("target-tube");
          const u32 sym_tube_start = sm64_get_symbol_offset("target-tube-start");
          const u32 sym_tube_jump  = sm64_get_symbol_offset("target-tube-jump");
          const u32 sym_tube_hit   = sm64_get_symbol_offset("target-tube-hit");
          in_tube =
              (sym_tube       && state_name == sym_tube)       ||
              (sym_tube_start && state_name == sym_tube_start) ||
              (sym_tube_jump  && state_name == sym_tube_jump)  ||
              (sym_tube_hit   && state_name == sym_tube_hit);
        }
      }
    }
  }

  // Push current state to the GOAL bridge symbol so update-mario-music!
  // can see it without re-reading *target*.  Returns false silently if
  // the symbol doesn't exist yet (target-handler.gc not linked).
  {
    const u32 desired = in_tube ? true_val : false_val;
    u32 cur = sm64_find_symbol_value("*sm64-in-tube-slide*");
    if (cur != desired) {
      sm64_set_symbol_value("*sm64-in-tube-slide*", desired);
    }
  }

  constexpr uint32_t kActButtSlide              = 0x00840452;  // sm64.h ACT_BUTT_SLIDE

  // Flip the libsm64-side "treat every floor as very slippery" global.
  // This mirrors the SM64 slide level treatment: mario_get_floor_class
  // returns SURFACE_CLASS_VERY_SLIPPERY, mario_floor_is_slippery /
  // _is_slope / _is_steep all follow, and Mario's native slide physics
  // handle acceleration, transitions, jump-and-land-back-in-slide, etc.
  // We don't glue Mario to Jak — they slide independently through the
  // same tube geometry, matching vanilla SM64 behaviour on slide levels
  // (Mario's speed is set by ramp angle + gravity, not by coupling to
  // another actor).
  {
    std::scoped_lock lock(m_sm64_lock);
    sm64_set_force_slide(in_tube ? 1 : 0);
  }

  // Rising edge: put Mario into ACT_BUTT_SLIDE so he starts the slide
  // immediately rather than walking onto the slope first.  Once in the
  // slide action, SM64's native logic keeps him there (jumps transition
  // to butt-slide-air and land back in butt-slide because the floor is
  // force-slippery), so per-frame re-forcing isn't needed.  Skipped if
  // he's already in a butt/stomach-slide variant.
  constexpr uint32_t kActFlagButtOrStomachSlide = 0x00400000;
  if (in_tube && !m_prev_in_tube_slide) {
    uint32_t current_action = 0;
    {
      std::lock_guard<std::mutex> g(m_geo_mutex);
      current_action = m_state.action;
    }
    if ((current_action & kActFlagButtOrStomachSlide) == 0) {
      std::scoped_lock lock(m_sm64_lock);
      sm64_set_mario_action(m_mario_id, kActButtSlide);
      lg::info("[libsm64] target-tube: Mario → ACT_BUTT_SLIDE (force_slide on)");
    }
  }

  // Falling edge: slide-force goes off via the setter above.  If Mario is
  // still coasting in a butt/stomach-slide action on what is now
  // non-slippery ground, nudge him into freefall so he stands up at the
  // bottom instead of sliding to a stop on a flat patch and looking
  // awkward.
  if (!in_tube && m_prev_in_tube_slide) {
    uint32_t current_action = 0;
    {
      std::lock_guard<std::mutex> g(m_geo_mutex);
      current_action = m_state.action;
    }
    if ((current_action & kActFlagButtOrStomachSlide) != 0) {
      constexpr uint32_t kActFreefall = 0x0100088C;  // ACT_FREEFALL
      std::scoped_lock lock(m_sm64_lock);
      sm64_set_mario_action(m_mario_id, kActFreefall);
      lg::info("[libsm64] target-tube: Mario exited slide → ACT_FREEFALL");
    }
  }

  m_prev_in_tube_slide = in_tube;
}

// --------------------------------------------------------------------------
// Target-ice skating
// --------------------------------------------------------------------------
//
// When Jak walks on the snow level's slippery ice, GOAL swaps him into
// the target-ice-* family (target-ice-stance = standing still, slipping
// imperceptibly; target-ice-walk = input-driven motion with reduced
// grip).  Mario's side mirrors this by flipping sm64_set_force_ice,
// which short-circuits mario_get_floor_class to VERY_SLIPPERY — exactly
// what Cool, Cool Mountain / Snowman's Land tags their icy patches as.
// SM64's walking-speed code then uses the low-friction branch in
// update_walking_speed / apply_slope_decel and Mario coasts.
//
// Separate from force_slide on purpose: force_ice deliberately skips
// the slide-speed scale so Mario gets vanilla SM64 ice friction rather
// than the tube's slowed-down numbers.

void LibSM64Manager::update_target_ice(u8* ee_mem) {
  if (!m_initialized || m_mario_id < 0 || !ee_mem) {
    m_prev_on_ice = false;
    {
      std::scoped_lock lock(m_sm64_lock);
      sm64_set_force_ice(0);
    }
    return;
  }

  const u32 true_val = sm64_true_offset();
  const u32 false_val = s7.offset;
  if (false_val == 0) return;

  // target-ice family is jak 1-only (snow level state machine).
  if (g_game_version != GameVersion::Jak1) {
    m_prev_on_ice = false;
    {
      std::scoped_lock lock(m_sm64_lock);
      sm64_set_force_ice(0);
    }
    return;
  }

  // Read *target*'s state.name — same pattern as update_target_tube /
  // update_launcher_glue.
  u32 target_ptr = sm64_get_symbol_value("*target*");
  bool on_ice = false;
  if (target_ptr != 0 && target_ptr != false_val) {
    constexpr u32 STATE_RUNTIME_OFF = 52;
    if (target_ptr + STATE_RUNTIME_OFF + 4 <= EE_MAIN_MEM_SIZE) {
      u32 state_ptr;
      std::memcpy(&state_ptr, ee_mem + target_ptr + STATE_RUNTIME_OFF, 4);
      if (state_ptr != 0 && state_ptr != false_val && state_ptr < EE_MAIN_MEM_SIZE) {
        if (state_ptr + 4 <= EE_MAIN_MEM_SIZE) {
          u32 state_name;
          std::memcpy(&state_name, ee_mem + state_ptr, 4);

          // Lazy lookup — symbols don't exist until target-ice.gc (snow
          // DGO) is loaded, so the symbol-offset returns 0 on other
          // levels and the short-circuit keeps on_ice=false.
          const u32 sym_ice_stance = sm64_get_symbol_offset("target-ice-stance");
          const u32 sym_ice_walk   = sm64_get_symbol_offset("target-ice-walk");
          on_ice =
              (sym_ice_stance && state_name == sym_ice_stance) ||
              (sym_ice_walk   && state_name == sym_ice_walk);
        }
      }
    }
  }

  // GOAL bridge — for any GOAL-side consumers that want to react (music
  // overrides, HUD, etc.).  C++'s own behaviour doesn't read it back.
  {
    const u32 desired = on_ice ? true_val : false_val;
    u32 cur = sm64_find_symbol_value("*sm64-on-ice*");
    if (cur != desired) {
      sm64_set_symbol_value("*sm64-on-ice*", desired);
    }
  }

  // Flip the libsm64 flag every frame.  Setter is a single int write
  // under the lock — cheap enough to not bother edge-detecting.
  {
    std::scoped_lock lock(m_sm64_lock);
    sm64_set_force_ice(on_ice ? 1 : 0);
  }

  if (on_ice && !m_prev_on_ice) {
    lg::info("[libsm64] target-ice: Mario floor-class → VERY_SLIPPERY (ice friction)");
  } else if (!on_ice && m_prev_on_ice) {
    lg::info("[libsm64] target-ice: Mario floor-class restored");
  }
  m_prev_on_ice = on_ice;
}

// --------------------------------------------------------------------------
// GOAL-state glue (launchers, warp gates, continue points)
// --------------------------------------------------------------------------
//
// Several Jak states involve GOAL controlling Jak's position entirely:
//
//   Launchers (spring pads):
//     target-launch → target-duck-high-jump → target-duck-high-jump-jump
//     target-high-jump (also used by normal high jumps — acceptable false positive)
//
//   Warp gates:
//     target-warp-in  — arriving at a warp gate destination
//     target-warp-out — departing via a warp gate
//
//   Continue points:
//     target-continue — respawning at a checkpoint after death
//
// During these states the normal Mario→Jak sync would overwrite GOAL's
// position with Mario's, breaking the trajectory / teleport.  We detect
// the state by reading process.state.name from EE memory and glue Mario
// to Jak's position until the state exits.

bool LibSM64Manager::update_launcher_glue(u8* ee_mem) {
  if (!m_initialized || m_mario_id < 0 || !ee_mem) {
    m_in_launcher = false;
    m_post_glue_settle_frames = 0;
    return false;
  }
  if (!follow_mario) {
    m_in_launcher = false;
    m_post_glue_settle_frames = 0;
    return false;
  }

  const u32 false_val = s7.offset;
  if (false_val == 0) return false;

  // Launcher / warp / continue glue is built around jak 1's target state
  // names — jak 2 has different states, so skip the whole detection.
  if (g_game_version != GameVersion::Jak1) {
    m_in_launcher = false;
    m_post_glue_settle_frames = 0;
    return false;
  }

  u32 target_ptr = sm64_get_symbol_value("*target*");
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

  // Resolve state symbols. sm64_get_symbol_offset returns 0 if the symbol
  // isn't linked yet (e.g. before the level's DGO is loaded) — the
  // short-circuits below treat that as "no match".

  // Launcher states
  const u32 sym_launch    = sm64_get_symbol_offset("target-launch");
  const u32 sym_high_jump = sm64_get_symbol_offset("target-high-jump");
  const u32 sym_duck_hj   = sm64_get_symbol_offset("target-duck-high-jump");
  const u32 sym_duck_hj_j = sm64_get_symbol_offset("target-duck-high-jump-jump");

  // Warp gate states
  const u32 sym_warp_in   = sm64_get_symbol_offset("target-warp-in");
  const u32 sym_warp_out  = sm64_get_symbol_offset("target-warp-out");

  // Continue point state
  const u32 sym_continue  = sm64_get_symbol_offset("target-continue");

  const bool jak_in_glue_state =
      (sym_launch    && state_name == sym_launch)    ||
      (sym_high_jump && state_name == sym_high_jump) ||
      (sym_duck_hj   && state_name == sym_duck_hj)   ||
      (sym_duck_hj_j && state_name == sym_duck_hj_j) ||
      (sym_warp_in   && state_name == sym_warp_in)   ||
      (sym_warp_out  && state_name == sym_warp_out)  ||
      (sym_continue  && state_name == sym_continue);

  if (!jak_in_glue_state) {
    if (m_in_launcher) {
      // The GOAL glue state just ended.  Pick the settle duration based
      // on the kind of glue — continue points stay in the same level so
      // no collision reload is needed, and the long 30-frame pin shows
      // as "Mario awkwardly matches Jak's movement" for about a second.
      // Warp gates / launchers might cross levels, so those keep the
      // full duration.
      const int settle = (m_last_glue_kind == GlueKind::Continue)
                             ? POST_CONTINUE_SETTLE_DURATION
                             : POST_GLUE_SETTLE_DURATION;
      lg::info("[libsm64] Glue state ended ({}) — starting {} frame settle",
               m_last_glue_kind == GlueKind::Continue ? "continue" : "other",
               settle);
      m_in_launcher = false;
      m_post_glue_settle_frames = settle;
    }

    // During post-glue settle: keep reading Jak's position so the
    // position override in tick() can pin Mario there. m_in_launcher
    // stays false — tick() checks m_post_glue_settle_frames separately.
    if (m_post_glue_settle_frames > 0) {
      m_post_glue_settle_frames--;
      math::Vector3f jak_pos;
      if (read_target_transform(ee_mem, &jak_pos, nullptr)) {
        m_launcher_target_jak = jak_pos;
      }
      if (m_post_glue_settle_frames == 0) {
        lg::info("[libsm64] Post-glue settle complete — resuming normal sync");
      }
      return true;  // caller should skip sync_jak_to_mario during settle
    }

    return false;
  }

  // Still in a GOAL glue state — reset any running settle timer.
  m_post_glue_settle_frames = 0;

  // Classify this glue so the falling edge picks the right settle.
  // Only `target-continue` needs the short settle; the rest (warps,
  // launch pads, high-jumps) stay on the conservative default.
  m_last_glue_kind = (sym_continue && state_name == sym_continue)
                         ? GlueKind::Continue
                         : GlueKind::Other;

  // Jak is in a glue state — read Jak's position and store it.
  // The actual Mario position override happens inside tick() after
  // sm64_mario_tick, within the existing sm64_lock scope.
  if (!m_in_launcher) {
    lg::info("[libsm64] Glue state detected ({}) — syncing Mario to Jak",
             m_last_glue_kind == GlueKind::Continue ? "continue" : "other");
  }
  m_in_launcher = true;

  math::Vector3f jak_pos;
  if (read_target_transform(ee_mem, &jak_pos, nullptr)) {
    m_launcher_target_jak = jak_pos;
  }

  return true;  // caller should skip sync_jak_to_mario
}

}  // namespace sm64
