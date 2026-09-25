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

bool KsCapture::open(int32_t sampleRate, int32_t periodFrames, int32_t blockFrames, const char* endpointId, uint8_t mode) {
  close();
  lastError_ = S_OK;
  lastStep_ = "";
  glitches_ = 0;
  LARGE_INTEGER f;
  QueryPerformanceFrequency(&f);
  qpcFreq_ = static_cast<double>(f.QuadPart);
  rate_ = sampleRate;

  comInitialized_ = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
  KsOpenResult r;
  if (!KsOpen(eCapture, endpointId, mode, sampleRate, periodFrames, blockFrames, r)) return fail(r.step, r.error);
  audioClient_ = r.client;
  shared_ = r.shared;
  format_ = r.format;
  channels_ = r.channels;
  streamLatencyFrames_ = r.streamLatencyFrames;
  periodFrames_ = r.periodFrames;
  endpointId_ = r.endpointId;
  const int block = blockFrames > 0 ? blockFrames : r.periodFrames;
  // Shared: the Windows mixer hands packets over on its own thread, up to two of its periods at once.
  block_ = block;
  fifoPeriod_ = r.shared ? 2 * r.periodFrames : r.periodFrames;
  fifo_.reset(rate_, channels_, block_, fifoPeriod_);
  packet_.assign(static_cast<size_t>(r.capacityFrames) * channels_, 0.0f);

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
    UINT64 qpc100ns = 0;  // device packet stamp (QPC, 100 ns units)
    if (FAILED(captureClient_->GetBuffer(&data, &frames, &flags, nullptr, &qpc100ns))) return;
    if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) glitches_.fetch_add(1);
    const size_t samples = static_cast<size_t>(frames) * channels_;
    if (samples > packet_.size()) packet_.resize(samples);
    const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
    for (size_t i = 0; i < samples; ++i) packet_[i] = silent ? 0.0f : KsFromDevice(format_, data, static_cast<int>(i));
    captureClient_->ReleaseBuffer(frames);
    // The device's packet stamp (capture or delivery time: devices differ, the FIFO's DLL absorbs
    // the offset and jitter); without one, now.
    double stamp;
    if (qpc100ns) {
      stamp = static_cast<double>(qpc100ns) / 1e7;
      // The Windows mixer stamps a packet at its first frame; the FIFO takes the time of its newest.
      // Unshifted, the backlog counted the newest packet twice (queued and "not yet delivered"): a
      // whole mixer period less really queued than the target, and a late mixer wake starved it.
      if (shared_) stamp += static_cast<double>(frames) / rate_;
    } else {
      LARGE_INTEGER now;
      QueryPerformanceCounter(&now);
      stamp = static_cast<double>(now.QuadPart) / qpcFreq_;
    }
    fifo_.push(packet_.data(), frames, stamp);
  }
}

void KsCapture::hold(float* data, int frames, int channels) {
  std::memset(data, 0, sizeof(float) * static_cast<size_t>(frames) * static_cast<size_t>(channels));
  if (!opened_) return;
  UINT32 packet = 0;
  while (SUCCEEDED(captureClient_->GetNextPacketSize(&packet)) && packet > 0) {
    BYTE* buffer = nullptr;
    UINT32 got = 0;
    DWORD flags = 0;
    if (FAILED(captureClient_->GetBuffer(&buffer, &got, &flags, nullptr, nullptr))) return;
    captureClient_->ReleaseBuffer(got);
  }
}

bool KsCapture::read(float* data, int frames, int channels) {
  if (!opened_) {
    std::memset(data, 0, sizeof(float) * static_cast<size_t>(frames) * static_cast<size_t>(channels));
    return false;
  }
  pull();
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  return fifo_.read(data, frames, channels, static_cast<double>(now.QuadPart) / qpcFreq_);
}

}  // namespace wha
