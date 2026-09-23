#pragma once

// KsCapture — WASAPI Exclusive capture from real HW into HW IN slots.
// Vocabulary: Master Clock, Slot, Worker.
//
// The Worker pulls one block per Master Clock tick: device packets, with their capture timestamps,
// go into an HwInputFifo, which holds the backlog at its target and resamples when the device clock
// differs from the Master Clock (see HwInputFifo.h).

#include <atomic>
#include <cstdint>
#include <vector>
#include <windows.h>
#include <audioclient.h>

#include "HwInputFifo.h"
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
  // Worker thread: the Master Clock ticked without a read; drop those frames (see HwInputFifo::skip).
  void skip(size_t frames) {
    if (!opened_) return;
    pull();
    fifo_.skip(frames);
  }

  KsSampleFormat format() const { return format_; }
  HRESULT lastError() const { return lastError_; }
  const char* lastStep() const { return lastStep_; }
  int32_t streamLatency() const { return streamLatencyFrames_; }
  // Frames from capture to the end of the FIFO: the backlog target plus the resampler's delay.
  int32_t latency() const { return fifo_.expectedFill() + HwInputFifo::resamplerDelay(); }
  // Worker thread: true once after the backlog target grew (the DAW must re-query latencies).
  bool takeLatencyChanged() { return fifo_.takeLatencyChanged(); }
  const HwInputFifo& fifo() const { return fifo_; }  // counters (any thread)
  uint64_t glitches() const { return glitches_.load(); }  // device-flagged discontinuities

 private:
  bool fail(const char* step, HRESULT hr);
  void pull();  // every available device packet into the FIFO

  IAudioClient* audioClient_ = nullptr;
  IAudioCaptureClient* captureClient_ = nullptr;
  KsSampleFormat format_ = KsSampleFormat::Float32;
  bool opened_ = false;
  bool comInitialized_ = false;
  HRESULT lastError_ = S_OK;
  const char* lastStep_ = "";
  int32_t streamLatencyFrames_ = 0;
  double rate_ = 48000.0;
  double qpcFreq_ = 1.0;
  HwInputFifo fifo_;
  std::vector<float> packet_;  // interleaved device frames (Worker thread)
  std::atomic<uint64_t> glitches_{0};
};

}  // namespace wha
