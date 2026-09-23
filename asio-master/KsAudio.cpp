#include "KsAudio.h"
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audiopolicy.h>

#include <cstdio>

namespace wha {

namespace {

struct ClientLock {  // exclusive SRW lock for the scope
  explicit ClientLock(SRWLOCK& lock) : lock_(lock) { AcquireSRWLockExclusive(&lock_); }
  ~ClientLock() { ReleaseSRWLockExclusive(&lock_); }
  ClientLock(const ClientLock&) = delete;
  ClientLock& operator=(const ClientLock&) = delete;
  SRWLOCK& lock_;
};

}  // namespace

KsAudio::KsAudio() = default;
KsAudio::~KsAudio() { close(); }

bool KsAudio::fail(const char* step, HRESULT hr) {
  lastError_ = hr;
  lastStep_ = step;
  char msg[160];
  std::snprintf(msg, sizeof(msg), "WinHookAudio KsAudio: %s failed 0x%08lX\n", step, static_cast<unsigned long>(hr));
  OutputDebugStringA(msg);
  close();
  return false;
}

bool KsAudio::open(int32_t sampleRate, int32_t bufferFrames, int32_t blockFrames, const char* endpointId) {
  close();
  sampleRate_ = sampleRate;
  bufferFrames_ = bufferFrames;
  exclusive_ = false;
  lastError_ = S_OK;
  lastStep_ = "";
  capacityFrames_ = 0;
  streamLatencyFrames_ = 0;
  endpointId_.clear();
  writes_ = 0;
  underruns_ = 0;
  drops_ = 0;
  minFill_ = -1;
  maxFill_ = 0;

  comInitialized_ = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
  KsOpenResult r;
  if (!KsOpenExclusive(eRender, endpointId, sampleRate, bufferFrames, blockFrames, r)) return fail(r.step, r.error);
  audioClient_ = r.client;
  format_ = r.format;
  exclusive_ = true;
  bufferFrames_ = r.periodFrames;
  capacityFrames_ = r.capacityFrames;
  streamLatencyFrames_ = r.streamLatencyFrames;
  endpointId_ = r.endpointId;
  blockFrames_ = blockFrames > 0 ? blockFrames : bufferFrames_;

  HRESULT hr = audioClient_->GetService(__uuidof(IAudioRenderClient), (void**)&renderClient_);
  if (FAILED(hr)) return fail("GetService IAudioRenderClient", hr);

  hr = audioClient_->GetService(__uuidof(IAudioClock), (void**)&audioClock_);
  // Not fatal if no clock

  opened_ = true;
  // Do not CoUninitialize while IAudioClient is held — caller or close() will handle it
  return true;
}

void KsAudio::close() {
  ClientLock lock(clientLock_);
  if (renderClient_) { renderClient_->Release(); renderClient_ = nullptr; }
  if (audioClock_) { audioClock_->Release(); audioClock_ = nullptr; }
  if (audioClient_) { audioClient_->Stop(); audioClient_->Release(); audioClient_ = nullptr; }
  opened_ = false;
  exclusive_ = false;
  if (comInitialized_) { CoUninitialize(); comInitialized_ = false; }
}

bool KsAudio::start() {
  ClientLock lock(clientLock_);
  if (!audioClient_ || !renderClient_) return false;
  // Queue silence up to the target fill: the first Worker writes land ahead of the device, and the
  // Master Clock starts at its steady state instead of bursting to fill the buffer.
  BYTE* buffer = nullptr;
  const UINT32 prefill = static_cast<UINT32>(targetFill() > 0 ? targetFill() : bufferFrames_);
  if (SUCCEEDED(renderClient_->GetBuffer(prefill, &buffer))) renderClient_->ReleaseBuffer(prefill, AUDCLNT_BUFFERFLAGS_SILENT);
  HRESULT hr = audioClient_->Start();
  return SUCCEEDED(hr);
}
void KsAudio::stop() {
  ClientLock lock(clientLock_);
  if (audioClient_) audioClient_->Stop();
}

// data is planar: channel c at data[c * frames]. Source channel c -> device channel c; others silent.
bool KsAudio::write(const float* data, int frames, int channels) {
  ClientLock lock(clientLock_);
  if (!renderClient_ || !audioClient_) return false;
  UINT32 padding = 0;
  audioClient_->GetCurrentPadding(&padding);
  UINT32 bufferFrames = 0;
  audioClient_->GetBufferSize(&bufferFrames);
  UINT32 available = bufferFrames - padding;
  if (available < (UINT32)frames) { drops_.fetch_add(1); return false; }
  if (padding == 0 && writes_.load() > 0) underruns_.fetch_add(1);  // device ran dry before this block
  const int32_t fill = static_cast<int32_t>(padding);
  if (minFill_.load() < 0 || fill < minFill_.load()) minFill_ = fill;
  if (fill > maxFill_.load()) maxFill_ = fill;
  BYTE* buffer = nullptr;
  HRESULT hr = renderClient_->GetBuffer(frames, &buffer);
  if (FAILED(hr)) return false;
  for (int f = 0; f < frames; ++f) {
    for (int ch = 0; ch < kKsDeviceChannels; ++ch) {
      KsToDevice(format_, ch < channels ? data[ch * frames + f] : 0.0f, buffer, f * kKsDeviceChannels + ch);
    }
  }
  hr = renderClient_->ReleaseBuffer(frames, 0);
  if (SUCCEEDED(hr)) writes_.fetch_add(1);
  return SUCCEEDED(hr);
}

long KsAudio::padding() const {
  ClientLock lock(clientLock_);
  if (!audioClient_) return -1;
  UINT32 queued = 0;
  return SUCCEEDED(audioClient_->GetCurrentPadding(&queued)) ? static_cast<long>(queued) : -1;
}

double KsAudio::latencyMs() const {
  if (!sampleRate_ || !bufferFrames_) return 0;
  return 1000.0 * bufferFrames_ / sampleRate_;
}

}  // namespace wha
