// Input - the window proc, UI mode, the IME and the engine-side sinks (stage 4.6).
//
// ONE translation unit on purpose. The window proc, MagelightFocusMenu, the UI
// pass carrier and the IME share a lock and a GameTask::Scope domain
// (invariant 2), and the mute at src/host/KeyboardMute.cpp exists because of
// what this code does to the engine's keyboard device (invariant 17). Splitting
// them further would separate a Scope from its posts, which is exactly the
// failure the invariant is about.
//
// The three blocks below were moved verbatim from src/Magelight.cpp. They were
// three DISJOINT ranges in that file - the UI-pass composite section sat
// between the first two and stayed behind - which is why the gaps are visible.
//
// Rules these blocks must keep:
//
//   - SetUIMode stays SEH-wrapped with no locals-with-destructors inside it
//     (invariant 2). The work is in SetUIModeImpl.
//   - Every main-thread entry point (the window proc, the sinks, the menu's
//     ProcessMessage) holds a GameTask::Scope, so posts from it go through the
//     pump rather than a raw SKSE AddTask.
//   - While UI mode is on the keyboard device's Poll is skipped
//     (src/host/KeyboardMute.cpp, invariant 17). Do not "helpfully" deliver a
//     key from here instead - that path cannot precede a dispatcher hook.
//
// Cross-TU surface, all declared in src/host/State.h:
//   LogSehAndDisable   the SEH filter shared with the render pass
//   CursorMenuWatchdog game-thread; the render pass posts it
//   EscapeCapturedByUiView, s_origWndProc, s_ansiShimPrev, s_focusMenu*
//                       read by the hooks (InstallHook installs the subclass)
//   RegisterFocusMenu, RegisterOverlayMenu, RegisterInputSink
//                       called from InstallHook

#include "Magelight.h"

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <climits>
#include <cstring>
#include <cwctype>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

#include <windowsx.h>
#include <imm.h>

#include "MagelightApi4.h"
#include "MagelightGameTask.h"
#include "MagelightSound.h"
#include "MagelightVR.h"
#include "host/Input.h"
#include "host/KeyboardMute.h"
#include "host/Platform.h"
#include "host/Render.h"
#include "host/State.h"
#include "host/Views.h"

namespace Magelight {

    // ── Settings this TU owns ─────────────────────────────────────────────
    // Declared in host/State.h with the rest of the JSON settings. The
    // definitions live HERE rather than in Magelight.cpp because this is the
    // TU that reads s_toggleKey (the input sink, on every keyboard event) and
    // Magelight.cpp only writes it. C4211 is why this is not a silent hazard:
    // a `static` copy declared extern elsewhere keeps EXTERNAL linkage under
    // MSVC's extension, so both translation units would emit external
    // definitions, the code would link, and the two objects would carry
    // separate storage (fixed 2026-10-03, commit 2c41fa7).

    // "toggleKey": 201 — Page Up by default. Read from Magelight.json by
    // LoadHostSettings on the game thread, read here on the input-dispatch
    // path, so it is atomic.
    std::atomic<std::uint32_t> s_toggleKey{ 201 };

    // "demoViews": false — the playground and badge dev pages. Only read at
    // InstallHook; declared extern so every setting answers one grep.
    bool s_demoViews = false;

    // IME (0.27.0): native composition for CJK input. The context is ATTACHED
    // only while a text field in the UI-mode view has focus, so gameplay keys
    // never raise a composition. Moved here with the IME code in stage 4.6.
    // (The fuller rationale stays with the code below.)
    static constexpr UINT kImeCtlMsg = WM_APP + 0x4F46;   // wparam: 1 attach, 0 detach, 2 reposition candidates
    static std::atomic<bool> s_imeAttached{ false };
    static std::atomic<bool> s_imeComposing{ false };
    static std::atomic<int>  s_imeCaretX{ -1 }, s_imeCaretY{ -1 }, s_imeCaretH{ 0 };   // client px; x<0 = unknown

    // Input, moved verbatim from src/Magelight.cpp. The banner comments are
    // kept deliberately - each records a field-proven constraint.

    WNDPROC               s_origWndProc = nullptr;
    std::atomic<WNDPROC>  s_ansiShimPrev{ nullptr };   // HookWndProc as an A-callable value (AnsiShimWndProc)
    // The SA-port close hook (Magelight.h): fired on the game thread on
    // every UI-mode exit — the host's own menu-close notification.
    // MULTICAST (0.9.1): every embedding plugin registers its own hook and
    // hears every exit with the exiting view, filtering by the views it
    // owns. The 0.9.0 single slot silently replaced the previous
    // registrant the moment a second consumer loaded.
    static std::mutex                s_uiModeExitMutex;
    static std::vector<UIModeExitFn> s_uiModeExitCbs;

    // The append end, for SetUIModeExitCallback in src/app/Api.cpp. Both ends
    // of the registry live here so the mutex has one owner; a public surface
    // that pushed onto a vector this file also reads would need the lock
    // exported, and the lock is the thing most worth keeping private.
    void RegisterUIModeExitCallback(UIModeExitFn cb)
    {
        if (!cb) return;
        std::lock_guard<std::mutex> lock(s_uiModeExitMutex);
        if (std::find(s_uiModeExitCbs.begin(), s_uiModeExitCbs.end(), cb) == s_uiModeExitCbs.end())
            s_uiModeExitCbs.push_back(cb);
    }

    void InvokeUIModeExitCallbacks(ViewId view)
    {
        // Copy under the lock, call outside it: a consumer's callback reaches
        // back into the host (SA's Close bookkeeping shows views, asks for a
        // pause), and holding this mutex across that is a deadlock waiting for
        // the consumer to register something.
        std::vector<UIModeExitFn> cbs;
        {
            std::lock_guard<std::mutex> lock(s_uiModeExitMutex);
            cbs = s_uiModeExitCbs;
        }
        for (UIModeExitFn cb : cbs) cb(view);
    }

    // ── Our own engine menu ─────────────────────────────────────────────
    // Pushed on UI-mode entry, popped on exit. Owning a real IMenu is what
    // makes the engine cooperate with an overlay: the engine drives
    // MenuCursor only while a kUsesCursor menu is topmost (field-found 2026-
    // 09-01 — SA's item-carrier menu froze the cursor the moment it became
    // the only real menu on the stack), kUsesMenuContext puts input into the
    // menu context natively (gameplay controls off, gamepad included), the
    // engine's Cancel user event (Escape / gamepad B) lands in ProcessMessage,
    // MenuOpenCloseEvent fires for anyone sinking menu state, and
    // kPausesGame is a per-open flag — pause-on-open for free. We still draw
    // our own cursor sprite (the engine's is a pre-Present Scaleform menu,
    // always under our overlay) and keep the delta-integration cursor
    // fallback as belt-and-suspenders. The movie is a blank SWF shipped at
    // Data/Interface/magelightfocus.swf; if it's missing the menu is skipped
    // and UI mode runs exactly as before (no engine menu).
    // SetUIMode, EscapeCapturedByUiView and CursorMenuWatchdog are declared in
    // host/State.h (stage 4.6). The `static` forward declarations that used to
    // sit here are gone: they would re-declare an extern symbol as file-local,
    // which MSVC reports as C4211 and which gives this TU its own copy.
    std::atomic<bool> s_focusMenuPause{ false };   // flag for the NEXT open
    bool              s_focusMenuRegistered = false;   // read by Render.cpp's NoteUiPassState (invariant 18 observability)
    // 0.31.0: what the focus menu's kFreezeFrameBackground flag should be. Latched
    // on UI-mode entry so the menu's CONSTRUCTOR can set it during the show (the
    // flag is a per-open one, exactly like s_focusMenuPause above), and read by
    // ApplyFreezeWorld to decide whether a live menu's flag needs changing.
    std::atomic<bool> s_focusMenuFreeze{ false };

