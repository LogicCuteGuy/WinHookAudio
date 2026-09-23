#pragma once

// ImGui user config for WinHookAudio (IMGUI_USER_CONFIG).
// IM_ASSERT never aborts inside the DAW process: failures are counted and logged,
// so Release builds and offline tests can still detect misuse (panel-view-test checks the count is 0).

namespace wha {
void ImGuiAssertFailed(const char* expr, const char* file, int line);
}

#define IM_ASSERT(_EXPR) ((_EXPR) ? (void)0 : ::wha::ImGuiAssertFailed(#_EXPR, __FILE__, __LINE__))
#define IMGUI_DISABLE_OBSOLETE_FUNCTIONS

// Each popup thread owns its ImGui context: two driver instances in one DAW process
// (or a test beside a live popup) must not share the process-wide current context.
struct ImGuiContext;
extern thread_local ImGuiContext* WhaImGuiTls;
#define GImGui WhaImGuiTls
