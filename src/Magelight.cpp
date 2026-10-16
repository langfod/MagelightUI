// Magelight UI - the host shell. See Magelight.h for the threading model.
//
// The include list is deliberately short: this file is 429 lines of UI-mode
// entry points, the version gate and the VR keyboard delivery hook, and it
// carried 60 includes when it was the 6123-line original. Every header below
// is one the 429 lines NAME in code - not merely one that happens to compile
// without it:
//
//   Magelight.h          ViewId, UIModeExitFn, the public surface contract
//   RE/Skyrim.h          RE::UI (SetUIModePause's menu-flag flip)
//   SKSE/SKSE.h          SKSE::log, everywhere
//   windows.h            WPARAM / LPARAM / UINT + MapVirtualKeyW in the VR hook
//   host/State.h         the shared statics: s_focused, s_renderDead,
//                        s_runtimeDir, s_worldReady, s_toggleKey, s_devMode,
//                        s_focusMenuPause, s_toggleView, s_uiModeView
//   host/Views.h         MlView, s_viewsMutex, FindViewLocked, CreateView,
//                        ShowView, SetViewLayer, RegisterJSListener, InvokeJS,
//                        QueueInput, IsViewValid, InteropCall
//   host/Input.h         SetUIMode, kFocusMenuName, RegisterUIModeExitCallback
//   MagelightApi4.h      Api4::TooOldJson (the version gate)
//   MagelightGameTask.h  GameTask::Post - every entry point marshals through it
//   MagelightVR.h        VR::IsLive (the keyboard delivery gate)
//   <atomic> <mutex> <string> <filesystem>
//
// What was DELETED, and why each was safe: none of the 46 removed headers
// names a symbol this file uses. Each went away with the code that needed it -
// the D3D / MinHook / dxgi surface to host/Hooks.cpp and host/Render.cpp, the
// IME to host/Input.cpp, the Ultralight / AppCore surface to host/Draw.cpp, the
// Magelight.json parser to host/Hooks.cpp. 60 includes -> 14.
//
// Do NOT re-add an include just because a symbol compiles without it. That is
// transitive luck, and it is exactly why RE/Skyrim.h and SKSE/SKSE.h stay even
// though cmake/PCH.h also provides them: removing those two would make this
// file depend on a PCH detail it does not own.
//
// KNOWN LOOSE END: std::uint32_t / std::uint64_t / std::error_code are used
// below and arrive transitively rather than through a direct <cstdint> /
// <system_error>. Left as-is so this change stays purely subtractive; worth
// tightening in a follow-up.

#include "Magelight.h"

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
#include <windows.h>

#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>

#include "MagelightApi4.h"
#include "MagelightGameTask.h"  // after CommonLib, which must precede windows.h
#include "MagelightVR.h"
#include "host/Input.h"         // SetUIMode, the window procs, the focus menu name (stage 4.6)
#include "host/State.h"         // the host's shared state, defined in src/host/*.cpp
#include "host/Views.h"         // the view + image registries (s_views, s_images, MlView)

namespace Magelight {