    // The focus menu's name as the ENGINE's map keys it. Declared in host/Input.h
    // because src/app/Api.cpp's SetUIModePause looks the menu up by name to
    // pause it in place; the menu class itself stays file-local here.
    const char* const kFocusMenuName = "MagelightFocus";
    static_assert(sizeof(kFocusMenuName) > 1, "the focus menu name must not be empty");

    class MagelightFocusMenu final : public RE::IMenu
    {
    public:
        inline static const char* MENU_NAME = kFocusMenuName;

        MagelightFocusMenu()
        {
            using F = RE::UI_MENU_FLAGS;
            depthPriority = 10;  // above SA's carrier (0), below the CursorMenu
            inputContext  = RE::UserEvents::INPUT_CONTEXT_ID::kMenuMode;
            menuFlags.set(F::kUsesCursor, F::kUsesMenuContext, F::kDontHideCursorWhenTopmost,
                          F::kCustomRendering, F::kAllowSaving);
            if (s_focusMenuPause.load()) menuFlags.set(F::kPausesGame);
            // 0.31.0: the world freeze rides the menu that holds the pause, so an
            // engine-forced close drops both together. See ApplyFreezeWorld below.
            if (s_focusMenuFreeze.load()) menuFlags.set(F::kFreezeFrameBackground);
            if (auto* sm = RE::BSScaleformManager::GetSingleton()) {
                sm->LoadMovie(this, uiMovie, "magelightfocus");
            }
        }

        static RE::IMenu* Create() { return new MagelightFocusMenu(); }

        RE::UI_MESSAGE_RESULTS ProcessMessage(RE::UIMessage& msg) override
        {
            GameTask::Scope pumpScope;   // posts from here go through the pump (MagelightGameTask.h)
            if (msg.type == RE::UI_MESSAGE_TYPE::kUserEvent) {
                if (auto* d = static_cast<RE::BSUIMessageData*>(msg.data)) {
                    if (d->fixedStr == "Cancel") {
                        // Escape / gamepad B through the engine: exit UI mode
                        // (idempotent with the WndProc Escape path). A page
                        // that captured Escape (0.28.0) keeps it on THIS path
                        // too: on VR the keyboard poll is not muted, so a
                        // physical Escape reaches the engine's MenuControls as
                        // Cancel and closed the UI under a modal that had
                        // asked for the key (review 2026-09-09).
                        if (!EscapeCapturedByUiView())
                            GameTask::Post([]() { SetUIMode(false); });
                        return RE::UI_MESSAGE_RESULTS::kHandled;
                    }
                    // Every other menu-mode user event (Backspace, Enter,
                    // arrows, F-keys — the controlmap's 0x10 group) is the
                    // page's business and was already delivered through the
                    // window proc. Swallow it here so it cannot fall through
                    // to whatever engine menu sits beneath us (field
                    // 2026-09-03: the cursor froze right after Backspace).
                    static std::unordered_set<std::string> s_seen;
                    const std::string ev = d->fixedStr.c_str() ? d->fixedStr.c_str() : "";
                    if (s_seen.insert(ev).second)
                        SKSE::log::info("Magelight: menu user event '{}' swallowed by the focus menu", ev);
                    return RE::UI_MESSAGE_RESULTS::kHandled;
                }
            } else if (msg.type == RE::UI_MESSAGE_TYPE::kHide) {
                // Engine-initiated close (loading screen, force-close): UI
                // mode must follow. Our own SetUIModeImpl(false) clears
                // s_focused BEFORE queuing this hide, so it never re-enters.
                if (s_focused.load()) {
                    GameTask::Post([]() { SetUIMode(false); });
                }
            }
            return RE::IMenu::ProcessMessage(msg);
        }

        void PostDisplay() override {}  // kCustomRendering: nothing to draw

        // ── Skyrim VR object layout (0.17.2) ─────────────────────────────
        // IMenu is 0x30 bytes on SE/AE but 0x40 on VR: the VR engine keeps
        // unk30 (set through the VR-only vfunc 09), unk34 and a
        // BSFixedString menuName at 0x38 on EVERY menu. CommonLibSSE-NG's
        // cross-runtime build leaves those out of the struct and reaches
        // them by relocation (GetVRRuntimeData -> this+0x30), so an object
        // allocated at the SE size hands the VR engine garbage past its end:
        // opening this menu crashed SkyrimVR.exe reading menuName on the
        // first F10 in the headset (2026-09-03, VR-2 field pass). These
        // members reproduce the VR tail exactly; on SE they are dead weight
        // the engine never looks at.
        REX::EnumSet<RE::UI_MENU_Unk09, std::uint32_t> vrUnk30{ RE::UI_MENU_Unk09::kNone };   // 30
        std::byte                                      vrUnk34{ 1 };                          // 34
        RE::BSFixedString                              vrMenuName{ MENU_NAME };               // 38
    };
    static_assert(sizeof(MagelightFocusMenu) == 0x40, "MagelightFocusMenu must match the VR IMenu size (0x40)");

    void RegisterFocusMenu()
    {
        std::error_code ec;
        const auto swf = GameRoot() / L"Data" / L"Interface" / L"magelightfocus.swf";
        if (!std::filesystem::exists(swf, ec)) {
            SKSE::log::warn("Magelight: {} missing — UI mode runs without an engine menu",
                swf.string());
            return;
        }
        if (auto* ui = RE::UI::GetSingleton()) {
            ui->Register(MagelightFocusMenu::MENU_NAME, MagelightFocusMenu::Create);
            s_focusMenuRegistered = true;
            SKSE::log::info("Magelight: engine menu '{}' registered", MagelightFocusMenu::MENU_NAME);
        }
    }

    // ── 0.31.0 world freeze ────────────────────────────────────────────
    // kFreezeFrameBackground on the focus menu: the engine shows a frozen frame
    // instead of rendering the 3D world. A freeze over a RUNNING game deadlocks
    // it or pins a stale frame, so it is set only while OUR menu carries
    // kPausesGame AND the game is paused, and it is dropped BEFORE the pause on
    // every unpause (see SetUIModePause in src/Magelight.cpp). It rides the menu
    // that holds the pause, so an engine-forced close takes both together.
    //
    // Never add kTopmostRenderedMenu here: menus under it stop drawing, and
    // MagelightOverlay's PostDisplay (the UI-pass composite) among them, so the
    // page falls back to Present, which frame generation drops. Never on VR —
    // untested there, and the freeze frame may never be rendered in a headset.

    // The UI-mode view asks for the freeze (its preference only). Any thread;
    // takes s_viewsMutex.
    bool UiViewWantsFreeze()
    {
        if (!FreezeWorldAvailable() || !s_focused.load()) return false;
        std::lock_guard<std::mutex> lk(s_viewsMutex);
        const MlView* v = FindViewLocked(static_cast<ViewId>(s_uiModeView.load()));
        return v && v->freezeWorld;
    }

    // Game thread.
    void DropFreezeWorld(RE::IMenu* menu, const char* why)
    {
        using F = RE::UI_MENU_FLAGS;
        if (menu && menu->menuFlags.all(F::kFreezeFrameBackground)) {
            menu->menuFlags.reset(F::kFreezeFrameBackground);
            SKSE::log::info("Magelight: world freeze OFF ({})", why);
        }
    }

    // Game thread. Makes the live focus menu's flag match what the UI-mode view
    bool FocusMenuFreezeActive()
    {
        auto* ui = RE::UI::GetSingleton();
        if (!ui || !s_focusMenuRegistered) return false;
        const auto menu = ui->GetMenu(MagelightFocusMenu::MENU_NAME);
        return menu && menu->menuFlags.all(RE::UI_MENU_FLAGS::kFreezeFrameBackground);
    }

