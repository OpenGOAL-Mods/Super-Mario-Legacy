/*!
 * @file sm64_audio.cpp
 * Cubeb-backed audio player for libsm64.
 *
 * SM64's audio engine runs at 32 kHz stereo. We open a cubeb output stream at
 * that native rate and pull PCM inside the cubeb worker callback by calling
 * sm64_audio_tick() — which itself advances the N64 audio engine, mixes the
 * sequences and samples, and writes 16-bit interleaved PCM into a scratch
 * buffer. We then feed that scratch buffer through a small ring so cubeb can
 * ask for any `nframes` size without us having to perfectly match the
 * 528/544-frame chunks libsm64 emits per tick.
 *
 * sm64_audio_tick() and sm64_mario_tick() both touch libsm64's global state,
 * so the caller gives us a mutex that is also held by the main-thread mario
 * tick; we grab it whenever we call sm64_audio_tick().
 */

#include "sm64_audio.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <combaseapi.h>
#include <windows.h>
#endif

#include <algorithm>
#include <cstring>

#include "common/log/log.h"

extern "C" {
#include "libsm64.h"
}

namespace sm64 {

// Ring capacity in frames: enough to buffer a handful of libsm64 ticks so we
// never underrun even if cubeb requests a small nframes and the sm64 tick
// briefly blocks on the mario lock. 8192 frames = 256 ms at 32 kHz.
static constexpr size_t kRingCapacityFrames = 8192;

SM64AudioPlayer::SM64AudioPlayer(std::mutex& sm64_lock) : m_sm64_lock(sm64_lock) {
  m_ring_capacity = kRingCapacityFrames;
  m_ring.assign(m_ring_capacity * 2, 0);
}

SM64AudioPlayer::~SM64AudioPlayer() {
  stop();
}

bool SM64AudioPlayer::start() {
  if (m_running) return true;

#ifdef _WIN32
  HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  m_coinitialized = SUCCEEDED(hr);
  if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
    lg::error("[libsm64-audio] CoInitializeEx failed");
    return false;
  }
#endif

  if (cubeb_init(&m_ctx, "OpenGOAL-SM64", nullptr) != CUBEB_OK) {
    lg::error("[libsm64-audio] cubeb_init failed");
    return false;
  }

  cubeb_stream_params outparam = {};
  outparam.channels = 2;
  outparam.format = CUBEB_SAMPLE_S16LE;
  outparam.rate = SM64_AUDIO_SAMPLE_RATE;
  outparam.layout = CUBEB_LAYOUT_STEREO;
  outparam.prefs = CUBEB_STREAM_PREF_NONE;

  uint32_t latency = 0;
  if (cubeb_get_min_latency(m_ctx, &outparam, &latency) != CUBEB_OK) {
    lg::error("[libsm64-audio] cubeb_get_min_latency failed");
    cubeb_destroy(m_ctx);
    m_ctx = nullptr;
    return false;
  }
  // Clamp up to at least ~11 ms so the callback doesn't fire more often than
  // libsm64 expects; it generates one "audio tick" of 528/544 frames which is
  // roughly 16 ms at 32 kHz.
  if (latency < 512) latency = 512;

  int err = cubeb_stream_init(m_ctx, &m_stream, "OpenGOAL-SM64",
                              nullptr, nullptr,
                              nullptr, &outparam,
                              latency,
                              &sound_callback, &state_callback, this);
  if (err != CUBEB_OK) {
    lg::error("[libsm64-audio] cubeb_stream_init failed: {}", err);
    cubeb_destroy(m_ctx);
    m_ctx = nullptr;
    return false;
  }

  if (cubeb_stream_start(m_stream) != CUBEB_OK) {
    lg::error("[libsm64-audio] cubeb_stream_start failed");
    cubeb_stream_destroy(m_stream);
    m_stream = nullptr;
    cubeb_destroy(m_ctx);
    m_ctx = nullptr;
    return false;
  }

  m_running = true;
  lg::info("[libsm64-audio] Audio stream started ({} Hz, latency={} frames)",
           SM64_AUDIO_SAMPLE_RATE, latency);
  return true;
}

