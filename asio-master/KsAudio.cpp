#include "KsAudio.h"
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <functiondiscoverykeys_devpkey.h>

namespace wha {

KsAudio::KsAudio() = default;
KsAudio::~KsAudio() { close(); }

bool KsAudio::open(int32_t sampleRate, int32_t bufferFrames) {
  close();
  sampleRate_ = sampleRate;
  bufferFrames_ = bufferFrames;
  exclusive_ = false;

  HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  comInitialized_ = SUCCEEDED(hr);
  bool needUninit = comInitialized_;

  IMMDeviceEnumerator* enumerator = nullptr;
  hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&enumerator);
  if (FAILED(hr)) { if (needUninit) CoUninitialize(); return false; }

  IMMDevice* device = nullptr;
  hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
  enumerator->Release();
  if (FAILED(hr)) { if (needUninit) CoUninitialize(); return false; }

  hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&audioClient_);
  device->Release();
  if (FAILED(hr)) { if (needUninit) CoUninitialize(); return false; }

  WAVEFORMATEX* mixFormat = nullptr;
  hr = audioClient_->GetMixFormat(&mixFormat);
  if (FAILED(hr)) {
    if (needUninit) CoUninitialize();
    comInitialized_ = false;
    return false;
  }

  WAVEFORMATEXTENSIBLE extFmt{};
  extFmt.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
  extFmt.Format.nChannels = 2;
  extFmt.Format.nSamplesPerSec = sampleRate;
  extFmt.Format.wBitsPerSample = 32;
  extFmt.Format.nBlockAlign = extFmt.Format.nChannels * extFmt.Format.wBitsPerSample / 8;
  extFmt.Format.nAvgBytesPerSec = extFmt.Format.nSamplesPerSec * extFmt.Format.nBlockAlign;
  extFmt.Format.cbSize = 22;
  extFmt.Samples.wValidBitsPerSample = 32;
  extFmt.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
  extFmt.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
  WAVEFORMATEX* fmtPtr = reinterpret_cast<WAVEFORMATEX*>(&extFmt);
  // If mix is not float, use PCM
  if (mixFormat && mixFormat->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
    auto* mixExt = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(mixFormat);
    if (mixExt->SubFormat != KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) {
      extFmt.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
    }
  } else if (mixFormat && mixFormat->wFormatTag != WAVE_FORMAT_EXTENSIBLE) {
    extFmt.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
  }
  CoTaskMemFree(mixFormat);
  mixFormat = nullptr;

  // Exclusive only — no shared fallback (busy pin must surface as failure)
  hr = audioClient_->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, 0, bufferFrames * 10000000 / sampleRate, bufferFrames * 10000000 / sampleRate, fmtPtr, nullptr);
  if (FAILED(hr)) {
    audioClient_->Release();
    audioClient_ = nullptr;
    if (needUninit) CoUninitialize();
    comInitialized_ = false;
    return false;
  }
  exclusive_ = true;

  hr = audioClient_->GetService(__uuidof(IAudioRenderClient), (void**)&renderClient_);
  if (FAILED(hr)) {
    audioClient_->Release();
    audioClient_ = nullptr;
    if (needUninit) CoUninitialize();
    comInitialized_ = false;
    return false;
  }

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
  if (!audioClient_) return false;
  HRESULT hr = audioClient_->Start();
  return SUCCEEDED(hr);
}
void KsAudio::stop() {
  if (audioClient_) audioClient_->Stop();
}

bool KsAudio::write(const float* data, int frames, int channels) {
  if (!renderClient_ || !audioClient_) return false;
  UINT32 padding = 0;
  audioClient_->GetCurrentPadding(&padding);
  UINT32 bufferFrames = 0;
  audioClient_->GetBufferSize(&bufferFrames);
  UINT32 available = bufferFrames - padding;
  if (available < (UINT32)frames) return false;
  BYTE* buffer = nullptr;
  HRESULT hr = renderClient_->GetBuffer(frames, &buffer);
  if (FAILED(hr)) return false;
  // Interleave float data
  float* out = reinterpret_cast<float*>(buffer);
  for (int f = 0; f < frames; ++f) {
    for (int ch = 0; ch < channels; ++ch) {
      out[f * channels + ch] = data[ch * frames + f];
    }
  }
  hr = renderClient_->ReleaseBuffer(frames, 0);
  return SUCCEEDED(hr);
}

double KsAudio::latencyMs() const {
  if (!sampleRate_ || !bufferFrames_) return 0;
  return 1000.0 * bufferFrames_ / sampleRate_;
}

}  // namespace wha
