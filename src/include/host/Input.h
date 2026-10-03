#pragma once
// Input — the window proc, UI mode, the IME and the engine-side sinks
// (src/host/Input.cpp, stage 4.6).
//
// WHY THIS FILE EXISTS
// These were three disjoint sections of the one-TU host. They are one
// translation unit ON PURPOSE: the window proc, MagelightFocusMenu, the UI
// pass carrier and the IME share a lock and a GameTask::Scope domain
// (invariant 2), and the device-level key mute in host/KeyboardMute.cpp
// exists because of what this code does to the engine's keyboard device
// (invariant 17). Splitting them further would separate a Scope from its
// posts, which is the failure the invariant is about.
//
// What lives here, and what the rest of the host asks of it. Everything
// below is reached from another TU, so it is declared here rather than
// duplicated: a per-TU copy of any of it would be a second object with
// its own storage, which is the exact defect this whole split exists to
// prevent (see host/State.h's header comment).

#include <Windows.h>

#include <cstdint>

#include "Magelight.h"   // ViewId, UIModeExitFn

namespace Magelight {

// ── Entering and leaving UI mode ───────────────────────────────────────────
// SetUIModeImpl (SEH-guarded by SetUIMode) is the ONE place that changes UI
// mode, and every exit path goes through it — key-driven, load-boundary, the
// focus menu's Cancel, or the render-dead safety valve. It must stay
// SEH-wrapped with no locals-with-destructors inside it (invariant 2), so
// the work lives in the Impl.
//
// `pauseGame` bumps the engine's pause while the focus menu is open;
// `noTextEntry` suppresses the text-entry flag for a page with nothing to
// type into (on VR the raise starts the engine's virtual keyboard, which
// OpenComposite then draws over the page).
void SetUIMode(bool on, bool hideView = true, bool pauseGame = false, bool noTextEntry = false);

// ── The window subclass ────────────────────────────────────────────────────
// Installed by InstallHook (src/host/Hooks.cpp). HookWndProc is the W proc;
// AnsiShimWndProc sits ON TOP of it as an ANSI-callable address, because a
// plugin that subclasses after us with SetWindowLongPtrA and calls the proc
// it replaced directly would otherwise execute a USER32 thunk handle.
LRESULT CALLBACK HookWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
LRESULT CALLBACK AnsiShimWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

// ── The focus menu's name ──────────────────────────────────────────────────
// Read by SetUIModePause (src/app/Api.cpp) to find the menu it pauses in
// place, and by the sinks' CursorMenuWatchdog to re-show it if the engine
// dropped it. It is a constant rather than a function so callers can pass it
// straight to the engine's menu map without linking the whole menu class.
extern const char* const kFocusMenuName;

// ── The UI-mode exit hooks (0.9.1, MULTICAST) ──────────────────────────────
// Every embedding plugin registers its own callback and hears EVERY exit with
// the exiting view, filtering by the views it owns. The 0.9.0 single slot
// silently replaced the previous registrant the moment a second consumer
// loaded. SetUIModeImpl invokes them; SetUIModeExitCallback (src/app/Api.cpp)
// appends through this, so the mutex and the vector live here with both ends.
void RegisterUIModeExitCallback(UIModeExitFn cb);
void InvokeUIModeExitCallbacks(ViewId view);

}  // namespace Magelight