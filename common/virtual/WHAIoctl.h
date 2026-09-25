#pragma once

// WHAIoctl — IOCTL definitions for WinHookAudio.sys Virtual Cable.
// Vocabulary: Virtual Cable, Slot.

#include <cstdint>

#ifdef _WIN32
#ifndef WHA_DEVICE_TYPE
#define WHA_DEVICE_TYPE 0x8000
#endif
#ifndef IOCTL_WHA_READ
#define IOCTL_WHA_READ CTL_CODE(WHA_DEVICE_TYPE, 0x800, METHOD_BUFFERED, FILE_READ_DATA)
#endif
#ifndef IOCTL_WHA_WRITE
#define IOCTL_WHA_WRITE CTL_CODE(WHA_DEVICE_TYPE, 0x801, METHOD_BUFFERED, FILE_WRITE_DATA)
#endif
#ifndef IOCTL_WHA_SET_LOOPBACK
#define IOCTL_WHA_SET_LOOPBACK CTL_CODE(WHA_DEVICE_TYPE, 0x802, METHOD_BUFFERED, FILE_WRITE_DATA)
#endif
#else
constexpr uint32_t IOCTL_WHA_READ = 0x80000800;
constexpr uint32_t IOCTL_WHA_WRITE = 0x80000801;
constexpr uint32_t IOCTL_WHA_SET_LOOPBACK = 0x80000802;
#endif

namespace wha {

constexpr uint32_t kVirtualCables = 16;
constexpr uint32_t kVirtualChannels = 8;  // up to 7.1 per cable (WHACableSetting::channels)
constexpr uint32_t kRingBufferFrames = 8192;
constexpr uint32_t kRingBufferBytes = kRingBufferFrames * kVirtualChannels * 4;  // 256 KB of float

struct WHAIoctlRead {
  uint32_t cableIndex = 0;  // 0..15
  uint32_t frames = 0;
};

struct WHAIoctlWrite {
  uint32_t cableIndex = 0;
  uint32_t frames = 0;
};

struct WHAIoctlLoopback {
  uint32_t cableIndex = 0;
  uint32_t enabled = 0;  // 0/1
};

}  // namespace wha
