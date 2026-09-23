#pragma once

// WHAAsio — the single ASIO include for the Master and Bridge drivers (ADR 0007).
// Vocabulary: Master Driver, Bridge Driver, Master Clock.
//
// WHA_HAVE_ASIO_SDK=1 (third_party/asio vendored): the real Steinberg ASIO SDK 2.3.4 headers.
// Otherwise: an offline mirror of exactly the same declarations — global namespace, same names,
// types, values and #pragma pack(4) — so the drivers compile identically and the vtable the DAW
// calls is the one the SDK defines. Keep the mirror in sync with common/asio.h + iasiodrv.h.

#include <windows.h>
#include <unknwn.h>

#include <cstdint>

#if WHA_HAVE_ASIO_SDK

#pragma warning(push, 0)
#include "asiosys.h"
#include "asio.h"
#include "iasiodrv.h"
#pragma warning(pop)

#else  // offline mirror of asio.h / iasiodrv.h (SDK 2.3.4)

#pragma pack(push, 4)

typedef long ASIOBool;
enum { ASIOFalse = 0, ASIOTrue = 1 };

typedef double ASIOSampleRate;
typedef struct ASIOSamples {
  unsigned long hi;
  unsigned long lo;
} ASIOSamples;
typedef struct ASIOTimeStamp {
  unsigned long hi;
  unsigned long lo;
} ASIOTimeStamp;

typedef long ASIOSampleType;
enum {
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
  ASIOSTLastEntry
};

typedef long ASIOError;
enum {
  ASE_OK = 0,
  ASE_SUCCESS = 0x3f4847a0,
  ASE_NotPresent = -1000,
  ASE_HWMalfunction,
  ASE_InvalidParameter,
  ASE_InvalidMode,
  ASE_SPNotAdvancing,
  ASE_NoClock,
  ASE_NoMemory
};

typedef struct ASIOTime ASIOTime;  // only passed by pointer here

typedef struct ASIOCallbacks {
  void (*bufferSwitch)(long doubleBufferIndex, ASIOBool directProcess);
  void (*sampleRateDidChange)(ASIOSampleRate sRate);
  long (*asioMessage)(long selector, long value, void* message, double* opt);
  ASIOTime* (*bufferSwitchTimeInfo)(ASIOTime* params, long doubleBufferIndex, ASIOBool directProcess);
} ASIOCallbacks;

enum {
  kAsioSelectorSupported = 1,
  kAsioEngineVersion,
  kAsioResetRequest,
  kAsioBufferSizeChange,
  kAsioResyncRequest,
  kAsioLatenciesChanged,
  kAsioSupportsTimeInfo,
  kAsioSupportsTimeCode,
  kAsioMMCCommand,
  kAsioSupportsInputMonitor,
  kAsioSupportsInputGain,
  kAsioSupportsInputMeter,
  kAsioSupportsOutputGain,
  kAsioSupportsOutputMeter,
  kAsioOverload,
  kAsioNumMessageSelectors
};

typedef struct ASIOClockSource {
  long index;
  long associatedChannel;
  long associatedGroup;
  ASIOBool isCurrentSource;
  char name[32];
} ASIOClockSource;

typedef struct ASIOChannelInfo {
  long channel;
  ASIOBool isInput;
  ASIOBool isActive;
  long channelGroup;
  ASIOSampleType type;
  char name[32];
} ASIOChannelInfo;

typedef struct ASIOBufferInfo {
  ASIOBool isInput;
  long channelNum;
  void* buffers[2];
} ASIOBufferInfo;

enum {
  kAsioEnableTimeCodeRead = 1,
  kAsioDisableTimeCodeRead,
  kAsioSetInputMonitor,
  kAsioTransport,
  kAsioSetInputGain,
  kAsioGetInputMeter,
  kAsioSetOutputGain,
  kAsioGetOutputMeter,
  kAsioCanInputMonitor,
  kAsioCanTimeInfo,
  kAsioCanTimeCode,
  kAsioCanTransport,
  kAsioCanInputGain,
  kAsioCanInputMeter,
  kAsioCanOutputGain,
  kAsioCanOutputMeter,
  kAsioOptionalOne,
};

#pragma pack(pop)

struct IASIO : public IUnknown {
  virtual ASIOBool init(void* sysHandle) = 0;
  virtual void getDriverName(char* name) = 0;
  virtual long getDriverVersion() = 0;
  virtual void getErrorMessage(char* string) = 0;
  virtual ASIOError start() = 0;
  virtual ASIOError stop() = 0;
  virtual ASIOError getChannels(long* numInputChannels, long* numOutputChannels) = 0;
  virtual ASIOError getLatencies(long* inputLatency, long* outputLatency) = 0;
  virtual ASIOError getBufferSize(long* minSize, long* maxSize, long* preferredSize, long* granularity) = 0;
  virtual ASIOError canSampleRate(ASIOSampleRate sampleRate) = 0;
  virtual ASIOError getSampleRate(ASIOSampleRate* sampleRate) = 0;
  virtual ASIOError setSampleRate(ASIOSampleRate sampleRate) = 0;
  virtual ASIOError getClockSources(ASIOClockSource* clocks, long* numSources) = 0;
  virtual ASIOError setClockSource(long reference) = 0;
  virtual ASIOError getSamplePosition(ASIOSamples* sPos, ASIOTimeStamp* tStamp) = 0;
  virtual ASIOError getChannelInfo(ASIOChannelInfo* info) = 0;
  virtual ASIOError createBuffers(ASIOBufferInfo* bufferInfos, long numChannels, long bufferSize,
                                  ASIOCallbacks* callbacks) = 0;
  virtual ASIOError disposeBuffers() = 0;
  virtual ASIOError controlPanel() = 0;
  virtual ASIOError future(long selector, void* opt) = 0;
  virtual ASIOError outputReady() = 0;
};

#endif  // WHA_HAVE_ASIO_SDK

static_assert(sizeof(ASIOChannelInfo) == 52, "ASIOChannelInfo layout (SDK pack 4)");
static_assert(sizeof(ASIOBufferInfo) == 24, "ASIOBufferInfo layout (SDK pack 4)");
static_assert(sizeof(ASIOClockSource) == 48, "ASIOClockSource layout (SDK pack 4)");
static_assert(sizeof(ASIOSamples) == 8 && sizeof(ASIOTimeStamp) == 8, "ASIO 64-bit hi/lo structs");

namespace wha {

// ASIOSamples / ASIOTimeStamp carry 64-bit values as {hi, lo} (NATIVE_INT64 0 on Windows).
inline void ToAsio64(uint64_t v, unsigned long* hi, unsigned long* lo) {
  *hi = static_cast<unsigned long>(v >> 32);
  *lo = static_cast<unsigned long>(v & 0xffffffffu);
}
inline uint64_t FromAsio64(unsigned long hi, unsigned long lo) { return (static_cast<uint64_t>(hi) << 32) | lo; }

// CLSIDs (Master + 4 Bridges) — must match installer/WinHookAudio.reg and .iss
constexpr GUID CLSID_WinHookMaster = {0x12345678, 0x1234, 0x1234, {0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf0}};
constexpr GUID CLSID_WinHookBridge1 = {0x12345678, 0x1234, 0x1234, {0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf1}};
constexpr GUID CLSID_WinHookBridge2 = {0x12345678, 0x1234, 0x1234, {0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf2}};
constexpr GUID CLSID_WinHookBridge3 = {0x12345678, 0x1234, 0x1234, {0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf3}};
constexpr GUID CLSID_WinHookBridge4 = {0x12345678, 0x1234, 0x1234, {0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf4}};

}  // namespace wha