    // wants AND the pause allows. `why` is only ever a string literal, so the
    // log line is allocation-free.
    void ApplyFreezeWorld(const char* why)
    {
        const bool want = UiViewWantsFreeze();
        s_focusMenuFreeze.store(want);   // a focus menu shown later starts with it
        auto* ui = RE::UI::GetSingleton();
        if (!ui || !s_focusMenuRegistered) return;
        const auto menu = ui->GetMenu(MagelightFocusMenu::MENU_NAME);
        if (!menu) return;
        using F = RE::UI_MENU_FLAGS;
        const bool paused = menu->menuFlags.all(F::kPausesGame) && ui->GameIsPaused();
        const bool has = menu->menuFlags.all(F::kFreezeFrameBackground);
        if (want && paused) {
            if (!has) {
                menu->menuFlags.set(F::kFreezeFrameBackground);
                SKSE::log::info("Magelight: world freeze ON (view {}, {})", s_uiModeView.load(), why);
            }
            return;
        }
        if (has && want) {
            // The view wants it but the game is running: another mod cleared the
            // pause. Warn once — the freeze returns with the pause.
            static std::atomic<bool> s_warned{ false };
            if (!s_warned.exchange(true))
                SKSE::log::warn("Magelight: the game is running under the world freeze (another mod cleared the "
                                "pause?) - freeze dropped; it returns with the pause");
        }
        DropFreezeWorld(menu.get(), why);
    }

    // ── Typing on VR: the engine's keyboard device, not the window ──────
    // In a headset the game window almost never holds OS focus, so it receives
    // NO keyboard messages at all — field-verified 2026-09-03: with a page
    // focused and both a physical keyboard and the runtime's virtual keyboard
    // in use, not one WM_CHAR, WM_KEYDOWN or private message arrived. The flat
    // host types through the window proc; on VR that path is simply dead.
    //
    // The SKSE input sink DOES see the engine's keyboard device (that is how
    // Escape and the hotkeys work in VR), so translate its DirectInput
    // scancode into exactly the WM_* the window would have produced and queue
    // it. DrainInputQueue is unchanged. This also picks up any keyboard the
    // RUNTIME synthesises at the driver level (OpenComposite's virtual
    // keyboard sends scancodes alongside its own delivery paths).
    //
    // Modifier state comes from GetAsyncKeyState, which is global and does not
    // need window focus. Called from the input sink (game thread).
    static void QueueScancodeAsText(std::uint32_t scan, bool down)
    {
        if (scan == 0 || scan > 0xFF) return;
        const UINT vk = MapVirtualKeyW(scan, MAPVK_VSC_TO_VK_EX);
        if (!vk) return;
        const LPARAM base = static_cast<LPARAM>(scan) << 16;
        if (!down) {
            QueueInput(WM_KEYUP, vk, base | (1 << 30) | (1 << 31));
            return;
        }
        QueueInput(WM_KEYDOWN, vk, base);
        // Printable? Ask the active layout, with the live modifier state.
        BYTE ks[256]{};
        if (GetAsyncKeyState(VK_SHIFT) & 0x8000)   ks[VK_SHIFT]   = 0x80;
        if (GetAsyncKeyState(VK_CONTROL) & 0x8000) ks[VK_CONTROL] = 0x80;
        if (GetAsyncKeyState(VK_MENU) & 0x8000)    ks[VK_MENU]    = 0x80;
        if (GetKeyState(VK_CAPITAL) & 1)           ks[VK_CAPITAL] = 1;
        wchar_t buf[8]{};
        const int n = ToUnicode(vk, scan, ks, buf, 7, 0);
        for (int i = 0; i < n && i < 7; ++i) {
            const wchar_t ch = buf[i];
            if (ch >= 0x20 || ch == L'\t' || ch == L'\r')
                QueueInput(WM_CHAR, static_cast<WPARAM>(ch), 0);
        }
        static std::atomic<int> s_typeLog{ 0 };
        if (s_typeLog.fetch_add(1) < 12)
            SKSE::log::info("Magelight: VR typing — scancode 0x{:02X} -> vk 0x{:02X}, {} char(s)", scan, vk, n);
    }

    // Public bridge (declared in Magelight.h with stdint types): the VR laser
    // injects WM_MOUSEMOVE / WM_LBUTTON* through the same queue.
    void QueueSyntheticInput(unsigned int msg, std::uintptr_t wparam, std::intptr_t lparam)
    {
        QueueInput(static_cast<UINT>(msg), static_cast<WPARAM>(wparam), static_cast<LPARAM>(lparam));
    }