    // ── Host settings (Magelight.json; all optional) ────────────────────────
    // Location: My Games/Skyrim Special Edition/SKSE/Magelight.json (user
    // data, install-independent — beside the log and the cache), falling back
    // to the runtime dir for hand-installed setups.
    // {
    //   "toggleKey": 201,     // DirectInput scancode for the UI-mode toggle
    //   "demoViews": false,   // create the badge + playground demo views
    //   "imageProbe": false   // create the ImageSource probe page (views/probe)
    //   "devMode": false      // hot reload every v4 page folder, JS error overlay (reload/inspector keys unbound for now)
    //   "fontHinting": "normal", "fontGamma": 1.8   // text rasterization: smooth|normal|monochrome|none, gamma 1.0-3.0
    //   "vr": { "enabled": true, "submitViews": true, "mirror": true, "alpha": "straight",
    //            "beam": true, "beamAlpha": 0.55, "cursorScale": 0.012, "cursorDot": true,   // laser-end pointer: a point (0.26.12); false
    //            = the arrow art "suppressRuntimeLaser": true, "aimUseTip": true, "aimPitchDeg": -35, "runtimeKeyboard": false, "keyboard":
    //            true, "panelWidth": 1600, "panelHeight": 900,   // fullscreen views in VR (NOT the mirror's aspect) "panelFollow": false,
    //            // true = panels glide back to centre when you look away "panelDistanceM": 1.2, "panelWidthM": 1.6, "panelHeightOffsetM":
    //            -0.1,  // the page layer's quad "followAngleDeg": 45, "followMinDeg": 80, "followDistM": 0.5 }
    //                                                  // lazy-follow deadzone: margin past the panel EDGE,
    //                                                  // with an absolute floor in degrees
    //                                                  // Skyrim VR presenter (docs/VR_PRESENTER.md)
    //   "logLevel": "info"    // trace | debug | info | warn | error — Magelight.log verbosity
    //   "stallWatchdog": true, "stallThresholdMs": 1500   // 0.28.3: log the stalled thread's stack when no
    //                                                     // frame presents this long (250-60000)
    //   "hotkeys": { "ModId/viewName": "F7", "Other/hud": 0 }   // rebind (or 0 = disable) any mod's hotkey
    // }
    // Toggle key default: PAGE UP (201). Moved off Home in 0.3.1: Home sits
    // in hotkey territory other UI mods use, and two systems doing their
    // cursor/controls/window dance in the same instant is a crash recipe we
    // don't need to be part of. demoViews defaults FALSE now that a real
    // consumer (SeverActions) rides the host — the always-on badge and the
    // Page Up playground are dev surfaces, not something end users should
    // see; flip the flag to field-test the host by itself.
    //
    // s_toggleKey and s_demoViews are DEFINED in src/host/Input.cpp (stage
    // 4.6) and declared in host/State.h. They used to be `static` here, and
    // declaring them extern in State.h without deleting these copies is the
    // C4211 trap: MSVC keeps the ORIGINAL external linkage, both TUs emit
    // external definitions, the code links, and the two objects carry
    // separate storage (fixed 2026-10-03, commit 2c41fa7 — do not re-add
    // them here).
    // (LoadHostSettings is defined after s_runtimeDir, below.)

    // ── State ───────────────────────────────────────────────────────────────
    // The lifecycle/diagnostic statics (s_runtimeReady, s_worldReady, the thread
    // ids, the stall-watchdog stamps, s_runtimeDir) live in
    // src/host/Telemetry.cpp and are declared in src/host/State.h — Telemetry
    // is the only code that runs before the renderer exists.
    //
    // The D3D resources and the renderer (s_origPresent, s_device, s_context,
    // the shader pair, the blend states, the cursor/dot textures, the cursor
    // position and backbuffer size) live in src/host/Render.cpp, declared in
    // src/host/Render.h. They moved out before the render section itself did:
    // the UI-pass composite, the input sink and the VR presenter all draw with
    // them, so one definition had to become shared before any of those three
    // could move.

    // IME (0.27.0): native composition for CJK input ──────────────────
    // The window's IME context is ATTACHED only while a text field in the
    // UI-mode view has focus (the bridge's '__textfocus' signal) and DETACHED
    // otherwise, so gameplay keys never raise a composition. The pre-edit is
    // drawn INLINE in the field (it rides as the field's selection, so the
    // commit — delivered as ordinary characters — replaces it and a cancel
    // deletes it), the OS candidate list is placed at the caret the page
    // reports, and keys the IME consumed (VK_PROCESSKEY) never reach the
    // page. Text-field focus is detected automatically from the page's own
    // focus events (focusin / focusout on real inputs), so a page needs no
    // focus signalling of its own. VR is exempt: the OS IME UI is invisible in a headset
    // and the runtime keyboard owns text there.
    // s_runtimeDir is declared in host/State.h and defined in host/Telemetry.cpp.
    // ── GPU backend (MagelightGPU.dll — optional; CPU surface is the fallback)
    // Resolved via GetProcAddress after the runtime preload; the typedefs and
    // the s_gpu* pointers now live in src/host/State.h and Telemetry.cpp, which
    // is where the backend is bound. See gpu/MagelightGpuApi.h for the contract.

