#pragma once

// CableEndpointSync — keeps each Virtual Cable's Windows endpoints in the format the driver streams.
// Vocabulary: Virtual Cable, Master Clock, Worker.
// A cable streams at the Master Clock's rate in the channels and bits the panel sets, and the driver
// accepts only that format. Windows, though, picks an endpoint's device format once and keeps it (it
// does not follow KSEVENT_PINCAPS_FORMATCHANGE), so after a change every app would get
// AUDCLNT_E_UNSUPPORTED_FORMAT. This thread compares each WinHookAudio endpoint's device format with
// the one the driver last accepted and sets it when they differ (IPolicyConfig::SetDeviceFormat, what
// Sound settings > Advanced > Default Format does; no admin needed). COM and endpoint enumeration stay
// off the Worker: it only calls wake().

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

namespace wha {

class CableEndpointSync {
 public:
  // The format the driver accepted for cable c (0-based): rate, channels, WHASampleKind; false = none yet.
  using DriverFormat = std::function<bool(int cable, uint32_t& rate, uint32_t& channels, uint32_t& format)>;

  explicit CableEndpointSync(DriverFormat driverFormat) : driverFormat_(std::move(driverFormat)) {}
  ~CableEndpointSync() { stop(); }
  CableEndpointSync(const CableEndpointSync&) = delete;
  CableEndpointSync& operator=(const CableEndpointSync&) = delete;

  bool start();
  void stop();
  void wake() {  // any thread, cheap: check now instead of at the next 2 s pass
    if (wake_) SetEvent(wake_);
  }
  // Endpoints switched so far, and the last SetDeviceFormat failure (HRESULT, 0 = none).
  uint32_t switches() const { return switches_.load(); }
  int32_t lastError() const { return lastError_.load(); }

 private:
  static DWORD WINAPI threadProc(LPVOID self);
  void run();

  DriverFormat driverFormat_;
  HANDLE thread_ = nullptr;
  HANDLE wake_ = nullptr;
  std::atomic<bool> stop_{false};
  std::atomic<uint32_t> switches_{0};
  std::atomic<int32_t> lastError_{0};
};

}  // namespace wha
