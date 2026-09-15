# Control Panel ImGui DX11 Popup Type 1

Date: 2026-09-16
Status: accepted

## Context
Control Panel needs `INPUTS 512 | OUTPUTS 512 | NETWORK 8 | GENERAL | ABOUT` with `512` rows `ListClipper` `20` drawn, `Filter`/`Search`, `+Add`/`Loop`/`En`, `Per-Thing` `GENERAL`, and `ABOUT`. Datasheet FINAL v10 §8 specifies `Popup Type 1 only (controlPanel() → CreateThread → ImGui DX11 1180×720 TopMost dark #1E1E1E, ListClipper 512 rows → 20 drawn)` and `WHAControlPanelUI.cpp ONE ImGui code — Master & Bridge`. Alternative is `Win32` `ListView`/`PropertySheet` without `ImGui`/`DX11`.

## Decision
Use `ImGui 1.90 + Win32 + DX11` per datasheet. Vendor under `third_party/imgui/` (like `ASIO SDK` per ADR 0007), `.gitignore` payload, document fetch step. `WHAControlPanelUI.cpp` is one `ImGui` code for Master & Bridge (Bridge filtered to `BRIDGE1` only, `GENERAL` read-only `Follows Master`). First slice is `INPUTS 512` + `OUTPUTS 512` + `GENERAL` + `ABOUT` (no `NETWORK` — deferred with `WHAA` `libvorbis`/`UDP`).

## Consequences
- `512` rows `ListClipper` `20` drawn proven without `Win32` rewrite.
- `third_party/imgui/` not redistributed publicly; CI must fetch via documented step.
- `NETWORK` tab deferred with `WHAA` `CODEBOOK`/`jitter` dependency.
- `Worker` added to `CONTEXT.md` per `ADR 0002` (`MasterHolder` is `Worker` in code).