    // Game thread only (a posted task). Idempotent. Step-logged (0.3.2 diagnostics:
    // the teardown is the crash neighborhood) and SEH-guarded by the caller.
    // hideView applies only to the OFF path: key-driven / load-boundary
    // exits hide the UI-mode view (the demo + SA close behavior); the public
    // ExitUIMode passes false so a view can stay rendered while the game
    // owns input (SA's Free Look).
    static void SetUIModeImpl(bool on, bool hideView, bool pauseGame, bool noTextEntry)
    {
        if (s_focused.load() == on) return;
        const ViewId uiView = static_cast<ViewId>(s_uiModeView.load());
        if (on && !s_rendererUp.load()) {
            // No renderer = nothing to show: entering UI mode here would only
            // suspend the game's controls over an empty screen (field
            // 2026-09-03, the missed-kNewGame session). Refuse, loudly.
            SKSE::log::warn("Magelight: UI mode refused — renderer not initialised yet (world not ready)");
            Emit(HostEvent::UIModeRefused, uiView, 0, 0, "renderer not ready");
            return;
        }
        SKSE::log::info("Magelight: SetUIMode({}{}) begin", on, (on && pauseGame) ? ", paused" : "");
        s_focused.store(on);
        if (!on && s_hwnd) { s_imeCaretX.store(-1); PostMessageW(s_hwnd, kImeCtlMsg, 0, 0); }   // IME follows UI mode off
        // UI mode shows its target view; other views (HUD badges etc.) keep
        // their own visibility.
        if (on) ShowView(uiView, true);
        else if (hideView) ShowView(uiView, false);
        if (!on) ShowOwnKeyboard(false);
        if (!on) {
            // Inspectors live only inside UI mode: Escape on the inspector (or
            // on its page) would otherwise leave it painted over the lower
            // half of the screen with nothing focused (field log 2026-09-02).
            std::vector<ViewId> inspectors;
            {
                std::lock_guard<std::mutex> lk(s_viewsMutex);
                for (auto& vp : s_views) if (vp->isInspector && vp->visible) inspectors.push_back(vp->id);
            }
            for (ViewId i : inspectors) ShowView(i, false);
        }

        if (auto* queue = RE::UIMessageQueue::GetSingleton()) {
            queue->AddMessage(RE::CursorMenu::MENU_NAME,
                on ? RE::UI_MESSAGE_TYPE::kShow : RE::UI_MESSAGE_TYPE::kHide, nullptr);
            // Our engine menu rides the same queue; pause is decided per open
            // (the flag is read by the menu's constructor on show).
            if (s_focusMenuRegistered) {
                if (on) {
                    s_focusMenuPause.store(pauseGame);
                    // 0.31.0: latch the freeze BEFORE the show, so the menu's
                    // constructor carries it into this open (the flag is per-open,
                    // exactly like the pause above).
                    s_focusMenuFreeze.store(UiViewWantsFreeze());
                } else {
                    // s_focused is already off, so UiViewWantsFreeze() is false and
                    // this DROPS the flag — before the hide, and therefore before
                    // the engine can drop the pause under us (invariant 18).
                    ApplyFreezeWorld("UI mode exit");
                }
                queue->AddMessage(MagelightFocusMenu::MENU_NAME,
                    on ? RE::UI_MESSAGE_TYPE::kShow : RE::UI_MESSAGE_TYPE::kHide, nullptr);
            }
        }
        SKSE::log::info("Magelight: cursor-menu {} queued{}", on ? "show" : "hide",
            s_focusMenuRegistered ? " (+ engine menu)" : "");
        // The CursorMenu must be OPEN (it drives MenuCursor position updates)
        // but its sprite draws pre-Present and lands under the overlay — we
        // render our own cursor topmost instead, so hide the vanilla one. The
        // show message above processes async and may reset visibility, so the
        // render thread re-asserts this shortly after focus (see FrameWork).
        if (auto* mc = RE::MenuCursor::GetSingleton()) {
            mc->SetCursorVisibility(!on);
            // The SPRITE is the CursorMenu's movie, not the Windows cursor (see
            // HideVanillaCursor). Hidden here too, so entry/exit is one decision
            // in one place; the watchdog keeps it hidden afterwards.
            // NOTE the polarity flip: HideVanillaCursor(true) HIDES, while
            // SetCursorVisibility(true) SHOWS, so this is `on`, not `!on`. At
            // entry the movie may not exist yet (its show is queued async and
            // this is a no-op then) — that is what the frame-30 re-assert and
            // the watchdog are for.
            HideVanillaCursor(on);
            // MenuCursor has no visibility getter, so this records the INTENT and
            // that it was applied. A report of two cursors, with the 'hidden
            // again' line also present, means the engine un-hid it LATER.
            SKSE::log::info("Magelight: vanilla cursor {} (UI mode {})",
                on ? "hidden - we draw ours on top" : "restored to the engine", on ? "on" : "off");
        } else {
            SKSE::log::warn("Magelight: no MenuCursor singleton - cannot {} the vanilla cursor; "
                            "{} cursors will show", on ? "hide" : "restore", on ? "two" : "no");
        }
        if (auto* cm = RE::ControlMap::GetSingleton()) {
            using UEFlag = RE::ControlMap::UEFlag;
            // Scoped enum without a free operator| — combine the bits directly.
            const auto flags = static_cast<UEFlag>(
                std::to_underlying(UEFlag::kMovement) | std::to_underlying(UEFlag::kLooking) |
                std::to_underlying(UEFlag::kActivate) | std::to_underlying(UEFlag::kMenu) |
                std::to_underlying(UEFlag::kFighting) | std::to_underlying(UEFlag::kPOVSwitch) |
                std::to_underlying(UEFlag::kWheelZoom));
            // storeState=false: a transient toggle that can never bake into a
            // save. Console stays reachable as an escape hatch.
            cm->ToggleControls(flags, !on, false);
            // Text-entry flag while UI mode is on (the engine's own text
            // fields do the same through SkyUI's AllowTextInput). SKSE
            // suppresses Papyrus OnKeyDown while it is raised, and the
            // well-behaved native mods skip their hotkeys too — so a key
            // typed into a page can no longer double as another mod's
            // hotkey (field 2026-09-03: Backspace in a search field opened
            // a third mod's display manager and froze the cursor).
            // Balanced: raised once per focus, lowered once per unfocus.
            // kUIModeFlagNoTextEntry: a page with nothing to type into asks
            // us not to raise it — on Skyrim VR the raise starts the engine's
            // virtual keyboard device, which OpenComposite hooks to raise its
            // own keyboard over the page (field 2026-09-05, SA's quick wheel).
            static bool s_textEntryRaised = false;
            if (on && !s_textEntryRaised && !noTextEntry) {
                cm->AllowTextInput(true);
                s_textEntryRaised = true;
            } else if (!on && s_textEntryRaised) {
                cm->AllowTextInput(false);
                s_textEntryRaised = false;
            }
        }
        SKSE::log::info("Magelight: controls {}", on ? "suspended" : "restored");
        // OpenComposite's own menu laser: its SKSE plugin sets the window
        // property OC_MENU_ACTIVE whenever an engine menu is open (ours
        // counts), and OCU draws a second beam over our panels. Clear it while
        // we hold focus — OCU's own MenuOpenCloseEvent watcher re-asserts it
        // the moment a real menu opens or ours closes, so we never leave it
        // wrong. Their laser could not point at our overlays anyway.
        if (on && s_hwnd && VR::SuppressRuntimeLaser()) {
            SetPropW(s_hwnd, L"OC_MENU_ACTIVE", reinterpret_cast<HANDLE>(static_cast<std::intptr_t>(0)));
            SKSE::log::info("Magelight: cleared OC_MENU_ACTIVE — the runtime's own menu laser stands down");
        }
        QueueInput(0, on ? 1 : 0, 0);  // focus/unfocus marker for the render thread
        SKSE::log::info("Magelight: UI mode {}", on ? "ON (cursor up, game controls suspended)" : "OFF");
        // Close hook: every exit path lands here on the game thread — the
        // embedding mod's (SA's) close bookkeeping hangs off this.
        if (!on) InvokeUIModeExitCallbacks(uiView);
        // 0.29.0: the view's manifest/API open/close sound. Game
        // thread already — play directly, after the exit hooks so a close
        // sound never precedes the consumer's own close bookkeeping.
        {
            std::string snd;
            {
                std::lock_guard<std::mutex> lock(s_viewsMutex);
                if (MlView* v = FindViewLocked(uiView)) snd = on ? v->soundOpen : v->soundClose;
            }
            if (!snd.empty()) Sound::Play(snd, uiView);
        }
        Emit(on ? HostEvent::UIModeEntered : HostEvent::UIModeExited, uiView);
        InvokeJS(uiView, on ? "window.magelight&&window.magelight._dispatch('__uimode','1');"
                            : "window.magelight&&window.magelight._dispatch('__uimode','0');");
    }

    // SEH wrapper — no locals with destructors allowed in here.
    void SetUIMode(bool on, bool hideView, bool pauseGame, bool noTextEntry)
    {
        __try {
            SetUIModeImpl(on, hideView, pauseGame, noTextEntry);
        } __except (LogSehAndDisable("SetUIMode", GetExceptionCode(),
                        (GetExceptionInformation())->ExceptionRecord->ExceptionAddress)) {
        }
    }

    // OpenComposite Unleashed's virtual keyboard posts every character to the
    // GAME WINDOW as a private message (VRKeyboard.cpp: WM_OC_CHAR = WM_APP +
    // 0x4F45; wParam = wchar_t, lParam 0 = printable character, 1 = a virtual
    // key such as Backspace/Enter/Tab/Escape). We already subclass that window,
    // so OCU's keyboard reaches a Magelight page with NO change on their side —
    // their dedicated export path (which resolves another host's DLL by name and can
    // never find us) is not the only road in. Only consumed while a page holds
    // focus; otherwise it passes through to the console/Scaleform as usual.
    static constexpr UINT kOcCharMsg = WM_APP + 0x4F45;

    // Returns true when the message was consumed (swallow at the window proc).
    // Focused-window hotkey path. The engine input sink does not deliver
    // keyboard button events while our menu-context engine menu is up (F10
    // opened the manifest panel every time and never closed it, 2026-09-02),
    // but the window still receives WM_KEYDOWN — that is how the page gets
    // its typing. The lparam scancode is the DirectInput code; extended keys
    // (arrows, Page Up/Down, Home/End/Insert/Delete) carry bit 24 and map to
    // the 0x80+ range. Auto-repeat (bit 30) never re-fires.
    static void PeekHotkey(LPARAM lparam)
    {
        if (lparam & (1 << 30)) return;
        const std::uint32_t scan = static_cast<std::uint32_t>((lparam >> 16) & 0xFF);
        if (!scan) return;
        const std::uint32_t dx = scan | ((lparam & (1 << 24)) ? 0x80u : 0u);
        Api4::DispatchHotkey(dx, "window");
    }