    // ── View registry + JS <-> C++ bridge (per-view) ───────────────────────
    // The registry itself — MlView, s_views, s_viewsMutex and the lock-held
    // finders — lives in host/Views.h and is defined in host/Views.cpp. It is
    // declared there rather than here because eight other TUs reach into it;
    // one copy in an anonymous namespace would give each of them its own.
    // What remains in this file is the BEHAVIOUR: the dispatcher and its
    // per-listener window shims (JS -> C++), and InteropCall's queue
    // (C++ -> JS).
    //
    // JS -> C++: one native dispatcher (__mlNative) is exposed to each page;
    // per-listener window shims forward (viewId, name, arg) to it — installed
    // at OnWindowObjectReady, BEFORE the page's own scripts run (the contract
    // SeverActions' frontend assumes).
    // C++ -> JS: InteropCall queues {fn, arg} on the view; the render thread
    // drains per frame and calls window.fn(arg) through JavaScriptCore with
    // the payload as a REAL string argument (no JS-source inlining).
    //
    // s_viewsMutex must NOT be held by any of it: the dispatcher runs page JS
    // and the drain calls window.fn, so a lock across either would re-enter
    // through a listener (invariant 16).

    // Render thread (JS execution context). Handlers fire HERE — game-state
    // work must marshal via GameTask::Post (MagelightGameTask.h).
    // Args: (viewIdString, listenerName, payload).
    // s_hwnd (the game window) is declared in host/State.h — the platform
    // bridges open the clipboard against it.

    // LogSehAndDisable is declared in host/State.h (stage 4.6); its definition
    // is below, with the render pass that shares the filter.
    void ToggleVisible() {
        // UI-mode toggle (visible + focused + cursor + suspended game
        // controls, all together). Marshalled to the game thread — the sink
        // fires on the input-dispatch path. With no UI-mode view registered
        // (demo views off, no consumer called SetUIModeView) entering would
        // suspend controls over an empty screen — refuse instead. Exits
        // always go through so the key can never strand a stuck mode.
        GameTask::Post([]() {
            if (s_focused.load()) {
                SetUIMode(false);  // exit always works — never a stuck mode
                return;
            }
            const ViewId target = static_cast<ViewId>(s_toggleView.load());
            if (!IsViewValid(target)) {
                SKSE::log::info("Magelight: toggle ignored — no toggle view registered");
                return;
            }
            s_uiModeView.store(target);
            {
                std::lock_guard<std::mutex> lk(s_viewsMutex);
                QueueKeyboardThemeLocked();   // the hidden keyboard page takes this view's theme ahead of a show (0.31.9)
            }
            SetUIMode(true);
        });
    }

    void ForceExitUIMode() {
        // Safety valve for load boundaries: never carry a suspended-controls /
        // cursor-up state across a save load. Game thread.
        SetUIMode(false);
    }

    void EnterUIMode(ViewId view) { EnterUIModeEx(view, false); }

    void EnterUIModeEx(ViewId view, bool pauseGame, bool noTextEntry) {
        GameTask::Post([view, pauseGame, noTextEntry]() {
            if (s_focused.load()) {
                // One surface at a time — callers gate on IsUIModeActive()
                // (SA's mutual-exclusion contract); refuse rather than steal.
                SKSE::log::warn("Magelight: EnterUIMode({}) refused — UI mode already active on view {}", view, s_uiModeView.load());
                Emit(HostEvent::UIModeRefused, view, 0, 0, "UI mode already active");
                return;
            }
            const char* refusal = nullptr;
            {
                std::lock_guard<std::mutex> lk(s_viewsMutex);
                MlView* v = FindViewLocked(view);
                if (!v || v->destroyPending)
                    refusal = "no such view";
                else if (v->clickThrough)
                    refusal = "click-through views cannot take UI mode";
            }
            if (refusal) {
                SKSE::log::warn("Magelight: EnterUIMode({}) — {}", view, refusal);
                Emit(HostEvent::UIModeRefused, view, 0, 0, refusal);
                return;
            }
            s_uiModeView.store(view);
            {
                std::lock_guard<std::mutex> lk(s_viewsMutex);
                QueueKeyboardThemeLocked();   // the hidden keyboard page takes this view's theme ahead of a show (0.31.9)
            }
            SetUIMode(true, true, pauseGame, noTextEntry);
        });
    }