void SM64AudioPlayer::stop() {
  if (!m_running && !m_stream && !m_ctx) {
#ifdef _WIN32
    if (m_coinitialized) {
      CoUninitialize();
      m_coinitialized = false;
    }
#endif
    return;
  }
  if (m_stream) {
    cubeb_stream_stop(m_stream);
    cubeb_stream_destroy(m_stream);
    m_stream = nullptr;
  }
  if (m_ctx) {
    cubeb_destroy(m_ctx);
    m_ctx = nullptr;
  }
  m_running = false;
  m_ring_read = 0;
  m_ring_write = 0;
#ifdef _WIN32
  if (m_coinitialized) {
    CoUninitialize();
    m_coinitialized = false;
  }
#endif
  lg::info("[libsm64-audio] Audio stream stopped");
}

long SM64AudioPlayer::sound_callback([[maybe_unused]] cubeb_stream* stream,
                                     void* user,
                                     [[maybe_unused]] const void* input,
                                     void* output_buffer,
                                     long nframes) {
  auto* self = static_cast<SM64AudioPlayer*>(user);
  self->fill(static_cast<int16_t*>(output_buffer), nframes);
  return nframes;
}

void SM64AudioPlayer::state_callback([[maybe_unused]] cubeb_stream* stream,
                                     [[maybe_unused]] void* user,
                                     [[maybe_unused]] cubeb_state state) {}

void SM64AudioPlayer::fill(int16_t* out, long nframes) {
  long frames_written = 0;
  const int volume = m_volume.load(std::memory_order_relaxed);

  auto available = [&]() -> size_t {
    return (m_ring_write + m_ring_capacity - m_ring_read) % m_ring_capacity;
  };

  while (frames_written < nframes) {
    size_t avail = available();
    if (avail == 0) {
      // Ring is empty — ask libsm64 for more audio. We pass the number of
      // "queued samples" so libsm64 knows whether to pick SAMPLES_LOW (528)
      // or SAMPLES_HIGH (544) per sub-buffer. `numDesiredSamples` is our
      // target high-water-mark.
      int16_t scratch[SM64_AUDIO_MAX_FRAMES_PER_TICK * 2];
      uint32_t num_samples = 0;
      {
        std::scoped_lock lock(m_sm64_lock);
        num_samples = sm64_audio_tick(0, 1100, scratch);
      }
      if (num_samples == 0) {
        // Audio engine not ready or returned nothing — output silence for the
        // rest of the buffer so cubeb doesn't stall.
        std::memset(out + frames_written * 2, 0,
                    static_cast<size_t>(nframes - frames_written) * 2 * sizeof(int16_t));
        return;
      }
      // sm64_audio_tick emits 2 sub-buffers of `num_samples` stereo frames.
      const size_t frames_produced = static_cast<size_t>(num_samples) * 2;
      for (size_t i = 0; i < frames_produced; i++) {
        // If the ring is about to overrun (shouldn't, cubeb just drained it)
        // drop the oldest frame to stay current.
        size_t next_write = (m_ring_write + 1) % m_ring_capacity;
        if (next_write == m_ring_read) {
          m_ring_read = (m_ring_read + 1) % m_ring_capacity;
        }
        m_ring[m_ring_write * 2 + 0] = scratch[i * 2 + 0];
        m_ring[m_ring_write * 2 + 1] = scratch[i * 2 + 1];
        m_ring_write = next_write;
      }
      continue;
    }

    // Copy up to min(nframes - frames_written, avail, contiguous-to-end).
    size_t want = static_cast<size_t>(nframes - frames_written);
    size_t take = std::min(want, avail);
    // Contiguous span until we wrap around the ring.
    size_t contig = m_ring_capacity - m_ring_read;
    if (take > contig) take = contig;

    int16_t* dst = out + frames_written * 2;
    const int16_t* src = m_ring.data() + m_ring_read * 2;
    const size_t samples = take * 2;
    if (volume >= 100) {
      std::memcpy(dst, src, samples * sizeof(int16_t));
    } else if (volume <= 0) {
      std::memset(dst, 0, samples * sizeof(int16_t));
    } else {
      for (size_t i = 0; i < samples; i++) {
        dst[i] = static_cast<int16_t>((static_cast<int32_t>(src[i]) * volume) / 100);
      }
    }
    m_ring_read = (m_ring_read + take) % m_ring_capacity;
    frames_written += static_cast<long>(take);
  }
}

}  // namespace sm64
