/* ARM64X forwarder (ADR 0016): no code, only exports forwarding to WHA_FWD_TARGET (a DLL name without
   ".dll"). Built once per half: the ARM64 object's exports go to the native export table (target
   ...ASIOARM64), the ARM64EC object's to the one x64 processes see (target ...ASIO64).
   The forwards are object-file directives: in a .def or on the link command line this linker takes
   "name=dll.export" as a forward only if it can resolve the export at link time, which it cannot. */
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
