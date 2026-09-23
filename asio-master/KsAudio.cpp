#include "KsAudio.h"
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <functiondiscoverykeys_devpkey.h>
#include <ksmedia.h>

#include <cstdio>

namespace wha {

namespace {

constexpr int kDeviceChannels = 2;
constexpr int kBufferPeriods = 4;   // device buffer = 4 periods: headroom for Master Clock vs device clock
constexpr int kPrefillPeriods = 2;  // silence queued before Start

WAVEFORMATEXTENSIBLE MakeFormat(int32_t sampleRate, KsAudio::SampleFormat format) {
  const bool isFloat = format == KsAudio::SampleFormat::Float32;
  const WORD bits = format == KsAudio::SampleFormat::Pcm16 ? 16 : 32;
  WAVEFORMATEXTENSIBLE x{};
  x.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
  x.Format.nChannels = kDeviceChannels;
  x.Format.nSamplesPerSec = static_cast<DWORD>(sampleRate);
  x.Format.wBitsPerSample = bits;
  x.Format.nBlockAlign = static_cast<WORD>(kDeviceChannels * bits / 8);
  x.Format.nAvgBytesPerSec = x.Format.nSamplesPerSec * x.Format.nBlockAlign;
  x.Format.cbSize = 22;
  x.Samples.wValidBitsPerSample = format == KsAudio::SampleFormat::Pcm24In32 ? 24 : bits;
  x.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
  x.SubFormat = isFloat ? KSDATAFORMAT_SUBTYPE_IEEE_FLOAT : KSDATAFORMAT_SUBTYPE_PCM;
  return x;
}

float Clamp(float v) { return v > 1.0f ? 1.0f : (v < -1.0f ? -1.0f : v); }

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

bool KsAudio::open(int32_t sampleRate, int32_t bufferFrames, int32_t blockFrames) {
  close();
  sampleRate_ = sampleRate;
  bufferFrames_ = bufferFrames;
  exclusive_ = false;
  lastError_ = S_OK;
  lastStep_ = "";
  capacityFrames_ = 0;
  writes_ = 0;
  underruns_ = 0;
  drops_ = 0;
  minFill_ = -1;
  maxFill_ = 0;

  HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  comInitialized_ = SUCCEEDED(hr);

  IMMDeviceEnumerator* enumerator = nullptr;
  hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&enumerator);
  if (FAILED(hr)) return fail("MMDeviceEnumerator", hr);

  IMMDevice* device = nullptr;
  hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
  enumerator->Release();
  if (FAILED(hr)) return fail("GetDefaultAudioEndpoint", hr);

  hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&audioClient_);
  if (FAILED(hr)) { device->Release(); return fail("Activate", hr); }

  // Period: 0 = device minimum; a smaller request than the device allows is rejected, not stretched.
  REFERENCE_TIME defaultPeriod = 0, minPeriod = 0;
  audioClient_->GetDevicePeriod(&defaultPeriod, &minPeriod);
  // Round up: 128 frames @ 48k = 26666.67 hns must compare equal to a 26667 hns device minimum.
  REFERENCE_TIME period = bufferFrames > 0
                              ? (static_cast<REFERENCE_TIME>(bufferFrames) * 10000000 + sampleRate - 1) / sampleRate
                              : minPeriod;
  if (period < minPeriod) { device->Release(); return fail("period below device minimum", AUDCLNT_E_INVALID_DEVICE_PERIOD); }
  // Buffer: whole periods, at least kBufferPeriods and at least 4 write() blocks.
  const REFERENCE_TIME blockTime = static_cast<REFERENCE_TIME>(blockFrames) * 10000000 / sampleRate;
  REFERENCE_TIME periods = kBufferPeriods;
  while (periods * period < 4 * blockTime) ++periods;

