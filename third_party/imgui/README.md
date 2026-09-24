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

Copy the whole release tree (`imgui_draw.cpp`, `imgui_tables.cpp`, `imgui_widgets.cpp` and the `.cpp` backends are compiled too). CMake builds static `wha_imgui` with `IMGUI_USER_CONFIG="wha_imconfig.h"` (`common/panel/`: counted `IM_ASSERT`, thread-local context per popup), adds `WHAControlPanelView.cpp` + `WHAControlPanelWindow.cpp` to both driver DLLs and defines `WHA_HAVE_IMGUI=1`. Override with `-DWHA_IMGUI_DIR=`.

## Offline build

`wha_common` + `abi-check` + `probe` + `host-sample` + `worker-test` + `ks-test` + `panel-test` build without ImGui (`stream_verified:false`): `controlPanel()` returns `ASE_OK` with no popup.

This directory is `.gitignore`d except for this README.