    void ExitUIMode() {
        // Deliberately does NOT hide the view (Magelight.h contract — SA's
        // Free Look keeps the view rendered while the game owns input;
        // callers that want a hide pair this with ShowView(view, false)).
        GameTask::Post([]() { SetUIMode(false, /*hideView=*/false); });
    }

    bool IsUIModeActive()
    {
        return s_focused.load() && !s_renderDead.load();
    }

    ViewId GetUIModeView()
    {
        return static_cast<ViewId>(s_uiModeView.load());
    }

    std::uint32_t GetToggleKey()
    {
        return s_toggleKey.load();
    }

    std::filesystem::path RuntimeDirPath()
    {
        return s_runtimeDir;
    }

    std::filesystem::path CacheDirPath()
    {
        if (auto dir = SKSE::log::log_directory()) return *dir / "Magelight-cache";
        return {};
    }

    bool DevModeEnabled()
    {
        return s_devMode.load();
    }

    bool InspectorAvailable()
    {
        std::error_code ec;
        return std::filesystem::exists(s_runtimeDir / "inspector" / "Main.html", ec);
    }

    // ── Version gate ──────────────────────────────────────────────────────
    static std::atomic<std::uint64_t> s_gateView{ 0 };
    static void GateDomReady(ViewId view) { InteropCall(view, "setGate", Api4::TooOldJson()); }
    static void GateDismiss(const char*) { if (const ViewId v = static_cast<ViewId>(s_gateView.load())) ShowView(v, false); }

    void ShowVersionGateIfNeeded()
    {
        static bool shown = false;
        if (shown) return;
        const std::string json = Api4::TooOldJson();
        if (json.empty()) return;
        shown = true;
        const ViewId id = CreateView("views/gate/index.html", 40, 120, 640, 200, &GateDomReady,
                                     /*clickThrough=*/true, /*startVisible=*/true);
        if (!id) return;
        SetViewLayer(id, 3);
        RegisterJSListener(id, "gateDismiss", &GateDismiss);
        s_gateView.store(id);
        SKSE::log::warn("Magelight: version gate shown — {}", json);
    }

    void SetUIModePause(bool pause)
    {
        // IN PLACE — never hide/re-show the focus menu (0.26.10 did, and the
        // holder's close detection read the CLOSED as the engine taking the
        // menu away: SeverActions' Live Stage auto-closed the whole UI the
        // moment it asked to unpause, field 2026-09-06). The engine's pause is
        // UI::numPausesGame, bumped by menus carrying kPausesGame on open and
        // dropped on close — so flip the flag on the LIVE menu and move the
        // counter by hand; the eventual close then balances against the flag
        // it sees at that moment.
        const bool was = s_focusMenuPause.exchange(pause);
        if (!s_focused.load() || was == pause) return;   // unfocused: the next Create picks it up
        // The RE::UI part on the GAME thread (review 2026-09-09): RequestUIMode
        // is any-thread by contract and its sibling posts a task; walking the
        // menu map and moving numPausesGame from a worker thread would race the
        // engine's own open/close bookkeeping. Queued AFTER a view switch on the
        // same request, so the pause lands on the view that ends up holding.
        GameTask::Post([pause]() {
            auto* ui = RE::UI::GetSingleton();
            if (!ui) return;
            auto menu = ui->GetMenu(kFocusMenuName);
            if (!menu) { SKSE::log::info("Magelight: UI mode pause -> {} (focus menu not open; next show carries it)", pause); return; }
            using F = RE::UI_MENU_FLAGS;
            const bool has = menu->menuFlags.all(F::kPausesGame);
            if (pause && !has) {
                menu->menuFlags.set(F::kPausesGame);
                ++ui->numPausesGame;
            } else if (!pause && has) {
                // 0.31.0 / invariant 18: drop the world freeze BEFORE the pause.
                // A freeze left on over a running game hangs it, so the order here
                // is load-bearing, not cosmetic.
                DropFreezeWorld(menu.get(), "pause off");
                menu->menuFlags.reset(F::kPausesGame);
                if (ui->numPausesGame > 0) --ui->numPausesGame;
            }
            SKSE::log::info("Magelight: UI mode pause -> {} in place (numPausesGame={})", pause, ui->numPausesGame);
            if (pause) ApplyFreezeWorld("pause on");
        });
    }

