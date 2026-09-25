#pragma once

// KsCapture — WASAPI capture (Exclusive or Shared, WHAHwMode) from real HW into HW IN slots.
// Vocabulary: Master Clock, Slot, Worker.
//
// The Worker pulls one block per Master Clock tick: device packets, with their capture timestamps,
// go into an HwInputFifo, which holds the backlog at its target and resamples when the device clock
// differs from the Master Clock (see HwInputFifo.h).

#include <atomic>
#include <cstdint>
#include <string>
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
  bool open(int32_t sampleRate, int32_t periodFrames, int32_t blockFrames, const char* endpointId = nullptr,
            uint8_t mode = HW_MODE_EXCLUSIVE);
  void close();
  bool start();
  void stop();
  bool opened() const { return opened_; }
  bool isShared() const { return opened_ && shared_; }  // through the Windows mixer

  // Worker thread: fill `frames` of planar `data` (channel c at data[c * frames]) for `channels`
  // channels from device channel c. Silence while priming or starved. Returns true if real audio.
  bool read(float* data, int frames, int channels);
  // Worker thread, while the Master Clock settles: drop the device's packets and fill silence. A HW
  // output that paces the clock may take frames far faster than real time at start (VMware HD Audio
  // ~1.5x for half a second); reading this device at that pace starved its FIFO and raised its target
  // for the whole session. restart() then primes the FIFO afresh.
  void hold(float* data, int frames, int channels);
  void restart() {
    if (opened_) fifo_.reset(rate_, channels_, block_, fifoPeriod_);
  }
  // Worker thread: the Master Clock ticked without a read; drop those frames (see HwInputFifo::skip).
  void skip(size_t frames) {
    if (!opened_) return;
    pull();
    fifo_.skip(frames);
  }

  KsSampleFormat format() const { return format_; }
  int channels() const { return channels_; }                   // after open(): all it has, or 2
  int32_t periodFrames() const { return periodFrames_; }       // after open(): actual device period
  const std::string& endpointId() const { return endpointId_; }  // after open(): endpoint opened
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
  int channels_ = 2;
  bool opened_ = false;
  bool shared_ = false;
  bool comInitialized_ = false;
  HRESULT lastError_ = S_OK;
  const char* lastStep_ = "";
  int32_t streamLatencyFrames_ = 0;
  int32_t periodFrames_ = 0;
  std::string endpointId_;
  double rate_ = 48000.0;
  int block_ = 0;       // FIFO parameters, for restart()
  int fifoPeriod_ = 0;
  double qpcFreq_ = 1.0;
  HwInputFifo fifo_;
  std::vector<float> packet_;  // interleaved device frames (Worker thread)
  std::atomic<uint64_t> glitches_{0};
};

}  // namespace wha
