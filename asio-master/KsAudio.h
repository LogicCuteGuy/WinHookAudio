#pragma once

// KsAudio — WASAPI Exclusive to Real HW for 10.
// Vocabulary: Master Clock, Slot.

#include <cstdint>
#include <windows.h>
#include <audioclient.h>

namespace wha {

class KsAudio {
 public:
  KsAudio();
  ~KsAudio();

  // Caller must CoInitializeEx before open() if COM not already initialized.
  // open() does not CoUninitialize while IAudioClient is held.
  bool open(int32_t sampleRate, int32_t bufferFrames);
  bool isExclusive() const { return exclusive_; }
  void close();
  bool start();
  void stop();
  bool write(const float* data, int frames, int channels);
  double latencyMs() const;
  bool opened() const { return opened_; }

 private:
  IAudioClient* audioClient_ = nullptr;
  IAudioRenderClient* renderClient_ = nullptr;
  IAudioClock* audioClock_ = nullptr;
  int32_t sampleRate_ = 48000;
  int32_t bufferFrames_ = 64;
  bool opened_ = false;
  bool exclusive_ = false;
  bool comInitialized_ = false;
};

}  // namespace wha
