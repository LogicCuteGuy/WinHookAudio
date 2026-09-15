# 11: Panel INPUTS 512 — ListClipper + Filter + Loop

**What to build:** `INPUTS 512` tab with `ListClipper` `512` rows `20` drawn, `Filter:[All/HW/VIRTUAL/NETWORK/BRIDGE1..4]` `Search`, `+Add`/`+Insert Empty Above/Below`/`Duplicate`/`X`/`::` drag `memmove` `INPUT` only, `Loop` checkbox (`VIRTUAL` only, grey otherwise), `En` `X`.

**Blocked by:** None (can start immediately).

**Status:** ready-for-agent

- [ ] `WHAControlPanelUI.cpp` `INPUTS 512` (`masterIn[512]` only) `ListClipper` `512` rows `20` drawn, `Filter`/`Search`, `::` drag `memmove` `INPUT` only, `+Insert Empty Above/Below` `+Add` `X` `Duplicate`, `Loop` checkbox (`VIRTUAL` only, grey otherwise), `En` `X`, `"- empty -"` grey for `SLOT_NONE`
- [ ] `ImGui 1.90 + Win32 + DX11` vendored under `third_party/imgui/` per ADR 0008, `.gitignore` payload, stub for offline build, `WHAControlPanelUI.cpp` one `ImGui` code for Master & Bridge
- [ ] Offline `ctest` green without admin/DAW/device/network, `stream_verified:false`, `slots.json` round-trip not yet (11 is UI only)
