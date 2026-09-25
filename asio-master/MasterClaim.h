#pragma once

// MasterClaim — one app uses the Master at a time (the Master DAW, CONTEXT.md). Vocabulary: Master
// DAW, Slot Table.
//
// Two apps each running a Master would drive the same Slot Table, Master audio and HW devices. The
// first process whose Master init() runs claims it: a named mapping (per Windows session, like the
// Slot Table) holding its PID and exe name. Another process's init() then fails, with "in use by
// <exe> (PID n)" for getErrorMessage, which DAWs show. Master instances in one process share the
// claim (a DAW may create a second while it scans its drivers); the last one to go releases it. A
// crashed owner's claim goes with its handles.

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace wha {

class MasterClaim {
 public:
  // Claim for this process (or join its claim). False: another app holds it; `error` says which.
  static bool acquire(char* error, size_t errorSize) {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.count > 0) {
      ++s.count;
      return true;
    }
    for (int attempt = 0; attempt < kAttempts; ++attempt) {
      HANDLE mapping = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(Owner), kName);
      if (!mapping) {
        std::snprintf(error, errorSize, "WinHookAudio Master: claim failed %lu", GetLastError());
        return false;
      }
      const bool exists = GetLastError() == ERROR_ALREADY_EXISTS;
      Owner* owner = static_cast<Owner*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Owner)));
      if (!owner) {
        std::snprintf(error, errorSize, "WinHookAudio Master: claim failed %lu", GetLastError());
        CloseHandle(mapping);
        return false;
      }
      if (!exists) {  // ours
        owner->pid = GetCurrentProcessId();
        ExeName(owner->exe, sizeof(owner->exe));
        s.mapping = mapping;
        s.owner = owner;
        s.count = 1;
        return true;
      }
      const Owner seen = *owner;
      UnmapViewOfFile(owner);
      CloseHandle(mapping);
      // The owner exited while another app was looking at its claim (that look keeps the name
      // alive for a moment): try again shortly.
      if (seen.pid != 0 && !Alive(seen.pid) && attempt + 1 < kAttempts) {
        Sleep(20);
        continue;
      }
      if (seen.pid == 0)  // just claimed, not filled in yet
        std::snprintf(error, errorSize, "WinHookAudio Master is in use by another app");
      else
        std::snprintf(error, errorSize, "WinHookAudio Master is in use by %.60s (PID %lu)", seen.exe,
                      static_cast<unsigned long>(seen.pid));
      return false;
    }
    std::snprintf(error, errorSize, "WinHookAudio Master is in use by another app");
    return false;
  }

  // One acquire() that succeeded is over.
  static void release() {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.count == 0 || --s.count > 0) return;
    UnmapViewOfFile(s.owner);
    CloseHandle(s.mapping);
    s.owner = nullptr;
    s.mapping = nullptr;
  }

 private:
  struct Owner {
    uint32_t pid;
    char exe[124];  // UTF-8 file name of the owning process
  };
  struct State {
    std::mutex mutex;
    int count = 0;
    HANDLE mapping = nullptr;
    Owner* owner = nullptr;
  };
  static constexpr const char* kName = "WinHookAudio_Master_Claim";
  static constexpr int kAttempts = 5;

  static State& state() {
    static State s;
    return s;
  }
  static bool Alive(uint32_t pid) {
    HANDLE p = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!p) return false;
    const bool alive = WaitForSingleObject(p, 0) == WAIT_TIMEOUT;
    CloseHandle(p);
    return alive;
  }
  static void ExeName(char* out, size_t size) {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    const wchar_t* name = wcsrchr(path, L'\\');
    name = name ? name + 1 : path;
    if (!WideCharToMultiByte(CP_UTF8, 0, name, -1, out, static_cast<int>(size), nullptr, nullptr)) out[0] = '\0';
  }
};

}  // namespace wha
