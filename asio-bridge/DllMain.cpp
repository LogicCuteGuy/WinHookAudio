#include <windows.h>
#include "WinHookBridgeASIO.h"
#include "WHARegister.h"
using namespace wha;
static HMODULE g_hModule=nullptr;
BOOL APIENTRY DllMain(HMODULE h,DWORD r,LPVOID){ if(r==DLL_PROCESS_ATTACH) g_hModule=h; return TRUE; }
class BridgeFactory : public IClassFactory {
  int idx_;
 public:
  explicit BridgeFactory(int i):idx_(i){}
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid,void** ppv) override {
    if(riid==IID_IUnknown||riid==IID_IClassFactory){*ppv=this; AddRef(); return S_OK;} *ppv=nullptr; return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
  ULONG STDMETHODCALLTYPE Release() override { return 1; }
  HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer,REFIID riid,void** ppv) override {
    if(outer) return CLASS_E_NOAGGREGATION;
    auto* obj=new WinHookBridgeASIO(idx_);
    HRESULT hr=obj->QueryInterface(riid,ppv);
    obj->Release(); return hr;
  }
  HRESULT STDMETHODCALLTYPE LockServer(BOOL f) override {(void)f; return S_OK;}
};
static BridgeFactory g_f1(0), g_f2(1), g_f3(2), g_f4(3);
extern "C" HRESULT __stdcall DllGetClassObject(REFCLSID rclsid,REFIID riid,void** ppv){
  if(rclsid==CLSID_WinHookBridge1) return g_f1.QueryInterface(riid,ppv);
  if(rclsid==CLSID_WinHookBridge2) return g_f2.QueryInterface(riid,ppv);
  if(rclsid==CLSID_WinHookBridge3) return g_f3.QueryInterface(riid,ppv);
  if(rclsid==CLSID_WinHookBridge4) return g_f4.QueryInterface(riid,ppv);
  return CLASS_E_CLASSNOTAVAILABLE;
}
extern "C" HRESULT __stdcall DllCanUnloadNow(){ return S_FALSE; }
static const AsioRegistration kRegistrations[] = {
    {&CLSID_WinHookBridge1, L"WinHookAudio Bridge 1", L"WinHookAudio Bridge 1"},
    {&CLSID_WinHookBridge2, L"WinHookAudio Bridge 2", L"WinHookAudio Bridge 2"},
    {&CLSID_WinHookBridge3, L"WinHookAudio Bridge 3", L"WinHookAudio Bridge 3"},
    {&CLSID_WinHookBridge4, L"WinHookAudio Bridge 4", L"WinHookAudio Bridge 4"},
};
// regsvr32 (elevated) WinHookAudioBridgeASIO64.dll
extern "C" HRESULT __stdcall DllRegisterServer(){ return RegisterAsioDrivers(g_hModule, kRegistrations, 4); }
extern "C" HRESULT __stdcall DllUnregisterServer(){ return UnregisterAsioDrivers(kRegistrations, 4); }
