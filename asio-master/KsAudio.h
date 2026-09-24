#pragma once

// KsAudio — WASAPI Exclusive to Real HW for 10.
// Vocabulary: Master Clock, Slot.

#include <atomic>
#include <cstdint>
#include <string>
#include <windows.h>
#include <audioclient.h>

#include "KsEndpoint.h"

namespace wha {

class KsAudio {
 public:
  using SampleFormat = KsSampleFormat;  // exclusive format negotiated by open()

  KsAudio();
  ~KsAudio();

  // Caller must CoInitializeEx before open() if COM not already initialized.
  // open() does not CoUninitialize while IAudioClient is held.
  // bufferFrames = device period (0: device minimum); blockFrames = frames per write(), so the
  // device buffer holds at least 4 blocks. endpointId: IMMDevice ID, empty = Windows default render.
  bool open(int32_t sampleRate, int32_t bufferFrames, int32_t blockFrames = 0, const char* endpointId = nullptr);
  bool isExclusive() const { return exclusive_; }
  void close();
  bool start();
  void stop();
  bool write(const float* data, int frames, int channels);  // planar: channel c at data[c * frames]
  bool writeInterleaved(const float* data, int frames, int channels);  // frame f at data[f * channels]
  double latencyMs() const;
  bool opened() const { return opened_; }
  SampleFormat format() const { return format_; }
  // After open(): the device period actually used (frames) and the endpoint ID opened.
  int32_t periodFrames() const { return bufferFrames_; }
  const std::string& endpointId() const { return endpointId_; }
  // Why the last open() failed (S_OK / "" after success).
  HRESULT lastError() const { return lastError_; }
  const char* lastStep() const { return lastStep_; }

  // Hardware Master Clock: frames queued in the device and not yet played (-1: device gone).
  // Called from the clock thread while the Worker may be in write(): WASAPI clients are not safe for
  // concurrent calls (overlapping GetCurrentPadding and GetBuffer/ReleaseBuffer crashed in AUDIOKSE
  // when a tick overran), so every call on the client takes clientLock_.
  // written: optional, frames written so far, read under the same lock (a consistent pair).
  long padding(int64_t* written = nullptr) const;
  int32_t capacity() const { return capacityFrames_; }
  // HW Master Clock: tick once the device has drained to this fill (2 blocks of room above it).
  // start() prefills silence up to it, so streaming begins at the steady-state fill.
  int32_t targetFill() const { return capacityFrames_ - 2 * blockFrames_; }
  // Device stream latency (IAudioClient::GetStreamLatency), frames; valid after open().
  int32_t streamLatency() const { return streamLatencyFrames_; }

  // Counters for WHAMasterStats (any thread).
  uint64_t writes() const { return writes_.load(); }
  // Frames queued to the device since start(), prefill included: minus padding() = frames it consumed.
  int64_t framesWritten() const { return framesWritten_.load(); }
  uint64_t underruns() const { return underruns_.load(); }
  uint64_t drops() const { return drops_.load(); }
  int32_t minFill() const { return minFill_.load(); }
  int32_t maxFill() const { return maxFill_.load(); }

 private:
  bool fail(const char* step, HRESULT hr);
  bool writeFrames(const float* data, int frames, int channels, bool interleaved);

  mutable SRWLOCK clientLock_ = SRWLOCK_INIT;  // guards audioClient_/renderClient_ calls
  IAudioClient* audioClient_ = nullptr;
  IAudioRenderClient* renderClient_ = nullptr;
  IAudioClock* audioClock_ = nullptr;
  int32_t sampleRate_ = 48000;
  int32_t bufferFrames_ = 64;
  int32_t capacityFrames_ = 0;
  int32_t blockFrames_ = 0;
  int32_t streamLatencyFrames_ = 0;
  bool opened_ = false;
  bool exclusive_ = false;
  bool comInitialized_ = false;
  SampleFormat format_ = SampleFormat::Float32;
  std::string endpointId_;
  HRESULT lastError_ = S_OK;
  const char* lastStep_ = "";
  std::atomic<uint64_t> writes_{0};
  std::atomic<int64_t> framesWritten_{0};
  std::atomic<uint64_t> underruns_{0};
  std::atomic<uint64_t> drops_{0};
  std::atomic<int32_t> minFill_{-1};
  std::atomic<int32_t> maxFill_{0};
};

}  // namespace wha