    void SwitchUIModeView(ViewId view)
    {
        GameTask::Post([view]() {
            if (!s_focused.load()) {
                Emit(HostEvent::UIModeRefused, view, 0, 0, "switch: UI mode is not active");
                return;
            }
            const char* refusal = nullptr;
            {
                std::lock_guard<std::mutex> lk(s_viewsMutex);
                MlView* v = FindViewLocked(view);
                if (!v || v->destroyPending) refusal = "switch: no such view";
                else if (v->clickThrough) refusal = "switch: click-through views cannot take UI mode";
            }
            if (refusal) {
                SKSE::log::warn("Magelight: SwitchUIModeView({}) — {}", view, refusal);
                Emit(HostEvent::UIModeRefused, view, 0, 0, refusal);
                return;
            }
            const ViewId old = static_cast<ViewId>(s_uiModeView.load());
            if (old == view) { Emit(HostEvent::UIModeSwitched, view); return; }
            s_uiModeView.store(view);
            {
                std::lock_guard<std::mutex> lk(s_viewsMutex);
                QueueKeyboardThemeLocked();   // the keyboard page, up or not, follows the new view's theme (0.31.9)
            }
            ShowView(view, true);
            // 0.31.0: the freeze follows the new view's preference. The inspector
            // has none (UiViewWantsFreeze reads the view's own flag), so a switch
            // to it drops the freeze.
            ApplyFreezeWorld("view switch");
            QueueInput(0, 1, 0);   // re-run the focus marker: key focus follows s_uiModeView
            InvokeJS(old, "window.magelight&&window.magelight._dispatch('__uimode','0');");
            InvokeJS(view, "window.magelight&&window.magelight._dispatch('__uimode','1');");
            SKSE::log::info("Magelight: UI mode switched from view {} to view {}", old, view);
            Emit(HostEvent::UIModeSwitched, view);
        });
    }

    void SetUIModeExitCallback(UIModeExitFn cb)
    {
        // Registrations accumulate (one per function pointer); v1-v3 have
        // no unregister — the v4 per-mod event sink replaces this surface.
        // The registry and its mutex live in src/host/Input.cpp (stage 4.6),
        // which also invokes them.
        RegisterUIModeExitCallback(cb);
    }

    void NotifyWorldReady()
    {
        if (!s_worldReady.exchange(true)) {
            SKSE::log::info("Magelight: world ready — Ultralight will initialize on the next in-game frame");
        }
    }

    // ── 0.31.0 host settings accessors ───────────────────────────────────
    // Thin readers over the config flags LoadHostSettings filled in. They are
    // functions rather than direct extern reads so the "unavailable" decision is
    // made in ONE place: VR is excluded as a precaution, because the freeze frame
    // may never be rendered in a headset.

    bool FreezeWorldAvailable()
    {
        return !REL::Module::IsVR() && s_freezeWorldCfg.load();
    }

    int ConsoleLogLevel()
    {
        const int cfg = s_consoleLogCfg.load();
        return cfg >= 0 ? cfg : (s_devMode.load() ? kConsoleLogAll : kConsoleLogWarnings);
    }

    bool LoadStaggerEnabled() { return s_loadStagger.load(); }

