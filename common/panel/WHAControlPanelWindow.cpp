#include "WHAControlPanelWindow.h"

#include <commdlg.h>
#include <d3d11.h>

#include <cstdio>
#include <cstring>
#include <cwchar>

#include "WHAControlPanelView.h"
#include "WHASharedMemory.h"
#include "WHASlotsJson.h"
#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace wha {

namespace {

constexpr int kClientWidth = 1180;
constexpr int kClientHeight = 720;

bool WriteFileReplace(const std::string& path, const std::string& data, DWORD* error) {
  const std::string tmp = path + ".tmp";
  HANDLE f = CreateFileA(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) {
    *error = GetLastError();
    return false;
  }
  DWORD written = 0;
  const BOOL ok = WriteFile(f, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) &&
                  written == data.size() && FlushFileBuffers(f);
  *error = ok ? 0 : GetLastError();
  CloseHandle(f);
  if (!ok) {
    DeleteFileA(tmp.c_str());
    return false;
  }
  if (!MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    *error = GetLastError();
    DeleteFileA(tmp.c_str());
    return false;
  }
  return true;
}

bool PickJsonPath(HWND owner, bool save, std::string* path) {
  char buf[MAX_PATH] = "slots.json";
  OPENFILENAMEA ofn{};
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = owner;
  ofn.lpstrFilter = "WinHookAudio slots (*.json)\0*.json\0All files\0*.*\0";
  ofn.lpstrFile = buf;
  ofn.nMaxFile = sizeof(buf);
  ofn.lpstrDefExt = "json";
  ofn.Flags = OFN_NOCHANGEDIR | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
  if (!(save ? GetSaveFileNameA(&ofn) : GetOpenFileNameA(&ofn))) return false;
  *path = buf;
  return true;
}

}  // namespace

bool ExpandSlotsJsonPath(const std::string& path, std::string* expanded) {
  char buf[MAX_PATH];
  const DWORD n = ExpandEnvironmentStringsA(path.c_str(), buf, sizeof(buf));
  if (n == 0 || n > sizeof(buf)) return false;
  *expanded = buf;
  return true;
}

bool CommitPanelSave(PanelModel& edit, const ControlPanelHost& host, std::string* status) {
  auto report = [&](const std::string& s) {
    if (status) *status = s;
  };
  if (!host.table) {
    report("Save failed: no Slot Table");
    return false;
  }
  std::string error;
  if (!ValidateSlots(edit.table, &error)) {
    report("Save rejected: " + error);
    return false;
  }
  const WHASlotTable before = *host.table;
  std::string json;
  bool clockChanged = false;
  if (!SavePanel(edit, host.table, &json, &clockChanged)) {
    report("Save rejected");
    return false;
  }
  const WHASlotTable& after = *host.table;
  const bool dawChanged = DawVisibleChanged(before, after);

  char msg[256];
  std::snprintf(msg, sizeof(msg), "Saved v%u", after.version);
  std::string result = msg;
  std::string path;
  if (!ExpandSlotsJsonPath(host.slotsJsonPath.empty() ? shm::kSlotsJsonPath : host.slotsJsonPath, &path)) {
    result += "; slots.json path invalid";
  } else {
    const size_t slash = path.find_last_of("\\/");
    if (slash != std::string::npos) CreateDirectoryA(path.substr(0, slash).c_str(), nullptr);
    DWORD err = 0;
    if (!WriteFileReplace(path, json, &err)) {
      std::snprintf(msg, sizeof(msg), "; slots.json write failed (%lu)", err);
      result += msg;
    }
  }
  FlushViewOfFile(host.table, sizeof(WHASlotTable));  // no-op error when the table is not a mapped view
  if (host.tableChanged) SetEvent(host.tableChanged);
  const bool reset = clockChanged || dawChanged;
  if (reset) result += "; DAW reset requested";
  if (host.onSaved) host.onSaved(reset);
  report(result);
  return true;
}

struct ControlPanelWindow::Gpu {
  ID3D11Device* device = nullptr;
  ID3D11DeviceContext* context = nullptr;
  IDXGISwapChain* swapChain = nullptr;
  ID3D11RenderTargetView* rtv = nullptr;