  // Exclusive formats differ per device (HD Audio often has no float): take the first one it accepts.
  const SampleFormat candidates[] = {SampleFormat::Float32, SampleFormat::Pcm24In32, SampleFormat::Pcm16};
  WAVEFORMATEXTENSIBLE fmt{};
  bool found = false;
  for (SampleFormat c : candidates) {
    fmt = MakeFormat(sampleRate, c);
    if (audioClient_->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, &fmt.Format, nullptr) == S_OK) {
      format_ = c;
      found = true;
      break;
    }
  }
  if (!found) { device->Release(); return fail("no exclusive format (float32/pcm24in32/pcm16)", AUDCLNT_E_UNSUPPORTED_FORMAT); }

  // Exclusive only — no shared fallback (busy pin must surface as failure). Timer-driven, so the
  // buffer may be longer than the period.
  hr = audioClient_->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, 0, period * periods, period, &fmt.Format, nullptr);
  if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {  // re-open with the device-aligned period
    UINT32 aligned = 0;
    audioClient_->GetBufferSize(&aligned);
    period = static_cast<REFERENCE_TIME>(10000000.0 * aligned / static_cast<double>(periods) / sampleRate + 0.5);
    audioClient_->Release();
    audioClient_ = nullptr;
    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&audioClient_);
    if (SUCCEEDED(hr)) hr = audioClient_->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, 0, period * periods, period, &fmt.Format, nullptr);
  }
  device->Release();
  if (FAILED(hr)) return fail("Initialize exclusive", hr);
  exclusive_ = true;
  bufferFrames_ = static_cast<int32_t>(period * sampleRate / 10000000);
  UINT32 capacity = 0;
  audioClient_->GetBufferSize(&capacity);
  capacityFrames_ = static_cast<int32_t>(capacity);

  hr = audioClient_->GetService(__uuidof(IAudioRenderClient), (void**)&renderClient_);
  if (FAILED(hr)) return fail("GetService IAudioRenderClient", hr);

  hr = audioClient_->GetService(__uuidof(IAudioClock), (void**)&audioClock_);
  // Not fatal if no clock

  opened_ = true;
  // Do not CoUninitialize while IAudioClient is held — caller or close() will handle it
  return true;
}

void KsAudio::close() {
  if (renderClient_) { renderClient_->Release(); renderClient_ = nullptr; }
  if (audioClock_) { audioClock_->Release(); audioClock_ = nullptr; }
  if (audioClient_) { audioClient_->Stop(); audioClient_->Release(); audioClient_ = nullptr; }
  opened_ = false;
  exclusive_ = false;
  if (comInitialized_) { CoUninitialize(); comInitialized_ = false; }
}

bool KsAudio::start() {
  if (!audioClient_ || !renderClient_) return false;
  // Queue silence so the first Worker writes land ahead of the device, not in an underrun.
  BYTE* buffer = nullptr;
  const UINT32 prefill = static_cast<UINT32>(bufferFrames_ * kPrefillPeriods);
  if (SUCCEEDED(renderClient_->GetBuffer(prefill, &buffer))) renderClient_->ReleaseBuffer(prefill, AUDCLNT_BUFFERFLAGS_SILENT);
  HRESULT hr = audioClient_->Start();
  return SUCCEEDED(hr);
}
void KsAudio::stop() {
  if (audioClient_) audioClient_->Stop();
}

// data is planar: channel c at data[c * frames]. Source channel c -> device channel c; others silent.
bool KsAudio::write(const float* data, int frames, int channels) {
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
    for (int ch = 0; ch < kDeviceChannels; ++ch) {
      const float v = ch < channels ? data[ch * frames + f] : 0.0f;
      const int i = f * kDeviceChannels + ch;
      switch (format_) {
        case SampleFormat::Float32: reinterpret_cast<float*>(buffer)[i] = v; break;
        case SampleFormat::Pcm24In32:
          reinterpret_cast<int32_t*>(buffer)[i] = static_cast<int32_t>(Clamp(v) * 8388607.0f) * 256;
          break;
        case SampleFormat::Pcm16: reinterpret_cast<int16_t*>(buffer)[i] = static_cast<int16_t>(Clamp(v) * 32767.0f); break;
      }
    }
  }
  hr = renderClient_->ReleaseBuffer(frames, 0);
  if (SUCCEEDED(hr)) writes_.fetch_add(1);
  return SUCCEEDED(hr);
}

long KsAudio::padding() const {
  if (!audioClient_) return -1;
  UINT32 queued = 0;
  return SUCCEEDED(audioClient_->GetCurrentPadding(&queued)) ? static_cast<long>(queued) : -1;
}

double KsAudio::latencyMs() const {
  if (!sampleRate_ || !bufferFrames_) return 0;
  return 1000.0 * bufferFrames_ / sampleRate_;
}

}  // namespace wha