    void SetViewFreezeWorld(ViewId view, bool freeze)
    {
        {
            std::lock_guard<std::mutex> lk(s_viewsMutex);
            MlView* v = FindViewLocked(view);
            if (!v || v->freezeWorld == freeze) return;
            v->freezeWorld = freeze;
        }
        SKSE::log::info("Magelight: view {} world freeze {}", view, freeze ? "requested" : "withdrawn");
        if (s_focused.load() && static_cast<ViewId>(s_uiModeView.load()) == view)
            GameTask::Post([]() { ApplyFreezeWorld("view preference"); });
    }

    // 0.31.0: decides only a first load that has not started.
    bool SetViewLoadOnShow(ViewId view, bool onShow)
    {
        bool started = false;
        {
            std::lock_guard<std::mutex> lk(s_viewsMutex);
            MlView* v = FindViewLocked(view);
            if (!v || v->destroyPending) return false;
            if (v->loadOnShow == onShow) return true;
            v->loadOnShow = onShow;
            started = v->ul || v->dormant;   // a page already loaded (a hibernated one wakes on show anyway)
        }
        if (started)
            SKSE::log::info("Magelight: view {} load-on-show {} - its page already loaded, nothing changes", view,
                            onShow ? "set" : "cleared");
        else
            SKSE::log::info("Magelight: view {} load-on-show {}", view,
                            onShow ? "set - first load waits for a show" : "cleared");
        return true;
    }

}  // namespace Magelight

// ── VR keyboard delivery hook (optional) ────────────────────────────────────
// The host's own on-panel VR keyboard (raised on text-field focus), the
// SteamVR overlay keyboard, and OpenComposite's WM_OC_CHAR window message all
// type into a focused page already — this export needs NONE of them and is a
// generic hook on top: an external tool (a custom keyboard, an accessibility
// aid, a VR runtime that prefers a named entry point) can resolve
// GetProcAddress(GetModuleHandle("Magelight.dll"), "Magelight_DeliverChar" /
// "Magelight_DeliverVKey") and feed characters in (printable chars as wchar_t;
// Backspace/Enter/arrows as a Windows VK). They land on the host's ordinary
// queued-input path (the same one WM_CHAR / WM_KEYDOWN take from the window
// proc), on the UI-mode view, exactly like typed keys. Gated: only while a
// page is focused AND the VR presenter is live; dropped otherwise. Any thread
// (the queue is mutex-guarded).
namespace {
    void DeliverCharImpl(wchar_t ch)
    {
        if (!Magelight::VR::IsLive()) return;
        if (!Magelight::s_focused.load() || Magelight::s_renderDead.load()) return;
        if (ch < 0x20 && ch != L'\t' && ch != L'\r' && ch != L'\n') return;
        // RawKeyDown (unknown vk) -> Char -> KeyUp.
        Magelight::QueueInput(WM_KEYDOWN, 0, 0);
        Magelight::QueueInput(WM_CHAR, static_cast<WPARAM>(ch), 0);
        Magelight::QueueInput(WM_KEYUP, 0, static_cast<LPARAM>(1u << 30) | static_cast<LPARAM>(1u << 31));
    }
    void DeliverVKeyImpl(int vk)
    {
        if (!Magelight::VR::IsLive()) return;
        if (!Magelight::s_focused.load() || Magelight::s_renderDead.load()) return;
        if (vk <= 0 || vk > 0xFF) return;
        const LPARAM scan = static_cast<LPARAM>(MapVirtualKeyW(static_cast<UINT>(vk), MAPVK_VK_TO_VSC) & 0xFF) << 16;
        Magelight::QueueInput(WM_KEYDOWN, static_cast<WPARAM>(vk), scan);
        Magelight::QueueInput(WM_KEYUP, static_cast<WPARAM>(vk), scan | static_cast<LPARAM>(1u << 30) | static_cast<LPARAM>(1u << 31));
    }
}
extern "C" __declspec(dllexport) void Magelight_DeliverChar(wchar_t ch) { DeliverCharImpl(ch); }
extern "C" __declspec(dllexport) void Magelight_DeliverVKey(int vkCode) { DeliverVKeyImpl(vkCode); }
