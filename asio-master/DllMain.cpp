#include <windows.h>
#include "WinHookMasterASIO.h"
#include "WHARegister.h"

using namespace wha;

static HMODULE g_hModule = nullptr;

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) g_hModule = hModule;
  return TRUE;
}

class MasterFactory : public IClassFactory {
 public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
    if (riid == IID_IUnknown || riid == IID_IClassFactory) { *ppv = this; AddRef(); return S_OK; }
    *ppv = nullptr; return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
  ULONG STDMETHODCALLTYPE Release() override { return 1; }
  HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* pUnkOuter, REFIID riid, void** ppv) override {
    if (pUnkOuter) return CLASS_E_NOAGGREGATION;
    auto* obj = new WinHookMasterASIO();
    HRESULT hr = obj->QueryInterface(riid, ppv);
    obj->Release();
    return hr;
  }
  HRESULT STDMETHODCALLTYPE LockServer(BOOL fLock) override { (void)fLock; return S_OK; }
};

static MasterFactory g_factory;

extern "C" HRESULT __stdcall DllGetClassObject(REFCLSID rclsid, REFIID riid, void** ppv) {
  if (rclsid == CLSID_WinHookMaster) return g_factory.QueryInterface(riid, ppv);
  return CLASS_E_CLASSNOTAVAILABLE;
}
extern "C" HRESULT __stdcall DllCanUnloadNow() { return S_FALSE; }
static const AsioRegistration kRegistrations[] = {
    {&CLSID_WinHookMaster, L"WinHookAudio Master", L"WinHookAudio Master (512)"},
};
// regsvr32 (elevated) WinHookAudioMasterASIO64.dll
extern "C" HRESULT __stdcall DllRegisterServer() { return RegisterAsioDrivers(g_hModule, kRegistrations, 1); }
extern "C" HRESULT __stdcall DllUnregisterServer() { return UnregisterAsioDrivers(kRegistrations, 1); }