    static std::wstring ImeGetString(HIMC hImc, DWORD what)
    {
        const LONG bytes = ImmGetCompositionStringW(hImc, what, nullptr, 0);
        if (bytes <= 0) return {};
        std::wstring out(static_cast<std::size_t>(bytes) / sizeof(wchar_t), L'\0');
        ImmGetCompositionStringW(hImc, what, out.data(), static_cast<DWORD>(bytes));
        return out;
    }

    // -> the UI-mode page's reserved '__ime' channel ({state, text}); the
    // bridge script draws the pre-edit and re-emits it as 'magelight:ime'.
    //
    // WideToUtf8Lossy, NOT the strict form: composition text legitimately
    // carries an unpaired surrogate while the user is mid-word, and this runs
    // off the window proc where a thrown exception is a crash rather than a
    // diagnostic. It replaced a local copy here that discarded the second
    // WideCharToMultiByte's return value and trusted the sizing call.
    static void ImeNotifyPage(const char* state, const std::wstring& text)
    {
        const std::string json = std::string("{\"state\":\"") + state + "\",\"text\":"
            + MlJsonQuote(WideToUtf8Lossy(text)) + "}";
        InteropCall(static_cast<ViewId>(s_uiModeView.load()), "__ime", json);
    }

    static void ImeRepositionCandidates()   // window thread
    {
        const int x = s_imeCaretX.load(), y = s_imeCaretY.load(), h = s_imeCaretH.load();
        if (x < 0 || !s_hwnd) return;
        if (HIMC hImc = ImmGetContext(s_hwnd)) {
            CANDIDATEFORM cf{};
            cf.dwIndex = 0; cf.dwStyle = CFS_CANDIDATEPOS; cf.ptCurrentPos = { x, y + h };
            ImmSetCandidateWindow(hImc, &cf);
            COMPOSITIONFORM cpf{};
            cpf.dwStyle = CFS_POINT; cpf.ptCurrentPos = { x, y };
            ImmSetCompositionWindow(hImc, &cpf);
            ImmReleaseContext(s_hwnd, hImc);
        }
    }

    static void ImeAttach(bool on)   // window thread only (kImeCtlMsg)
    {
        if (!s_hwnd || s_imeAttached.load() == on) return;
        if (on) {
            const BOOL ok = ImmAssociateContextEx(s_hwnd, nullptr, IACE_DEFAULT);
            // Diagnostics: did the window actually get a context, is the IME open,
            // and is it in a native (hiragana/kana) conversion mode or alphanumeric?
            // Field 2026-09-06: keys reached the page as plain letters with no
            // composition — this line tells 'A mode' apart from 'no context'.
            if (HIMC hImc = ImmGetContext(s_hwnd)) {
                // OPEN the IME: the on/off state is per window, and a freshly
                // attached context starts CLOSED — every key passed through
                // untouched even in hiragana mode (field 2026-09-06 18:00:
                // open=false, conversion=native). Opening on focus means the
                // user never has to hit the IME on/off key inside the game.
                if (!ImmGetOpenStatus(hImc)) ImmSetOpenStatus(hImc, TRUE);
                DWORD conv = 0, sent = 0;
                ImmGetConversionStatus(hImc, &conv, &sent);
                SKSE::log::info("Magelight: IME attach ok={} context=yes open={} conversion=0x{:X} ({})",
                    ok != FALSE, ImmGetOpenStatus(hImc) != FALSE, conv,
                    (conv & IME_CMODE_NATIVE) ? "native/hiragana" : "alphanumeric — composition will not start");
                ImmReleaseContext(s_hwnd, hImc);
            } else {
                SKSE::log::warn("Magelight: IME attach ok={} but the window has NO context — IME disabled on this thread?", ok != FALSE);
            }
        } else {
            if (HIMC hImc = ImmGetContext(s_hwnd)) {
                if (s_imeComposing.load()) ImmNotifyIME(hImc, NI_COMPOSITIONSTR, CPS_CANCEL, 0);
                ImmReleaseContext(s_hwnd, hImc);
            }
            ImmAssociateContext(s_hwnd, nullptr);
            s_imeComposing.store(false);
        }
        s_imeAttached.store(on);
        SKSE::log::info("Magelight: IME context {}", on ? "attached (text field focused)" : "detached");
    }

    // The page's caret, "x,y,h" in its CSS px -> client px of the game window,
    // so the OS candidate list opens at the caret rather than the corner.
    // A page's caret, "x,y,h" in its CSS px -> client px of the game window,
    // so the OS candidate list opens at the caret rather than the corner.
    //
    // External since stage 4.5: the page bridge detects focus and the caret and
    // calls this. Declared in host/Views.h until the IME section splits.
    void ImeOnCaret(ViewId id, const std::string& arg)
    {
        float cx = 0.f, cy = 0.f, ch = 0.f;
        if (sscanf_s(arg.c_str(), "%f,%f,%f", &cx, &cy, &ch) == 3) {
            std::lock_guard<std::mutex> lk(s_viewsMutex);
            if (MlView* v = FindViewLocked(id)) {
                float ex = 0.f, ey = 0.f;
                EffectivePos(*v, static_cast<float>(s_backbufferW.load()), static_cast<float>(s_backbufferH.load()), ex, ey);
                const float ds = v->deviceScale > 0.f ? v->deviceScale : 1.f;
                s_imeCaretX.store(static_cast<int>(ex + cx * ds));
                s_imeCaretY.store(static_cast<int>(ey + cy * ds));
                s_imeCaretH.store(static_cast<int>(ch * ds));
            }
        }
        if (s_imeComposing.load() && s_hwnd) PostMessageW(s_hwnd, kImeCtlMsg, 2, 0);
    }

    // The window's IME context follows text-field focus in the UI-mode view
    // (the caller filters other views); on VR the runtime keyboard owns text
    // and the OS IME UI is invisible.
    //
    // External since stage 4.5: the page bridge detects focus and calls this.
    // Declared in host/Views.h until the IME section splits into its own TU.
    void ImeOnTextFocus(bool focused)
    {
        if (VR::IsLive() || !s_hwnd) return;
        if (!focused) s_imeCaretX.store(-1);
        PostMessageW(s_hwnd, kImeCtlMsg, focused ? 1 : 0, 0);
    }

    // 0.28.0: does the UI-mode page own Escape right now? (SetEscapeCapture /
    // the '__escapecapture' channel — a modal or an editing control closes
    // itself instead of the whole UI.)
    bool EscapeCapturedByUiView()
    {
        std::lock_guard<std::mutex> lk(s_viewsMutex);
        const MlView* v = FindViewLocked(static_cast<ViewId>(s_uiModeView.load()));
        return v && v->escapeCapture;
    }

