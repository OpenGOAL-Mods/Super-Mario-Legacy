#pragma once

/*!
 * @file sm64_audio.h
 * cubeb-backed audio player for libsm64. Runs on its own dedicated worker
 * thread (driven by the cubeb callback) at SM64's native 32 kHz stereo rate,
 * pulling PCM from sm64_audio_tick(). Serialized against sm64_mario_tick()
 * via an externally supplied mutex (libsm64's global state is not reentrant).
 */

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

#include "third-party/cubeb/cubeb/include/cubeb/cubeb.h"

namespace sm64 {

// SM64's audio engine generates samples at 32000 Hz stereo.
static constexpr uint32_t SM64_AUDIO_SAMPLE_RATE = 32000;
// Worst-case per-tick output from sm64_audio_tick: 2 * SAMPLES_HIGH (544) * 2ch.
static constexpr uint32_t SM64_AUDIO_MAX_FRAMES_PER_TICK = 2 * 544;

class SM64AudioPlayer {
 public:
  // `sm64_lock` must outlive the player and is shared with the Mario tick so
  // we never call sm64_audio_tick() concurrently with sm64_mario_tick().
  explicit SM64AudioPlayer(std::mutex& sm64_lock);
  ~SM64AudioPlayer();

  SM64AudioPlayer(const SM64AudioPlayer&) = delete;
  SM64AudioPlayer& operator=(const SM64AudioPlayer&) = delete;

  // Start the cubeb stream. Safe to call only after sm64_audio_init() has run.
  // Returns false if cubeb setup failed (in which case audio is simply off).
  bool start();
  // Stop and tear down the stream. Idempotent.
  void stop();

  bool is_running() const { return m_running; }

  // Volume 0..100, applied to int16 samples in the cubeb callback. Atomic so
  // the UI thread can slide it while the worker thread is mixing.
  void set_volume(int volume) {
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    m_volume.store(volume, std::memory_order_relaxed);
  }
  int get_volume() const { return m_volume.load(std::memory_order_relaxed); }

 private:
  static long sound_callback(cubeb_stream* stream,
                             void* user,
                             const void* input,
                             void* output_buffer,
                             long nframes);
  static void state_callback(cubeb_stream* stream, void* user, cubeb_state state);

  // Drain+refill the ring buffer and copy `nframes` stereo s16 frames into
  // `out`. Called from the cubeb worker thread.
  void fill(int16_t* out, long nframes);

  std::mutex& m_sm64_lock;  // shared with mario tick

  cubeb* m_ctx = nullptr;
  cubeb_stream* m_stream = nullptr;
  std::atomic<bool> m_running{false};
  std::atomic<int> m_volume{100};  // 0..100

#ifdef _WIN32
  bool m_coinitialized = false;
#endif

  // Interleaved int16 stereo ring buffer. Written by fill() (via
  // sm64_audio_tick) and drained into the cubeb output in the same call.
  // Kept as a simple vector + read/write cursor since there is exactly one
  // reader (the cubeb worker thread) and no other producer.
  std::vector<int16_t> m_ring;  // size = capacity * 2 (stereo)
  size_t m_ring_read = 0;       // in frames
  size_t m_ring_write = 0;      // in frames
  size_t m_ring_capacity = 0;   // in frames
};

}  // namespace sm64
