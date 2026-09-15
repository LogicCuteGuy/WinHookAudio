# ImGui 1.90

Dear ImGui 1.90 is required for `WHAControlPanelUI.cpp` (`INPUTS 512 | OUTPUTS 512 | GENERAL | ABOUT` with `ListClipper`).

## Fetch

Download from https://github.com/ocornut/imgui (tag `v1.90`) and extract so that:

```
third_party/imgui/imgui.h
third_party/imgui/imgui.cpp
third_party/imgui/backends/imgui_impl_win32.h
third_party/imgui/backends/imgui_impl_dx11.h
```

## Offline build

`wha_common` + `abi-check` + `probe` + `host-sample` + `worker-test` + `ks-test` + `panel-test` build without ImGui via stub (`stream_verified:false`).

This directory is `.gitignore`d except for this README.