    static bool HandleFocusedMsg(UINT msg, WPARAM wparam, LPARAM lparam)
    {
        switch (msg) {
        case WM_MOUSEMOVE:
        case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDOWN: case WM_RBUTTONUP:
        case WM_MBUTTONDOWN: case WM_MBUTTONUP:
        case WM_MOUSEWHEEL:
            // Swallow but do NOT queue: mouse rides the SKSE sink + MenuCursor
            // (0.3.5) — Skyrim's DirectInput never delivers WM mouse messages
            // here anyway (zero arrived across every field run), and queueing
            // both sources would double-fire if one ever did.
            return true;
        case WM_KEYDOWN: case WM_KEYUP:
        case WM_CHAR:
            if ((msg == WM_KEYDOWN || msg == WM_KEYUP) && wparam == VK_PROCESSKEY) return true;   // the IME owns this key
            QueueInput(msg, wparam, lparam);
            // Editing-key trace (Backspace/Delete): a field report of
            // "Backspace does nothing in text fields" needs to know whether
            // the key reaches the window at all (rate-limited).
            if (msg == WM_KEYDOWN && (wparam == VK_BACK || wparam == VK_DELETE) && !(lparam & (1 << 30))) {
                static std::atomic<int> s_n{ 0 };
                if (s_n.fetch_add(1) < 8)
                    SKSE::log::info("Magelight: WM_KEYDOWN vk=0x{:02X} scan=0x{:02X} queued", wparam, (lparam >> 16) & 0x1FF);
            }
            if (msg == WM_KEYDOWN && wparam == VK_ESCAPE && !EscapeCapturedByUiView()) {   // captured: the page gets the key instead (queued above)
                GameTask::Post([]() { SetUIMode(false); });
            }
            if (msg == WM_KEYDOWN) PeekHotkey(lparam);
            // Dev-loop keys (manual reload / inspector toggle) are deliberately
            // UNBOUND as of 0.17.0 — the owner wants F5/F12 free for now. The
            // machinery stays (ReloadView, s_inspectorRequest -> the inspector
            // host); a binding comes back later, probably via Magelight.json.
            return true;
        case kOcCharMsg: {
            const wchar_t wch = static_cast<wchar_t>(wparam);
            if (lparam == 1) {   // control key delivered as a virtual key
                const LPARAM scan = static_cast<LPARAM>(
                    MapVirtualKeyW(static_cast<UINT>(wch), MAPVK_VK_TO_VSC) & 0xFF) << 16;
                QueueInput(WM_KEYDOWN, static_cast<WPARAM>(wch), scan);
                QueueInput(WM_KEYUP, static_cast<WPARAM>(wch), scan | (1 << 30) | (1 << 31));
            } else if (wch >= 0x20 || wch == L'\t' || wch == L'\r') {
                QueueInput(WM_CHAR, static_cast<WPARAM>(wch), 0);
            } else if (wch == L'\b') {
                QueueInput(WM_KEYDOWN, VK_BACK, 0);
                QueueInput(WM_KEYUP, VK_BACK, 0);
            }
            static std::atomic<int> s_ocLog{ 0 };
            if (s_ocLog.fetch_add(1) < 6)
                SKSE::log::info("Magelight: OpenComposite keyboard char 0x{:04X} (mode {}) delivered to view {}",
                    static_cast<unsigned>(wch), static_cast<int>(lparam),
                    static_cast<std::uint64_t>(s_uiModeView.load()));
            return true;   // ours now: don't let Scaleform also eat it
        }
        case WM_SYSKEYDOWN: case WM_SYSKEYUP:
            // Let the system combo (Alt+F4 etc.) proceed; nothing to the view —
            // but F10 and Alt+key arrive HERE, so bound hotkeys are still peeked.
            if (msg == WM_SYSKEYDOWN) PeekHotkey(lparam);
            return false;
        // ── IME (0.27.0) ──
        case WM_IME_STARTCOMPOSITION:
            s_imeComposing.store(true);
            SKSE::log::info("Magelight: IME composition started");
            ImeNotifyPage("start", L"");
            ImeRepositionCandidates();
            return true;   // no DefWindowProc: it would open the OS composition window; the pre-edit lives in the page
        case WM_IME_COMPOSITION: {
            if (HIMC hImc = ImmGetContext(s_hwnd)) {
                if (lparam & GCS_RESULTSTR) {
                    const std::wstring r = ImeGetString(hImc, GCS_RESULTSTR);
                    ImeNotifyPage("commit", r);
                    for (wchar_t ch : r) QueueInput(WM_CHAR, static_cast<WPARAM>(ch), 0);   // pairs re-join in the drain
                    static std::atomic<int> s_n{ 0 };
                    if (s_n.fetch_add(1) < 6) SKSE::log::info("Magelight: IME commit ({} UTF-16 unit(s))", r.size());
                }
                if (lparam & GCS_COMPSTR) ImeNotifyPage("compose", ImeGetString(hImc, GCS_COMPSTR));
                ImmReleaseContext(s_hwnd, hImc);
            }
            ImeRepositionCandidates();
            return true;   // never DefWindowProc: it would re-deliver the result as WM_CHARs (double insert)
        }
        case WM_IME_ENDCOMPOSITION:
            s_imeComposing.store(false);
            ImeNotifyPage("end", L"");
            return true;
        case WM_IME_SETCONTEXT:
            // Keep the OS candidate list; drop the OS composition window (the pre-edit is inline).
            DefWindowProcW(s_hwnd, msg, wparam, lparam & ~static_cast<LPARAM>(ISC_SHOWUICOMPOSITIONWINDOW));
            return true;
        default:
            return false;
        }
    }

    // SEH wrapper — no locals with destructors allowed in here.
    static bool GuardedHandleFocusedMsg(UINT msg, WPARAM wparam, LPARAM lparam)
    {
        __try {
            return HandleFocusedMsg(msg, wparam, lparam);
        } __except (LogSehAndDisable("wndproc", GetExceptionCode(),
                        (GetExceptionInformation())->ExceptionRecord->ExceptionAddress)) {
        }
        return false;
    }

    LRESULT CALLBACK HookWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
    {
        GameTask::Scope pumpScope;   // posts from here go through the pump (MagelightGameTask.h)
        if (msg == kImeCtlMsg) {   // IME control, marshalled to the window's thread
            if (wparam == 2) ImeRepositionCandidates();
            else ImeAttach(wparam != 0);
            return 0;
        }
        if (s_focused.load() && !s_renderDead.load()) {
            if (GuardedHandleFocusedMsg(msg, wparam, lparam)) return 0;
        }
        if (!s_origWndProc) return DefWindowProcW(hwnd, msg, wparam, lparam);
        return CallWindowProcW(s_origWndProc, hwnd, msg, wparam, lparam);
    }

    // The game's window, its message loop and most SKSE plugins' subclasses are ANSI. A plugin that subclasses
    // after us with SetWindowLongPtrA and calls the proc it replaced directly, not through CallWindowProcA, is
    // handed a USER32 thunk handle for our W proc and executes 0xFFFFxxxx. Sitting on top as an ANSI proc gives it
    // a real address; CallWindowProcA converts A->W exactly as DispatchMessageA did, so HookWndProc sees no change.
    LRESULT CALLBACK AnsiShimWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
    {
        const WNDPROC next = s_ansiShimPrev.load();
        if (!next) return DefWindowProcA(hwnd, msg, wparam, lparam);
        return CallWindowProcA(next, hwnd, msg, wparam, lparam);
    }

    // ── Input sink (toggle key + ALL mouse input) ───────────────────────────
    // Skyrim reads the mouse through exclusive DirectInput — the game window
    // never receives WM mouse messages (field-proven: zero across every run).
    // So while UI mode is up, mouse input comes from the game's own event
    // stream: kMouseMove events read the vanilla CursorMenu's position
    // (MenuCursor — the very cursor the user is looking at, in render-target
    // pixels) and mouse ButtonEvents map to synthetic WM_* entries in the same
    // queue the render thread already drains.

    // Cursor position source. MenuCursor is authoritative while the engine
    // drives it — but the engine stops updating it whenever a foreign engine
    // menu without kDontHideCursorWhenTopmost is topmost (SA's item-carrier
    // IMenu during the live mirror: field report 2026-09-01, cursor frozen on
    // the Outfits page). The device deltas keep arriving at the input sink
    // regardless, so: learn the engine's delta→pixel scale while MenuCursor
    // moves, detect a stall (deltas without movement), and integrate the raw
    // deltas ourselves until MenuCursor moves again. Input-sink thread only.
    static int   s_ownCursorX = 0, s_ownCursorY = 0;
    static int   s_lastMenuX = INT_MIN, s_lastMenuY = INT_MIN;
    static int   s_stallEvents = 0;
    static bool  s_integrating = false;
    static float s_deltaScale = 1.0f;                 // engine px per raw count, learned

