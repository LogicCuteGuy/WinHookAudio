#pragma once

// Minimal IASIO stub for offline build without Steinberg SDK.
// Real SDK provides asio.h/asiodrv.h under third_party/asio/ per ADR 0007.
// Vocabulary: Master Driver, Bridge Driver, Slot, Slot Table, Master Clock.

#include <cstdint>

#ifdef _WIN32
#include <windows.h>
#include <unknwn.h>
#else
using HRESULT = int32_t;
using ULONG = uint32_t;
#endif

namespace wha {

// ASIO error codes (subset)
enum ASIOError : int32_t {
  ASE_OK = 0,
  ASE_SUCCESS = 0x3f484d41,
  ASE_NotPresent = -1000,
  ASE_HWMalfunction = -1001,
  ASE_InvalidParameter = -1002,
  ASE_InvalidMode = -1003,
  ASE_SPNotAdvancing = -1004,
  ASE_NoClock = -1005,
  ASE_NoMemory = -1006,
};

// ASIO sample types
enum ASIOSampleType : int32_t {
  ASIOSTInt16MSB = 0,
  ASIOSTInt24MSB = 1,
  ASIOSTInt32MSB = 2,
  ASIOSTFloat32MSB = 3,
  ASIOSTFloat64MSB = 4,
  ASIOSTInt32MSB16 = 8,
  ASIOSTInt32MSB18 = 9,
  ASIOSTInt32MSB20 = 10,
  ASIOSTInt32MSB24 = 11,
  ASIOSTInt16LSB = 16,
  ASIOSTInt24LSB = 17,
  ASIOSTInt32LSB = 18,
  ASIOSTFloat32LSB = 19,
  ASIOSTFloat64LSB = 20,
  ASIOSTInt32LSB16 = 24,
  ASIOSTInt32LSB18 = 25,
  ASIOSTInt32LSB20 = 26,
  ASIOSTInt32LSB24 = 27,
  ASIOSTDSDInt8LSB1 = 32,
  ASIOSTDSDInt8MSB1 = 33,
  ASIOSTDSDInt8NER8 = 40,
};

// ASIO channel info
struct ASIOChannelInfo {
  int32_t channel = 0;
  int32_t isInput = 0;
  int32_t isActive = 0;
  int32_t channelGroup = 0;
  ASIOSampleType type = ASIOSTFloat32LSB;
  char name[32] = {};
};

// ASIO callbacks (host-provided)
struct ASIOCallbacks {
  void (*bufferSwitch)(int32_t doubleBufferIndex, int32_t directProcess) = nullptr;
  void (*sampleRateDidChange)(double sRate) = nullptr;
  int32_t (*asioMessage)(int32_t selector, int32_t value, void* message, double* opt) = nullptr;
  double (*getNanoSeconds)() = nullptr;
};

// ASIO messages
constexpr int32_t kAsioSelectorSupported = 1;
constexpr int32_t kAsioEngineVersion = 2;
constexpr int32_t kAsioResetRequest = 3;
constexpr int32_t kAsioBufferSizeChange = 4;
constexpr int32_t kAsioResyncRequest = 5;
constexpr int32_t kAsioLatenciesChanged = 6;
constexpr int32_t kAsioSupportsTimeInfo = 7;
constexpr int32_t kAsioSupportsTimeCode = 8;

// Minimal IASIO interface (subset used by 08)
struct IASIO : public IUnknown {
  virtual ASIOError init(void* sysHandle) = 0;
  virtual void getDriverName(char* name) = 0;
  virtual int32_t getDriverVersion() = 0;
  virtual void getErrorMessage(char* text) = 0;
  virtual ASIOError start() = 0;
  virtual ASIOError stop() = 0;
  virtual ASIOError getChannels(int32_t* numInputChannels, int32_t* numOutputChannels) = 0;
  virtual ASIOError getLatencies(int32_t* inputLatency, int32_t* outputLatency) = 0;
  virtual ASIOError getBufferSize(int32_t* minSize, int32_t* maxSize, int32_t* preferredSize, int32_t* granularity) = 0;
  virtual ASIOError canSampleRate(double sampleRate) = 0;
  virtual ASIOError getSampleRate(double* sampleRate) = 0;
  virtual ASIOError setSampleRate(double sampleRate) = 0;
  virtual ASIOError getClockSources(int64_t* clocks, int32_t* numSources) = 0;
  virtual ASIOError setClockSource(int32_t reference) = 0;
  virtual ASIOError getSamplePosition(int64_t* sPos, int64_t* tPos) = 0;
  virtual ASIOError getChannelInfo(ASIOChannelInfo* info) = 0;
  virtual ASIOError createBuffers(ASIOChannelInfo* infos, int32_t numChannels, int32_t bufferSize, ASIOCallbacks* callbacks) = 0;
  virtual ASIOError disposeBuffers() = 0;
  virtual ASIOError controlPanel() = 0;
  virtual ASIOError future(int32_t selector, void* opt) = 0;
  virtual ASIOError outputReady() = 0;
};

// CLSIDs (Master + 4 Bridges)
constexpr GUID CLSID_WinHookMaster = {0x12345678, 0x1234, 0x1234, {0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf0}};
constexpr GUID CLSID_WinHookBridge1 = {0x12345678, 0x1234, 0x1234, {0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf1}};
constexpr GUID CLSID_WinHookBridge2 = {0x12345678, 0x1234, 0x1234, {0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf2}};
constexpr GUID CLSID_WinHookBridge3 = {0x12345678, 0x1234, 0x1234, {0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf3}};
constexpr GUID CLSID_WinHookBridge4 = {0x12345678, 0x1234, 0x1234, {0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf4}};

}  // namespace wha