  bool CreateTarget() {
    ID3D11Texture2D* back = nullptr;
    if (FAILED(swapChain->GetBuffer(0, IID_PPV_ARGS(&back)))) return false;
    const HRESULT hr = device->CreateRenderTargetView(back, nullptr, &rtv);
    back->Release();
    return SUCCEEDED(hr);
  }
  void ReleaseTarget() {
    if (rtv) rtv->Release();
    rtv = nullptr;
  }
  ~Gpu() {
    ReleaseTarget();
    if (swapChain) swapChain->Release();
    if (context) context->Release();
    if (device) device->Release();
  }
};

ControlPanelWindow::~ControlPanelWindow() { Close(); }

bool ControlPanelWindow::IsOpen() const { return thread_ && WaitForSingleObject(thread_, 0) == WAIT_TIMEOUT; }

bool ControlPanelWindow::Open(const ControlPanelHost& host) {
  if (IsOpen()) {
    if (HWND h = hwnd_.load()) {
      ShowWindow(h, SW_RESTORE);
      SetForegroundWindow(h);
    }
    return true;
  }
  Close();  // reap a finished thread
  if (!host.table) return false;
  host_ = host;
  quit_ = false;
  frames_ = 0;
  usedWarp_ = false;
  lastError_.clear();
  thread_ = CreateThread(nullptr, 0, ThreadProc, this, 0, &threadId_);
  return thread_ != nullptr;
}

void ControlPanelWindow::Close() {
  if (!thread_) return;
  quit_ = true;
  if (HWND h = hwnd_.load()) PostMessageW(h, WM_NULL, 0, 0);  // wake the loop
  // A modal Export/Import dialog blocks the popup loop: cancel it (and any later one) until the thread ends.
  while (WaitForSingleObject(thread_, 50) == WAIT_TIMEOUT) {
    EnumThreadWindows(
        threadId_,
        [](HWND w, LPARAM) -> BOOL {
          wchar_t cls[16] = {};
          GetClassNameW(w, cls, 16);
          if (std::wcscmp(cls, L"#32770") == 0) PostMessageW(w, WM_COMMAND, IDCANCEL, 0);
          return TRUE;
        },
        0);
  }
  CloseHandle(thread_);
  thread_ = nullptr;
}

bool ControlPanelWindow::WaitClosed(DWORD timeoutMs) {
  return !thread_ || WaitForSingleObject(thread_, timeoutMs) == WAIT_OBJECT_0;
}

std::string ControlPanelWindow::LastError() const { return IsOpen() ? std::string() : lastError_; }

DWORD WINAPI ControlPanelWindow::ThreadProc(void* self) {
  static_cast<ControlPanelWindow*>(self)->Run();
  return 0;
}

