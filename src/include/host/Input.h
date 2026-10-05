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

// DropFreezeWorld takes the engine's menu type by pointer only. Forward-declare
// rather than include <RE/Skyrim.h> here: this header is on the include path of
// TUs that must not pull a Windows API header ahead of CommonLib (the ordering
// rule in the PCH), and a pointer parameter needs only the declaration.
namespace RE {
    class IMenu;
}

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

// ── The 0.31.0 world freeze ────────────────────────────────────────────────
// Applies kFreezeFrameBackground to the LIVE focus menu so the engine shows a
// frozen frame instead of rendering the 3D world. Game thread only (it walks
// the engine's menu map and reads ui->GameIsPaused()).
//
// Invariant 18, and the reason this is not a general helper: the flag is set
// ONLY while our menu carries kPausesGame AND the game is paused, and it is
// dropped BEFORE the pause on every unpause. A freeze over a running game hangs
// it. Never add kTopmostRenderedMenu -- menus under it stop drawing, including
// MagelightOverlay's PostDisplay, so the page falls back to Present, which
// frame generation drops. Never on VR.
//
// `why` must be a string literal: these run on paths that must not allocate.
// Callers: SetUIMode (entry and exit), SetUIModePause (both edges),
// SwitchUIModeView, SetViewFreezeWorld, and the render pass's reconcile tick.
void ApplyFreezeWorld(const char* why);
void DropFreezeWorld(RE::IMenu* menu, const char* why);
// What the UI-mode view asked for, ignoring whether the pause currently allows
// it. Any thread (takes s_viewsMutex).
bool UiViewWantsFreeze();
// True when the MagelightFocus menu currently carries kFreezeFrameBackground —
// i.e. the world freeze is ON right now. This is the OBSERVABILITY half of
// invariant 18, and it lives here rather than in Render.cpp because
// MagelightFocusMenu is deliberately TU-private to Input.cpp (the window proc,
// the menu, UI mode and the sinks share one lock and one GameTask::Scope
// domain). Any thread that can call into the engine's UI; the flag is a plain
// bool read, so call it on the game thread like the rest of the menu work.
bool FocusMenuFreezeActive();

// Hide/unhide the CursorMenu's SPRITE by setting the movie root's _alpha and
// _visible. `hide` true HIDES — the opposite sense to MenuCursor's
// SetCursorVisibility, which is a SHOW flag; do not pass one to the other.
//
// MenuCursor::SetCursorVisibility hides the WINDOWS cursor only. The sprite is
// the menu's Scaleform movie, and the engine re-shows that movie every frame, so
// it must be re-asserted rather than set once: the render pass does it at UI
// frame 30 (the movie does not exist until the queued show has processed) and
// CursorMenuWatchdog does it every tick after that. Game thread.
void HideVanillaCursor(bool hide);
// Logs the Windows cursor's state after a UI-mode switch (first entry/exit, then stray exits). Game thread.
void SampleCursorState(bool on);

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