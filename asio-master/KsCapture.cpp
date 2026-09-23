#include "KsCapture.h"

#include <cstdio>
#include <cstring>

namespace wha {

KsCapture::KsCapture() = default;
KsCapture::~KsCapture() { close(); }

bool KsCapture::fail(const char* step, HRESULT hr) {
  lastError_ = hr;
  lastStep_ = step;
  char msg[160];
  std::snprintf(msg, sizeof(msg), "WinHookAudio KsCapture: %s failed 0x%08lX\n", step, static_cast<unsigned long>(hr));
  OutputDebugStringA(msg);
  close();
  return false;
}

bool KsCapture::open(int32_t sampleRate, int32_t periodFrames, int32_t blockFrames, const char* endpointId) {
  close();
  lastError_ = S_OK;
  lastStep_ = "";
  reads_ = 0;
  starved_ = 0;
  trims_ = 0;
  glitches_ = 0;
  fill_ = 0;
  fillSum_ = 0;

  comInitialized_ = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
  KsOpenResult r;
  if (!KsOpenExclusive(eCapture, endpointId, sampleRate, periodFrames, blockFrames, r)) return fail(r.step, r.error);
  audioClient_ = r.client;
  format_ = r.format;
  streamLatencyFrames_ = r.streamLatencyFrames;
  // Enough queued that a block is always there although packets arrive one device period at a time.
  const int32_t block = blockFrames > 0 ? blockFrames : r.periodFrames;
  blockFrames_ = block;
  periodFrames_ = r.periodFrames;
  primeFrames_ = 2 * block + r.periodFrames;
  fifoFrames_ = static_cast<size_t>(r.capacityFrames + 8 * block);
  fifo_.assign(fifoFrames_ * kKsDeviceChannels, 0.0f);
  readPos_ = count_ = 0;
  primed_ = false;

  const HRESULT hr = audioClient_->GetService(__uuidof(IAudioCaptureClient), (void**)&captureClient_);
  if (FAILED(hr)) return fail("GetService IAudioCaptureClient", hr);
  opened_ = true;
  return true;
}

void KsCapture::close() {
  if (captureClient_) { captureClient_->Release(); captureClient_ = nullptr; }
  if (audioClient_) { audioClient_->Stop(); audioClient_->Release(); audioClient_ = nullptr; }
  opened_ = false;
  if (comInitialized_) { CoUninitialize(); comInitialized_ = false; }
}

bool KsCapture::start() { return audioClient_ && SUCCEEDED(audioClient_->Start()); }
void KsCapture::stop() {
  if (audioClient_) audioClient_->Stop();
}

void KsCapture::pull() {
  UINT32 packet = 0;
  while (SUCCEEDED(captureClient_->GetNextPacketSize(&packet)) && packet > 0) {
    BYTE* data = nullptr;
    UINT32 frames = 0;
    DWORD flags = 0;
    if (FAILED(captureClient_->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) return;
    if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) glitches_.fetch_add(1);
    const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
    bool overflowed = false;
    for (UINT32 f = 0; f < frames; ++f) {
      if (count_ == fifoFrames_) {  // full: the oldest frame goes
        readPos_ = (readPos_ + 1) % fifoFrames_;
        --count_;
        overflowed = true;
      }
      const size_t w = (readPos_ + count_) % fifoFrames_;
      for (int ch = 0; ch < kKsDeviceChannels; ++ch)
        fifo_[w * kKsDeviceChannels + ch] = silent ? 0.0f : KsFromDevice(format_, data, static_cast<int>(f) * kKsDeviceChannels + ch);
      ++count_;
    }
    captureClient_->ReleaseBuffer(frames);
    if (overflowed) trims_.fetch_add(1);
  }
}

bool KsCapture::read(float* data, int frames, int channels) {
  std::memset(data, 0, sizeof(float) * static_cast<size_t>(frames) * static_cast<size_t>(channels));
  if (!opened_) return false;
  pull();
  const size_t need = static_cast<size_t>(frames);
  if (!primed_ && count_ >= static_cast<size_t>(primeFrames_) + need) {
    // Start exactly at the target: packets arrive a device period at a time, and the overshoot would
    // otherwise become a run-to-run latency difference of up to one period.
    const size_t excess = count_ - (static_cast<size_t>(primeFrames_) + need);
    readPos_ = (readPos_ + excess) % fifoFrames_;
    count_ -= excess;
    primed_ = true;
  }
  // The device clock runs ahead of the Master Clock: trim back to the target, one discontinuity.
  if (primed_ && count_ > static_cast<size_t>(primeFrames_) + 5 * need) {
    const size_t drop = count_ - (static_cast<size_t>(primeFrames_) + need);
    readPos_ = (readPos_ + drop) % fifoFrames_;
    count_ -= drop;
    trims_.fetch_add(1);
  }
  fill_ = static_cast<int32_t>(count_);
  if (!primed_) return false;
  if (count_ < need) {  // behind the Master Clock: silence, then re-prime
    starved_.fetch_add(1);
    primed_ = false;
    return false;
  }
  for (size_t f = 0; f < need; ++f) {
    const size_t r = (readPos_ + f) % fifoFrames_;
    for (int ch = 0; ch < channels && ch < kKsDeviceChannels; ++ch)
      data[static_cast<size_t>(ch) * need + f] = fifo_[r * kKsDeviceChannels + ch];
  }
  fillSum_.fetch_add(count_);
  readPos_ = (readPos_ + need) % fifoFrames_;
  count_ -= need;
  reads_.fetch_add(1);
  return true;
}

}  // namespace wha