LRESULT CALLBACK ControlPanelWindow::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam)) return 1;
  auto* self = reinterpret_cast<ControlPanelWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  switch (msg) {
    case WM_NCCREATE:
      SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                        reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams));
      break;
    case WM_SIZE:
      if (self && self->gpu_ && self->gpu_->swapChain && wParam != SIZE_MINIMIZED) {
        self->gpu_->ReleaseTarget();
        self->gpu_->swapChain->ResizeBuffers(0, LOWORD(lParam), HIWORD(lParam), DXGI_FORMAT_UNKNOWN, 0);
        self->gpu_->CreateTarget();
      }
      return 0;
    case WM_SYSCOMMAND:
      if ((wParam & 0xfff0) == SC_KEYMENU) return 0;  // no Alt menu
      break;
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
    default:
      break;
  }
  return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void ControlPanelWindow::Run() {
  HMODULE module = nullptr;
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                     reinterpret_cast<LPCWSTR>(&ControlPanelWindow::WndProc), &module);
  wchar_t className[64];
  swprintf_s(className, L"WinHookAudioPanel_%p", static_cast<void*>(module));  // one class per driver DLL
  WNDCLASSEXW wc{sizeof(wc)};
  wc.style = CS_CLASSDC;
  wc.lpfnWndProc = WndProc;
  wc.hInstance = module;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.lpszClassName = className;
  if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    lastError_ = "RegisterClassEx failed";
    return;
  }

  wchar_t title[96];
  if (host_.isMaster) swprintf_s(title, L"WinHookAudio Master - Control Panel");
  else swprintf_s(title, L"WinHookAudio Bridge%d - Control Panel", host_.bridgeIndex + 1);
  constexpr DWORD kStyle = WS_OVERLAPPEDWINDOW;
  constexpr DWORD kExStyle = WS_EX_TOPMOST;
  RECT rc{0, 0, kClientWidth, kClientHeight};
  AdjustWindowRectEx(&rc, kStyle, FALSE, kExStyle);
  HWND hwnd = CreateWindowExW(kExStyle, className, title, kStyle, CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left,
                              rc.bottom - rc.top, nullptr, nullptr, module, this);
  if (!hwnd) {
    lastError_ = "CreateWindowEx failed";
    UnregisterClassW(className, module);
    return;
  }

  Gpu gpu;
  DXGI_SWAP_CHAIN_DESC sd{};
  sd.BufferCount = 2;
  sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  sd.OutputWindow = hwnd;
  sd.SampleDesc.Count = 1;
  sd.Windowed = TRUE;
  sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
  HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2, D3D11_SDK_VERSION,
                                             &sd, &gpu.swapChain, &gpu.device, nullptr, &gpu.context);
  if (FAILED(hr)) {  // no usable GPU (VM, RDP): software rasterizer
    hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &sd,
                                       &gpu.swapChain, &gpu.device, nullptr, &gpu.context);
    usedWarp_ = SUCCEEDED(hr);
  }
  if (FAILED(hr) || !gpu.CreateTarget()) {
    char msg[64];
    std::snprintf(msg, sizeof(msg), "D3D11 device failed 0x%08lx", static_cast<unsigned long>(hr));
    lastError_ = msg;
    DestroyWindow(hwnd);
    UnregisterClassW(className, module);
    return;
  }
  gpu_ = &gpu;
  hwnd_ = hwnd;
  ShowWindow(hwnd, host_.hidden ? SW_HIDE : SW_SHOWNORMAL);

  IMGUI_CHECKVERSION();
  ImGuiContext* ctx = ImGui::CreateContext();
  ImGui::GetIO().IniFilename = nullptr;
  ApplyPanelStyle();
  ImGui_ImplWin32_Init(hwnd);
  ImGui_ImplDX11_Init(gpu.device, gpu.context);

  PanelModel edit;
  edit.table = *host_.table;
  edit.isMaster = host_.isMaster;
  WHASlotTable baseline = edit.table;
  PanelViewState state;
  state.bridgeIndex = host_.isMaster ? -1 : host_.bridgeIndex;
  const float clear[4] = {0x1E / 255.0f, 0x1E / 255.0f, 0x1E / 255.0f, 1.0f};

  bool running = true;
  while (running) {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
      if (msg.message == WM_QUIT) running = false;
    }
    if (!running) break;
    if (quit_) {
      DestroyWindow(hwnd);
      continue;  // drain WM_DESTROY -> WM_QUIT
    }

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    state.dirty = std::memcmp(&edit.table, &baseline, sizeof(WHASlotTable)) != 0;
    PanelViewResult r = DrawControlPanel(edit, state, host_.bridges);
    ImGui::Render();
    gpu.context->OMSetRenderTargets(1, &gpu.rtv, nullptr);
    gpu.context->ClearRenderTargetView(gpu.rtv, clear);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    gpu.swapChain->Present(1, 0);  // vsync keeps the popup off the audio cores' budget
    const int frame = ++frames_;
    if (host_.testFrameHook) host_.testFrameHook(r, frame);

    if (r.save && CommitPanelSave(edit, host_, &state.status)) baseline = edit.table;
    if (r.revert) {
      edit.table = *host_.table;
      baseline = edit.table;
      state.status = "Reverted";
    }
    std::string path;
    if (r.exportSlots && PickJsonPath(hwnd, true, &path))
      state.status = ExportSlots(edit.table, path) ? "Exported " + path : "Export failed";
    if (r.importSlots && PickJsonPath(hwnd, false, &path)) {
      std::string err;
      WHASlotTable imported = edit.table;  // a half-parsed file must not leak into the edit copy
      if (ImportSlots(imported, path, &err)) {
        edit.table = imported;
        state.status = "Imported (Save to apply)";
      } else {
        state.status = "Import failed: " + err;
      }
    }
    if (r.close || (host_.autoCloseAfterFrames > 0 && frame >= host_.autoCloseAfterFrames)) quit_ = true;
  }

  ImGui_ImplDX11_Shutdown();
  ImGui_ImplWin32_Shutdown();
  ImGui::DestroyContext(ctx);
  hwnd_ = nullptr;
  gpu_ = nullptr;
  UnregisterClassW(className, module);
}

}  // namespace wha