    static LPARAM PackCursorPos(int dx = 0, int dy = 0)
    {
        int mx = 0, my = 0;
        if (auto* mc = RE::MenuCursor::GetSingleton()) {
            const auto& rd = mc->GetRuntimeData();
            mx = static_cast<int>(rd.cursorPosX);
            my = static_cast<int>(rd.cursorPosY);
        }
        const bool menuMoved = (mx != s_lastMenuX || my != s_lastMenuY);
        if (menuMoved) {
            // Learn the scale from real movement (both axes, EMA, clamped).
            if (s_lastMenuX != INT_MIN && (dx != 0 || dy != 0)) {
                const float rawLen  = std::sqrt(static_cast<float>(dx * dx + dy * dy));
                const float menuLen = std::sqrt(static_cast<float>(
                    (mx - s_lastMenuX) * (mx - s_lastMenuX) + (my - s_lastMenuY) * (my - s_lastMenuY)));
                if (rawLen > 0.0f && menuLen > 0.0f) {
                    const float r = std::clamp(menuLen / rawLen, 0.1f, 10.0f);
                    s_deltaScale = 0.85f * s_deltaScale + 0.15f * r;
                }
            }
            s_lastMenuX = mx;
            s_lastMenuY = my;
            s_stallEvents = 0;
            if (s_integrating) {
                s_integrating = false;
                SKSE::log::info("Magelight: MenuCursor moving again — engine cursor authoritative");
            }
            s_ownCursorX = mx;
            s_ownCursorY = my;
        } else if (dx != 0 || dy != 0) {
            if (!s_integrating && ++s_stallEvents >= 12) {
                s_integrating = true;
                SKSE::log::info("Magelight: MenuCursor stalled under mouse input — integrating raw deltas (scale {:.2f})",
                    s_deltaScale);
            }
            if (s_integrating) {
                const int bw = s_backbufferW.load(), bh = s_backbufferH.load();
                s_ownCursorX = std::clamp(s_ownCursorX + static_cast<int>(std::lround(dx * s_deltaScale)),
                                          0, std::max(0, bw - 1));
                s_ownCursorY = std::clamp(s_ownCursorY + static_cast<int>(std::lround(dy * s_deltaScale)),
                                          0, std::max(0, bh - 1));
            }
        }
        // Mirror for the render thread's own cursor sprite.
        s_cursorPosX.store(s_ownCursorX);
        s_cursorPosY.store(s_ownCursorY);
        return MAKELPARAM(static_cast<WORD>(s_ownCursorX), static_cast<WORD>(s_ownCursorY));
    }

    class InputSink final : public RE::BSTEventSink<RE::InputEvent*> {
    public:
        static InputSink* GetSingleton()
        {
            static InputSink sink;
            return &sink;
        }
        RE::BSEventNotifyControl ProcessEvent(
            RE::InputEvent* const* a_events, RE::BSTEventSource<RE::InputEvent*>*) override
        {
            GameTask::Scope pumpScope;   // posts from here go through the pump (MagelightGameTask.h)
            if (!a_events) return RE::BSEventNotifyControl::kContinue;
            const bool focused = s_focused.load() && !s_renderDead.load();
            // While UI mode is on, keyboard button events END here: the page
            // already received the key through the window proc, and every
            // sink registered after ours (other mods' hotkeys) would
            // otherwise act on a key that was typed into a text field. The
            // event chain is relinked in place — the engine's own sinks run
            // before ours and are unaffected (menu context handles them).
            // Gamepad and mouse events pass through untouched.
            RE::InputEvent* prev = nullptr;
            for (auto* e = *a_events; e; ) {
                RE::InputEvent* next = e->next;
                const auto type = e->GetEventType();
                if (type == RE::INPUT_EVENT_TYPE::kButton) {
                    const auto* b = e->AsButtonEvent();
                    if (!b) { prev = e; e = next; continue; }
                    if (b->GetDevice() == RE::INPUT_DEVICE::kKeyboard) {
                        const auto code = b->GetIDCode();
                        bool consumed = false;   // toggle key / Escape / a bound hotkey: never typed
                        if (b->IsDown()) {
                            if (code == s_toggleKey.load()) { ToggleVisible(); consumed = true; }
                            else if (focused && code == 0x01) {
                                // Escape through the SINK: the window-proc
                                // Escape needs OS focus (VR mirror window,
                                // alt-tabbed game). Idempotent with it.
                                if (EscapeCapturedByUiView()) {
                                    // The page owns Escape (0.28.0). On VR the sink IS the
                                    // key path, so hand it the key; on flat the window proc
                                    // already queued it.
                                    if (VR::IsVRRuntime()) {
                                        QueueInput(WM_KEYDOWN, VK_ESCAPE, 0);
                                        QueueInput(WM_KEYUP, VK_ESCAPE, (1 << 30) | (1 << 31));
                                    }
                                } else {
                                    SKSE::log::info("Magelight: Escape via sink — leaving UI mode");
                                    GameTask::Post([]() { SetUIMode(false); });
                                }
                                consumed = true;
                            }
                            else consumed = Api4::DispatchHotkey(code);   // v4 / manifest hotkey bindings
                        } else if (code == s_toggleKey.load() || code == 0x01) {
                            consumed = true;   // the release of a key we swallowed on press
                        }
                        // VR only: the window gets no key messages, so the page
                        // is typed from the engine device instead. On flat the
                        // window proc already does this — never both.
                        if (focused && !consumed && VR::IsLive())
                            QueueScancodeAsText(code, b->IsDown());
                        if (focused) {
                            // unlink: keys typed into a page stay in the page
                            if (prev) prev->next = next;
                            else *const_cast<RE::InputEvent**>(a_events) = next;
                            e = next;
                            continue;
                        }
                        prev = e; e = next;
                        continue;
                    }
                    // VR controllers (kVivePrimary..kWMRSecondary): record the
                    // edge for the VR presenter's laser (VR-3) and give the
                    // headset a way OUT of UI mode — B/Y on either hand closes,
                    // the VR twin of Escape. Events pass through untouched.
                    {
                        const int dev = static_cast<int>(b->GetDevice());
                        if (dev >= static_cast<int>(RE::INPUT_DEVICE::kVivePrimary) &&
                            dev <= static_cast<int>(RE::INPUT_DEVICE::kWMRSecondary)) {
                            const auto vrcode = b->GetIDCode();
                            VR::NoteButton(dev, vrcode, b->Value());   // analog LEVEL, not IsDown (see MagelightVR.h)
                            if (focused && b->IsDown() && vrcode == RE::BSOpenVRControllerDevice::Keys::kBY) {
                                // A mod that bound B/Y (bare or chorded) for the
                                // UI-mode view owns open/close on it — its callback
                                // fires from the hotkey tick, and running our exit
                                // as well turned one Grip+B/Y into close + re-open
                                // (SA, VR field log 2026-09-12 14:20). Everyone else
                                // keeps the built-in way out.
                                const auto uiView = static_cast<ViewId>(s_uiModeView.load());
                                if (VR::ViewBindsButton(uiView, VR::kButtonApplicationMenu)) {
                                    SKSE::log::info("Magelight: controller B/Y (device {}) — view {} binds it; built-in exit skipped", dev, uiView);
                                } else {
                                    SKSE::log::info("Magelight: controller B/Y (device {}) — leaving UI mode", dev);
                                    GameTask::Post([]() { SetUIMode(false); });
                                }
                            }
                            // M7: while a page is focused the trigger CLICKS the
                            // panel (via the laser's queued WM_LBUTTON*) and must
                            // not also swing the weapon — unlink it so no later
                            // sink sees it. B/Y stays linked (its exit is queued
                            // above); every other control passes through.
                            if (focused && vrcode == RE::BSOpenVRControllerDevice::Keys::kTrigger) {
                                if (prev) prev->next = next;
                                else *const_cast<RE::InputEvent**>(a_events) = next;
                                e = next;
                                continue;
                            }
                            prev = e; e = next;
                            continue;
                        }
                    }
                    if (!focused || b->GetDevice() != RE::INPUT_DEVICE::kMouse) { prev = e; e = next; continue; }
                    if (VR::IsLive()) { prev = e; e = next; continue; }   // VR: the laser is the only pointer
                    using MK = RE::BSWin32MouseDevice::Key;
                    const LPARAM pos = PackCursorPos();
                    switch (b->GetIDCode()) {
                    case MK::kLeftButton:
                        if (b->IsDown()) QueueInput(WM_LBUTTONDOWN, 0, pos);
                        else if (b->IsUp()) QueueInput(WM_LBUTTONUP, 0, pos);
                        break;
                    case MK::kRightButton:
                        if (b->IsDown()) QueueInput(WM_RBUTTONDOWN, 0, pos);
                        else if (b->IsUp()) QueueInput(WM_RBUTTONUP, 0, pos);
                        break;
                    case MK::kMiddleButton:
                        if (b->IsDown()) QueueInput(WM_MBUTTONDOWN, 0, pos);
                        else if (b->IsUp()) QueueInput(WM_MBUTTONUP, 0, pos);
                        break;
                    case MK::kWheelUp:
                        if (b->IsDown()) QueueInput(WM_MOUSEWHEEL, MAKEWPARAM(0, WHEEL_DELTA), pos);
                        break;
                    case MK::kWheelDown:
                        if (b->IsDown())
                            QueueInput(WM_MOUSEWHEEL,
                                MAKEWPARAM(0, static_cast<WORD>(-WHEEL_DELTA)), pos);
                        break;
                    default:
                        break;
                    }
                } else if (focused && type == RE::INPUT_EVENT_TYPE::kMouseMove && !VR::IsLive()) {
                    const auto* mm = e->AsMouseMoveEvent();
                    QueueInput(WM_MOUSEMOVE, 0,
                        PackCursorPos(mm ? mm->mouseInputX : 0, mm ? mm->mouseInputY : 0));
                } else if (type == RE::INPUT_EVENT_TYPE::kThumbstick) {
                    if (const auto* ts = e->AsThumbstickEvent()) {
                        const int dev = static_cast<int>(ts->GetDevice());
                        if (dev >= static_cast<int>(RE::INPUT_DEVICE::kVivePrimary) &&
                            dev <= static_cast<int>(RE::INPUT_DEVICE::kWMRSecondary)) {
                            VR::NoteThumbstick(dev, ts->xValue, ts->yValue);
                            // M7: don't let the stick turn/move the player while a
                            // page is focused (VR-3.1 will map it to scroll).
                            if (focused) {
                                if (prev) prev->next = next;
                                else *const_cast<RE::InputEvent**>(a_events) = next;
                                e = next;
                                continue;
                            }
                        }
                    }
                }
                prev = e; e = next;
            }
            return RE::BSEventNotifyControl::kContinue;
        }
    };

