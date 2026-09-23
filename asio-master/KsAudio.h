#pragma once

// KsAudio — WASAPI Exclusive to Real HW for 10.
// Vocabulary: Master Clock, Slot.

#include <cstdint>
#include <windows.h>
#include <audioclient.h>

namespace wha {

class KsAudio {
 public:
  enum class SampleFormat { Float32, Pcm24In32, Pcm16 };  // exclusive format negotiated by open()

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
  SampleFormat format() const { return format_; }
  // Why the last open() failed (S_OK / "" after success).
  HRESULT lastError() const { return lastError_; }
  const char* lastStep() const { return lastStep_; }

 private:
  bool fail(const char* step, HRESULT hr);


  IAudioClient* audioClient_ = nullptr;
  IAudioRenderClient* renderClient_ = nullptr;
  IAudioClock* audioClock_ = nullptr;
  int32_t sampleRate_ = 48000;
  int32_t bufferFrames_ = 64;
  bool opened_ = false;
  bool exclusive_ = false;
  bool comInitialized_ = false;
  SampleFormat format_ = SampleFormat::Float32;
  HRESULT lastError_ = S_OK;
  const char* lastStep_ = "";
};

}  // namespace wha
