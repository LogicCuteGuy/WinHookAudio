#pragma once

// KsCapture — WASAPI Exclusive capture from real HW into HW IN slots.
// Vocabulary: Master Clock, Slot, Worker.
//
// The Worker pulls one block per Master Clock tick. Device packets land in a FIFO; reads start once
// the FIFO holds the priming target, so capture jitter never starves a tick. The capture device runs
// on its own clock: the FIFO is kept within bounds (starve -> re-prime, overflow -> trim back to the
// target), counted, not yet resampled (clock adaptation is stage 3).

#include <atomic>
#include <cstdint>
#include <vector>
#include <windows.h>
#include <audioclient.h>

#include "KsEndpoint.h"

namespace wha {

class KsCapture {
 public:
  KsCapture();
  ~KsCapture();

  // Same rules as KsAudio::open; endpointId empty = Windows default capture device.
  bool open(int32_t sampleRate, int32_t periodFrames, int32_t blockFrames, const char* endpointId = nullptr);
  void close();
  bool start();
  void stop();
  bool opened() const { return opened_; }

  // Worker thread: fill `frames` of planar `data` (channel c at data[c * frames]) for `channels`
  // channels from device channel c. Silence while priming or starved. Returns true if real audio.
  bool read(float* data, int frames, int channels);

  KsSampleFormat format() const { return format_; }
  HRESULT lastError() const { return lastError_; }
  const char* lastStep() const { return lastStep_; }
  // FIFO priming target (frames left after a read at the start of the stream).
  int32_t targetFill() const { return primeFrames_; }
  // Mean FIFO fill at a read: reads start at the top of a sawtooth (target + block, right after a
  // packet), then fall a block per tick until the next device packet of `period` frames arrives.
  int32_t expectedFill() const {
    const int32_t top = primeFrames_ + blockFrames_;
    return periodFrames_ > blockFrames_ ? top - (periodFrames_ - blockFrames_) / 2 : top;
  }
  int32_t streamLatency() const { return streamLatencyFrames_; }

  // Counters for WHAMasterStats (any thread).
  uint64_t reads() const { return reads_.load(); }
  uint64_t starved() const { return starved_.load(); }
  uint64_t trims() const { return trims_.load(); }
  uint64_t glitches() const { return glitches_.load(); }  // device-flagged discontinuities
  int32_t fill() const { return fill_.load(); }
  // Mean FIFO frames at a read (before the block is taken), -1 before the first read.
  int32_t meanFill() const {
    const uint64_t n = reads_.load();
    return n ? static_cast<int32_t>(fillSum_.load() / n) : -1;
  }

 private:
  bool fail(const char* step, HRESULT hr);
  void pull();  // drain every available device packet into the FIFO

  IAudioClient* audioClient_ = nullptr;
  IAudioCaptureClient* captureClient_ = nullptr;
  KsSampleFormat format_ = KsSampleFormat::Float32;
  bool opened_ = false;
  bool comInitialized_ = false;
  HRESULT lastError_ = S_OK;
  const char* lastStep_ = "";
  int32_t streamLatencyFrames_ = 0;
  int32_t primeFrames_ = 0;
  int32_t blockFrames_ = 0;
  int32_t periodFrames_ = 0;

  // FIFO of interleaved device-channel frames (Worker thread only).
  std::vector<float> fifo_;
  size_t fifoFrames_ = 0;  // capacity
  size_t readPos_ = 0;     // frame index
  size_t count_ = 0;       // frames queued
  bool primed_ = false;

  std::atomic<uint64_t> reads_{0};
  std::atomic<uint64_t> starved_{0};
  std::atomic<uint64_t> trims_{0};
  std::atomic<uint64_t> glitches_{0};
  std::atomic<int32_t> fill_{0};
  std::atomic<uint64_t> fillSum_{0};
};

}  // namespace wha