    // Menu traffic while UI mode is on: which engine menus open and close
    // around us. The cursor only moves while a kUsesCursor menu is topmost,
    // so a stray open/close is the first thing to look at when it freezes.
    class MenuSink final : public RE::BSTEventSink<RE::MenuOpenCloseEvent> {
    public:
        static MenuSink* GetSingleton() { static MenuSink s; return &s; }
        RE::BSEventNotifyControl ProcessEvent(
            const RE::MenuOpenCloseEvent* e, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
        {
            if (e && s_focused.load()) {
                SKSE::log::info("Magelight: menu '{}' {} while UI mode is on",
                    e->menuName.c_str() ? e->menuName.c_str() : "?", e->opening ? "OPENED" : "CLOSED");
            }
            return RE::BSEventNotifyControl::kContinue;
        }
    };

    // The CursorMenu's SPRITE, hidden on flat (VR keeps its own pointer path).
    //
    // MenuCursor::SetCursorVisibility hides the WINDOWS cursor; the sprite is
    // the menu's Scaleform movie, and hiding that takes two calls on the movie
    // ROOT. GFxMovieView::SetVisible(false) is NOT enough: the engine re-shows
    // the CursorMenu movie every frame, so SetVisible blanked it for about one
    // frame a second and the stock cursor came back. The root's own _alpha and
    // _visible are what the engine never touches, so a reskinned cursor (any
    // mod replacing magelightfocus/cursor assets) hides the same way.
    //
    // Game thread. Must be RE-APPLIED, not applied once: every CursorMenu kShow
    // (the watchdog re-shows it, and so does any engine menu opening) resets the
    // movie, and the sprite is back for as long as UI mode is up.
    void HideVanillaCursor(bool hide)
    {
        if (REL::Module::IsVR()) return;   // the VR presenter draws its own pointer
        auto* ui = RE::UI::GetSingleton();
        if (!ui) return;
        const auto menu = ui->GetMenu(RE::CursorMenu::MENU_NAME);
        if (!menu || !menu->uiMovie) return;   // the movie exists only once the menu's show has processed
        menu->uiMovie->SetVariableDouble("_root._alpha", hide ? 0.0 : 100.0);
        menu->uiMovie->SetVariable("_root._visible", RE::GFxValue(!hide));
    }

    // Game thread. If the engine dropped the CursorMenu under us while UI
    // mode is on, put it back (and say so) — without it the MenuCursor
    // position never updates and our cursor sprite stands still.
    void CursorMenuWatchdog()
    {
        if (!s_focused.load()) return;
        auto* ui = RE::UI::GetSingleton();
        auto* q  = RE::UIMessageQueue::GetSingleton();
        if (!ui || !q) return;
        if (!ui->IsMenuOpen(RE::CursorMenu::MENU_NAME)) {
            SKSE::log::warn("Magelight: CursorMenu is not open while UI mode is on — re-showing it");
            q->AddMessage(RE::CursorMenu::MENU_NAME, RE::UI_MESSAGE_TYPE::kShow, nullptr);
        }
        if (s_focusMenuRegistered && !ui->IsMenuOpen(MagelightFocusMenu::MENU_NAME)) {
            SKSE::log::warn("Magelight: focus menu is not open while UI mode is on — re-showing it");
            q->AddMessage(MagelightFocusMenu::MENU_NAME, RE::UI_MESSAGE_TYPE::kShow, nullptr);
        }
        // Re-hide the sprite LAST, after any re-show above: a kShow resets the
        // movie, so hiding before the queue drains would hide nothing. This is
        // the only thing that keeps the stock cursor from reappearing for as
        // long as UI mode is up.
        HideVanillaCursor(true);
        // OCU re-sets OC_MENU_ACTIVE on every menu open/close; keep it clear
        // for as long as our page owns the pointer.
        if (s_hwnd && VR::SuppressRuntimeLaser())
            SetPropW(s_hwnd, L"OC_MENU_ACTIVE", reinterpret_cast<HANDLE>(static_cast<std::intptr_t>(0)));
    }

    void RegisterInputSink()
    {
        InstallKeyboardMute();
        // Registered as early as the device manager exists (kInputLoaded):
        // sinks run in registration order, and the keyboard unlink above
        // only shields the sinks that come AFTER ours.
        static bool s_registered = false;
        if (s_registered) return;
        if (auto* mgr = RE::BSInputDeviceManager::GetSingleton()) {
            mgr->AddEventSink(InputSink::GetSingleton());
            s_registered = true;
            SKSE::log::info("Magelight: input sink registered (Page Up toggles UI mode)");
        }
        if (auto* ui = RE::UI::GetSingleton()) {
            ui->AddEventSink(MenuSink::GetSingleton());
        }
    }

}  // namespace Magelight
