/* ARM64X forwarder (ADR 0016): the DLL the 64-bit registry points at on Windows on ARM. One file,
   built twice:
   - ARM64 half (ARM64 processes): exports forwarded to WHA_FWD_TARGET (...ASIOARM64), as object-file
     directives. From a .def (or /export on the link command line) this linker forwards only exports
     it can resolve at link time, which it cannot.
   - ARM64EC half (x64 processes, emulated): the linker does not forward these at all, so small
     functions load the x64 DLL from this DLL's folder (...ASIOARM64X.dll -> ...ASIO64.dll) and call it.
   No headers and no C runtime, so the build step needs only kernel32.lib (and the .def). WHA_FWD_MASTER adds the
   Master's WHAGetMasterStats. */
#if defined(_M_ARM64EC) || defined(WHA_FWD_RUNTIME)  /* WHA_FWD_RUNTIME: the same code on x64, for tests */

typedef long HRESULT;
typedef int (__stdcall* WhaProc)(void);
__declspec(dllimport) unsigned long __stdcall GetModuleFileNameW(void* module, unsigned short* path, unsigned long size);
__declspec(dllimport) void* __stdcall LoadLibraryExW(const unsigned short* path, void* file, unsigned long flags);
__declspec(dllimport) WhaProc __stdcall GetProcAddress(void* module, const char* name);
extern char __ImageBase;

#define WHA_MAX_PATH 32768
#define WHA_LOAD_WITH_ALTERED_SEARCH_PATH 0x00000008
#define WHA_CLASS_E_CLASSNOTAVAILABLE ((HRESULT)0x80040111L)
#define WHA_E_FAIL ((HRESULT)0x80004005L)
#define WHA_S_FALSE ((HRESULT)1)

/* Exported by the link's ForwarderMaster.def / ForwarderBridge.def (PRIVATE: COM entry points stay out
   of the import library). */
static void* volatile gTarget;
static unsigned short gPath[WHA_MAX_PATH];

/* The x64 DLL next to this one: "...ARM64X.dll" (10 characters) becomes "...64.dll". */
static WhaProc Target(const char* name) {
  void* target = gTarget;
  if (!target) {
    static const unsigned short suffix[] = {'6', '4', '.', 'd', 'l', 'l', 0};
    unsigned long n = GetModuleFileNameW(&__ImageBase, gPath, WHA_MAX_PATH);
    unsigned long i;
    if (n < 10 || n >= WHA_MAX_PATH - 1) return 0;
    for (i = 0; i < sizeof(suffix) / sizeof(suffix[0]); ++i) gPath[n - 10 + i] = suffix[i];
    target = LoadLibraryExW(gPath, 0, WHA_LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!target) return 0;
    gTarget = target;  /* two threads racing load it twice: LoadLibrary counts, nothing is lost */
  }
  return GetProcAddress(target, name);
}

typedef HRESULT (__stdcall* GetClassObjectFn)(const void* clsid, const void* iid, void** object);
typedef HRESULT (__stdcall* NoArgFn)(void);
typedef int (__stdcall* StatsFn)(void* out);

HRESULT __stdcall DllGetClassObject(const void* clsid, const void* iid, void** object) {
  GetClassObjectFn f = (GetClassObjectFn)Target("DllGetClassObject");
  if (object) *object = 0;
  return f ? f(clsid, iid, object) : WHA_CLASS_E_CLASSNOTAVAILABLE;
}
HRESULT __stdcall DllCanUnloadNow(void) { return WHA_S_FALSE; }  /* the x64 DLL stays loaded */
HRESULT __stdcall DllRegisterServer(void) {
  NoArgFn f = (NoArgFn)Target("DllRegisterServer");
  return f ? f() : WHA_E_FAIL;
}
HRESULT __stdcall DllUnregisterServer(void) {
  NoArgFn f = (NoArgFn)Target("DllUnregisterServer");
  return f ? f() : WHA_E_FAIL;
}
#ifdef WHA_FWD_MASTER
int __stdcall WHAGetMasterStats(void* out) {
  StatsFn f = (StatsFn)Target("WHAGetMasterStats");
  return f ? f(out) : -1;
}
#endif

#else  /* ARM64 half */

#define WHA_STR2(x) #x
#define WHA_STR(x) WHA_STR2(x)
#define WHA_FORWARD(name) __pragma(comment(linker, "/export:" #name "=" WHA_STR(WHA_FWD_TARGET) "." #name ",PRIVATE"))

WHA_FORWARD(DllGetClassObject)
WHA_FORWARD(DllCanUnloadNow)
WHA_FORWARD(DllRegisterServer)
WHA_FORWARD(DllUnregisterServer)
#ifdef WHA_FWD_MASTER
WHA_FORWARD(WHAGetMasterStats)
#endif

int wha_arm64x_forwarder_placeholder;

#endif
