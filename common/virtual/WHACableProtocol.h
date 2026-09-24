#pragma once

// WHACableProtocol - what WinHookAudio.sys (driver/) and the Worker exchange for Virtual Cables.
// Plain C types only: the kernel build has no C++ library. Include <windows.h> + <winioctl.h> (user
// mode) or <wdm.h> (kernel) first, for CTL_CODE.
// Vocabulary: Virtual Cable, Worker.
//
// A cable has two Windows endpoints. Its playback endpoint ("speaker") takes what Windows apps play
// and hands it to the Worker (a DAW input); its recording endpoint ("microphone") gives Windows apps
// what the Worker wrote (a DAW output).

// The driver's control device. The Worker opens it once; not present = the driver is not installed.
#define WHA_CABLE_USER_PATH L"\\\\.\\WinHookAudioCable"
#define WHA_CABLE_DEVICE_NAME L"\\Device\\WinHookAudioCable"
#define WHA_CABLE_LINK_NAME L"\\DosDevices\\WinHookAudioCable"

#define WHA_CABLE_PROTOCOL 1u
#define WHA_CABLE_MAX_FRAMES 4096u  // per exchange

// One Worker tick for one cable (METHOD_BUFFERED). In: the header, then (hasRecord) `frames` frames
// for the recording endpoint. Out: the header, then `frames` frames from the playback endpoint
// (silence while nothing plays). Both interleaved stereo float. frames = 0 only asks for the header.
#define IOCTL_WHA_CABLE_EXCHANGE CTL_CODE(0x8000u, 0x810, METHOD_BUFFERED, FILE_ANY_ACCESS)  // unsigned: bit 31 set

typedef struct WHACableExchange {
  unsigned int protocol;    // in: WHA_CABLE_PROTOCOL
  unsigned int cable;       // in: 0-based
  unsigned int frames;      // in: frames each way, 0..WHA_CABLE_MAX_FRAMES
  unsigned int hasRecord;   // in: 1 = recording frames follow the header; 0 = record silence
  unsigned int cables;      // out: how many cables the driver has
  unsigned int playRate;    // out: Hz of the running Windows playback stream, 0 = none
  unsigned int recordRate;  // out: Hz of the running Windows recording stream, 0 = none
  unsigned int playFill;    // out: frames queued from Windows for the Worker (after this exchange)
  unsigned int recordFill;  // out: frames queued from the Worker for Windows
  unsigned int playUnderruns, playDrops, recordUnderruns, recordDrops;  // out: since the driver loaded
  unsigned int reserved[3];
} WHACableExchange;

// Diagnostic: the driver's log of the last WHA_KS_LOG_ENTRIES opens and device controls on its audio
// filters (who asked what, and the answer), to see what Windows' endpoint builder asks and gets
// refused. Out: WHAKsLogHeader, then the entries oldest first.
#define IOCTL_WHA_KS_LOG CTL_CODE(0x8000u, 0x811, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define WHA_KS_LOG_ENTRIES 2048u

typedef struct WHAKsLogHeader {
  unsigned int total;    // requests logged since the driver loaded
  unsigned int entries;  // entries that follow
} WHAKsLogHeader;

typedef struct WHAKsLogEntry {
  unsigned int sequence;       // 0-based, since the driver loaded
  unsigned int process;        // requesting process id
  unsigned int major;          // IRP_MJ_CREATE (0) or IRP_MJ_DEVICE_CONTROL (14)
  unsigned int ioctl;          // device control code
  int status;                  // the driver's answer (NTSTATUS)
  unsigned int inLength, outLength;
  unsigned int onPin;          // 1 = the handle is a pin (name = its filter's)
  unsigned int propertySet[4]; // KS property/method/event set GUID (IOCTL_KS_PROPERTY/METHOD/ENABLE_EVENT)
  unsigned int propertyId, propertyFlags;
  unsigned int instance[2];    // the first 8 bytes after the KSPROPERTY (pin or node id, ...)
  unsigned short name[40];     // the filter's reference string ("\OutputWave1"), truncated
  unsigned int valueBytes;     // bytes of `value` captured (a property SET's data, from the start)
  unsigned char value[128];
} WHAKsLogEntry;
