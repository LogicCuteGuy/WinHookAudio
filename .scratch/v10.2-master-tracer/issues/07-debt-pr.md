# 07: Debt PR — deep Slot Table module

**What to build:** One PR turning `common/` into a deep Slot Table module with one seam: `wha_common INTERFACE`, `constexpr` not `#define`, `std::atomic` not `volatile`, single `32/128` source, central size asserts, `TruncateCopy` helper.

**Blocked by:** None (can start immediately).

**Status:** ready-for-agent

- [ ] `add_library(wha_common INTERFACE)` with `target_include_directories` + `target_compile_features(cxx_std_20)`, `src/abi-check/main.cpp` uses `#include "wha/WHASlotTable.h"` not `../../common/`
- [ ] `constexpr uint32_t kMax=512` etc for `WHA_MAX`/`WHA_BRIDGE_COUNT`/`WHA_NAME_LEN`/`WHA_NET_STREAMS`/`WHA_BRIDGE_CLIENTS`/`WHA_BRIDGE_CHANNELS`/`WHA_BRIDGE_FRAMES`/`WHA_BRIDGE_BUFFERS`/`WHAA_MAGIC`/`WHAA_VERSION`/`WHAA_MAX_PAYLOAD`/`WHAA_CODEBOOK_REDUNDANT`, single `IsValidNetworkChannels` in `WHASlotTable.h` called by `WHAPacket.h`/`WHASlotsJson.h`
- [ ] `std::atomic<int32_t> clientCount`/`ready[4]`/`activeBuf[4]`/`mixedActive` with `fetch_add`/`load`/`store`, 5th client `ASE_NotPresent` race-free, `tanh` stays in header for now
- [ ] Central `static_assert(kSlotTableSize >= sizeof(WHASlotTable))` and `kBridgeSharedSize == sizeof(WHABridgeShared)` or derived `constexpr`, `SetSlotName` `TruncateCopy` helper, `IsBridgeInsidePool` not tautological
- [ ] Offline checks pass with no admin, DAW, device, network, `stream_verified:false`, `ctest` 2/2 green, `13/13` `abi-check` green
