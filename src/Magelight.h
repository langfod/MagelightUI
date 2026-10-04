#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

struct ID3D11ShaderResourceView;

// Magelight UI — the host: Present hook, Ultralight on the game's D3D11
// device through the LGPL-isolated GPU driver (CPU-surface fallback), view
// registry + compositor, input routing, cursor, texture images, host events.
//
// Threading model (the load-bearing decision): ALL Ultralight objects are
// created and used on ONE thread — the game's main thread, which is also the
// in-game present thread — from inside the Present hook. Every listener
// callback fires there. The game thread owns UI-mode entry/exit (engine
// menu, cursor, control suspension), the multicast exit callbacks and v4
// GameThread event delivery. Public API functions may be called from any
// thread: they mutate registries under their mutex and leave intents the
// render thread applies next frame.
//
// See README.md / CLAUDE.md for the architecture and invariants.

namespace Magelight {

    // Arm the first-chance vectored exception logger (diagnostics).
    void InstallCrashTelemetry();

    // VR laser (MagelightVR.cpp, present thread) injects synthetic pointer
    // input through the same thread-safe queue DrainInputQueue drains. Typed
    // with stdint, not the Win32 aliases, because this header is parsed before
    // <windows.h> in the VR translation unit. msg is a WM_* value; w/l are the
    // WPARAM/LPARAM payload. Mutex-guarded; callable from the present thread.
    void QueueSyntheticInput(unsigned int msg, std::uintptr_t wparam, std::intptr_t lparam);

    // Load the four Ultralight runtime DLLs from Data/SKSE/Plugins/Magelight/
    // (full paths, dependency order). Our imports are /DELAYLOAD'ed, so this
    // must succeed before any Ultralight symbol is touched. False = runtime
    // missing; the plugin stays inert.
    bool PreloadRuntime();

    // kDataLoaded: fetch the game's swapchain from BSGraphics and vtable-hook
    // IDXGISwapChain::Present. Idempotent.
    void InstallHook();

    // kInputLoaded/kDataLoaded: register the input sink (the UI-mode toggle
    // key, Page Up by default, and mod hotkeys). Idempotent.
    void RegisterInputSink();

    // Toggle UI MODE — overlay shown + focused, vanilla cursor up, game
    // controls suspended (transient, storeState=false). Any thread.
    void ToggleVisible();

    // Drop out of UI mode unconditionally (load-boundary safety valve — a
    // suspended-controls state must never straddle a save load). Game thread.
    void ForceExitUIMode();

    // kPostLoadGame/kNewGame: the world is up, so Present now runs on the real
    // in-game render thread — Ultralight may be created. (Main-menu frames come
    // from a DIFFERENT thread; creating there binds WebKit's main-thread checks
    // to the wrong thread and every later event/paint fastfails.)
    void NotifyWorldReady();

    // ── Views + JS <-> C++ bridge (per-view) ──────────────────────────────
    //
    // Multi-view registry: SeverActions runs four views today (main
    // config, diary popup, travel prompt, retainer-assign), and the whole
    // API is per-view. CreateView may be called from
    // any thread at any time (even before Ultralight initializes); the render
    // thread materializes pending views once the renderer is up.
    //
    // Geometry: x/y are pixels from the top-left of the swapchain; a NEGATIVE
    // x or y anchors from the right/bottom edge instead (x = -260 puts the
    // view's left edge 260px from the right). clickThrough views never
    // receive input and cannot be focused (HUD widgets).
    //
    // JS -> C++: RegisterJSListener(view, "name", fn) creates
    // window.name(argString) in that page (shim installed at window-object-
    // ready, so page scripts see it from their first line). Callbacks fire on
    // the RENDER thread mid-frame — handlers touching game state must marshal
    // via GameTask::Post (MagelightGameTask.h).
    //
    // C++ -> JS: InteropCall(view, "name", arg) invokes window.name(argString)
    // with the payload marshalled as a REAL string argument through
    // JavaScriptCore (no JS-source inlining).
    // InvokeJS(view, script) evaluates raw JS. Both queue from any thread; the
    // render thread drains per frame.
    using ViewId = std::uint64_t;                       // 0 = invalid
    // const char*, not std::string&: these signatures cross the DLL boundary
    // verbatim through the exported API (api/MagelightUI_API.h) — keep them
    // C-ABI-safe. The argument is valid only for the duration of the call.
    using JsListenerFn = void (*)(const char* argument);
    using DomReadyFn = void (*)(ViewId view);           // fires on the render thread

    // A page's network reach, lowest first: File = file:/// under its own
    // root and the runtime dir only (the default, also for views no mod
    // owns); Loopback adds http(s) to this machine; Any lifts the sandbox.
    // Set from the owning mod's NetworkPolicy (MagelightApi4.cpp).
    enum class NetLevel : std::uint8_t { File = 0, Loopback = 1, Any = 2 };

    // htmlPath: relative to the runtime dir, OR an absolute Windows path
    // (drive-letter form) — the SA port ships its HTML in its own mod
    // folder, not ours. Pass w == 0 && h == 0 for a FULLSCREEN view that
    // tracks the backbuffer size (SA's pages are written for an implicitly
    // fullscreen view and self-scale off the viewport).
    // sessionName: nullptr/"" = the shared default jar, else a persistent jar
    // at cache_path/<name> (a filesystem-safe name). It and netLevel are set
    // before the render thread can see the view, so a page never loads in
    // the wrong jar or at the wrong reach.
    ViewId CreateView(const char* htmlPath,
                      int x, int y, int w, int h,
                      DomReadyFn onDomReady = nullptr,
                      bool clickThrough = false,
                      bool startVisible = false,
                      const char* sessionName = nullptr,
                      NetLevel netLevel = NetLevel::File);
    bool IsViewValid(ViewId view);
    void ShowView(ViewId view, bool show);
    void SetViewBounds(ViewId view, int x, int y, int w, int h);
    // False (logged once per name) for a name the host's page script owns:
    // "magelight" or anything starting with "__" (__mlNative, __MAGELIGHT__,
    // the reserved channels). A shim under such a name would break the page.
    bool RegisterJSListener(ViewId view, const char* name, JsListenerFn callback);
    void InteropCall(ViewId view, const char* functionName, const std::string& argument);
    void InvokeJS(ViewId view, const std::string& script);
    // Evaluate and get the completion value back. fn fires on the RENDER
    // thread right after the script ran (next frame); exception is "" on
    // success. Marshal in the caller if game state is touched.
    using JsResultFnInternal = void (*)(ViewId view, const char* result, const char* exception, void* user);
    void EvalJS(ViewId view, const std::string& script, JsResultFnInternal fn, void* user);

    // Which view UI mode shows + focuses (keyboard target). Mouse events
    // hit-test the topmost visible non-clickThrough view under the cursor.
    void SetUIModeView(ViewId view);

    // ── v4 internals (src/MagelightApi4.cpp layers mod identity on these) ──
    using JsListenerExFn = void (*)(ViewId view, const char* argument, void* user);
    bool RegisterJSListenerEx(ViewId view, const char* name, JsListenerExFn callback, void* user);   // names: see RegisterJSListener
    // Lifecycle. All async (the render thread applies them next frame, in
    // ApplyPendingLifecycle — the ONLY place that erases from the registry;
    // it erases the entry before the View RefPtr drops, because listeners
    // key on View* and WebKit may recycle the address).
    // DestroyView refuses (false) the UI-mode view while UI mode is up or an
    // entry is pending; the toggle-view reference is cleared if it was the
    // destroyed one. IsViewValid is false from the moment a destroy is queued.
    bool DestroyView(ViewId view);
    bool ReloadView(ViewId view);
    // file:/// URL, a runtime-relative path, or an absolute drive-letter path.
    bool NavigateView(ViewId view, const char* urlOrPath);
    // z-order: (layer, order). RaiseView moves to the top of its layer.
    void SetViewLayer(ViewId view, int layer);
    void RaiseView(ViewId view);
    struct ViewInfo {
        int x = 0, y = 0, w = 0, h = 0;
        bool visible = false, fullscreen = false, clickThrough = false;

        int layer = 1;
        std::string htmlPath;
        float deviceScale = 1.0f;   // 0.26.9 (ViewDesc::uiScale round-trips through GetViewInfo)
    };
    bool GetViewInfo(ViewId view, ViewInfo& out);
    void GetDisplaySize(int& w, int& h);   // backbuffer pixels; 0,0 before the first frame
    bool IsRenderDead();                   // the overlay disabled itself for the session
    const char* RenderDeadReason();        // why, in a line; "" while alive or when not known
    std::filesystem::path GameRootPath();  // the folder SkyrimSE.exe runs from
    std::filesystem::path RuntimeDirPath(); // Data/SKSE/Plugins/Magelight (the FileSystem root)
    // <SKSE log dir>/Magelight-cache, Ultralight's cache_path (the session
    // jars live under it); empty when the log directory is unknown. Any thread.
    std::filesystem::path CacheDirPath();
    // True when p resolves under the game root (case-insensitive, lexical).
    // The Papyrus tier's absolute-page boundary — see docs/PAPYRUS.md.
    bool PathIsUnderGameRoot(const std::filesystem::path& p);
    // Same test against an arbitrary base (lexical, case-insensitive, ".."
    // collapsed). The Papyrus tier's mod-folder boundary.
    bool PathIsUnderDir(const std::filesystem::path& p, const std::filesystem::path& base);
    // Quote a string as a JSON (and therefore JS) string literal. Every
    // mod-supplied string interpolated into page script goes through this.
    inline std::string JsonQuote(const std::string& in)
    {
        std::string out = "\"";
        for (unsigned char c : in) {
            switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    static const char* const kHex = "0123456789abcdef";
                    out += "\\u00";
                    out += kHex[(c >> 4) & 0xF];
                    out += kHex[c & 0xF];
                } else {
                    out += static_cast<char>(c);
                }
            }
        }
        return out + "\"";
    }
    // Dev loop (Magelight.json "devMode"): hot reload of every v4 view's page
    // folder; manual reload and the Web Inspector toggle are unbound for now
    // (hosted as a System-layer view on the lower half of the screen).
    bool DevModeEnabled();
    bool InspectorAvailable();             // <runtime>/inspector/Main.html present
    // Version gate: at world-ready, if any mod registered with HostTooOld,
    // show the host's own notice page (views/gate) naming them. Game thread.
    void ShowVersionGateIfNeeded();
    // Cutout: a rectangle of the view (view pixels) the compositor DISCARDS,
    // so the game frame shows through the page — SeverActions' transparent
    // wardrobe viewport. w or h <= 0 clears it. Any thread.
    void SetViewCutout(ViewId view, int x, int y, int w, int h);
    // Hibernation: a view hidden for longer than idleMs releases its
    // Ultralight View and GPU texture (the record, listeners, geometry and
    // layer stay); ShowView(true) reloads the page and any InteropCall /
    // InvokeJS sent meanwhile is delivered after DOM ready. The page's own
    // onDomReady fires again on wake. 0 = never (the default).
    void SetViewHibernate(ViewId view, std::uint32_t idleMs);
    void SetViewScale(ViewId view, float scale);   // Ultralight device scale (0.26.9)
    // 0.28.0
    void SetViewEscapeCapture(ViewId view, bool capture);   // the page owns Escape (no UI-mode exit)
    void SetViewNetworkLevel(ViewId view, NetLevel level);  // see NetLevel above
    void ShowInspectorFor(ViewId page, bool show);
    bool IsInspectorVisibleFor(ViewId page);
    void SetViewOrder(ViewId view, std::uint64_t order);
    std::uint64_t GetViewOrder(ViewId view);
    void SetViewScrollStep(ViewId view, int px);
    // 0.29.0 UI sounds: host-played on this view's UI-mode
    // enter/exit (nullptr or "" clears). See MagelightSound.h for names.
    void SetViewSounds(ViewId view, const char* open, const char* close);
    // Host events. Emitted on the RENDER thread (ViewDomReady, ViewLoadFailed,
    // ViewReloaded, ViewDestroyed, ConsoleMessage, DisplayResized), the GAME
    // thread (UIModeEntered/Exited/Refused) or the present thread that saw the
    // kill switch (RenderDead). Never emitted under s_viewsMutex; the sink
    // (MagelightApi4) marshals GameThread deliveries and calls RenderThread
    // consumers inline on the emitting thread.
    enum class HostEvent : int {
        ViewDomReady, ViewLoadFailed, ViewReloaded, ViewDestroyed,
        UIModeEntered, UIModeExited, DisplayResized, ConsoleMessage, RenderDead,
        UIModeRefused,   // EnterUIModeEx's game-thread task declined (detail = reason)
        UIModeSwitched,  // SwitchUIModeView retargeted an ACTIVE UI mode (view = the new target)
    };
    using HostEventFn = void (*)(HostEvent type, ViewId view, int x, int y, const char* detail);
    void SetHostEventSink(HostEventFn fn);

    // ── Deterministic UI-mode control (the SA-port surface) ────────────────
    // SA opens every view from C++ (Show+Focus) and closes
    // it the same way — a key-driven toggle alone can't drive that.
    //
    // EnterUIMode(view): make `view` the UI-mode target, show + focus it,
    // raise the cursor, suspend game controls. Any thread (marshals to the
    // game thread). No-op if UI mode is already active — callers gate on
    // IsUIModeActive() (SA's one-surface-at-a-time contract).
    //
    // ExitUIMode(): drop focus/cursor/controls but deliberately do NOT
    // change view visibility — a view can stay rendered while the game owns
    // input (SA's Free Look). Hiding stays ShowView's job. Key-driven exits
    // (Escape / toggle key / load boundary / render-death) DO hide the
    // UI-mode view, matching the demo behavior and SA's own close flow.
    //
    // IsUIModeActive(): the cross-surface mutex + hotkey gate — the single
    // "is a UI surface up right now?" query every consumer shares.
    void EnterUIMode(ViewId view);
    // pauseGame: push our engine menu with kPausesGame — the world freezes
    // while the view is up (Papyrus keeps running, like vanilla menus).
    void EnterUIModeEx(ViewId view, bool pauseGame, bool noTextEntry = false);
    void ExitUIMode();
    // While UI mode is active, retarget it to another view without leaving
    // (controls stay suspended, cursor stays up, key focus moves). Game-thread
    // task; refusals emit UIModeRefused with a "switch: ..." detail, success
    // emits UIModeSwitched. The previous view stays visible (ShowView hides).
    void SwitchUIModeView(ViewId view);
    // Retarget the PAUSE flag of the UI mode already held (0.26.11): flips
    // kPausesGame on the LIVE focus menu and moves UI::numPausesGame by hand —
    // no menu churn, so no holder can mistake it for an exit. Cursor, controls
    // and text entry are untouched. No-op when unfocused or unchanged.
    void SetUIModePause(bool pause);
    // World freeze (0.31.0): the view's preference that the game skip its 3D
    // render behind it (kFreezeFrameBackground on the focus menu) while it holds
    // UI mode paused. Any thread; applied on the game thread, never on VR, and
    // dropped before the pause whenever pause, focus or the view change.
    void SetViewFreezeWorld(ViewId view, bool freeze);
    bool FreezeWorldAvailable();           // flat and Magelight.json "freezeWorld" not false
    // Which page console messages reach Magelight.log (0.31.0): 0 none, 1 errors, 2 warnings and errors, 3 all.
    // Magelight.json "consoleLog", else 3 in devMode and 2 otherwise. Any thread.
    int ConsoleLogLevel();
    // Staggered view loading (0.31.0): Magelight.json "loadStagger" (default true). Any thread.
    bool LoadStaggerEnabled();
    // Load on show (0.31.0): the view's FIRST load waits until it is shown (ShowView(true), UI-mode entry). Only
    // decides a load that has not started; a loaded page stays loaded. False = no such view. Any thread.
    bool SetViewLoadOnShow(ViewId view, bool onShow);
    // Per-view cursors (0.31.0). A view may carry its own cursor images, one per page state, or "none" (the page
    // draws its own pointer). Two layers per view: its own set (SetViewCursor, Papyrus SetCursor, the manifest's
    // view "cursor") over its mod's default (the manifest's top-level "cursor"). Images decode lazily on a worker
    // thread the first time they are drawn and are shared by file, hotspot, height and press.
    struct CursorImage;                       // Magelight.cpp
    inline constexpr int kCursorStates = 3;   // 0 arrow, 1 pointer (over a clickable element), 2 text
    struct CursorSet {
        std::shared_ptr<CursorImage> image[kCursorStates];
        bool none = false;                    // hide the host cursor over the view: the page draws its own
        bool Empty() const { return !none && !image[0] && !image[1] && !image[2]; }
    };
    // An image for an absolute file (PNG, DDS or another format WIC reads; at most 256x256). hotX/hotY in image
    // pixels, height in px at 1080p (0 = the image's own height), press = shrink on a click like the host arrow.
    // nullptr, with why, when the file does not exist. Any thread; nothing is decoded here.
    std::shared_ptr<CursorImage> MakeCursorImage(const std::filesystem::path& file, float hotX, float hotY,
                                                 float height, bool press, std::string* why);
    // `path` against the view's folder (its page root): relative, no "..". absoluteOk takes an absolute path as is.
    bool ResolveViewFile(ViewId view, const std::string& path, bool absoluteOk, std::filesystem::path& out,
                         std::string* why);
    // Replace the view's own set, or (modDefault) its mod's default layer. False = no such view. Any thread.
    bool SetViewCursorSet(ViewId view, bool modDefault, const CursorSet& set);
    // One state of the view's own set (nullptr clears it; an image, or clearing state 0, clears "none"), or the
    // "none" form (replaces the own set). False = no such view. Any thread.
    bool SetViewCursorState(ViewId view, int state, std::shared_ptr<CursorImage> image);
    bool SetViewCursorNone(ViewId view);
    ViewId GetUIModeView();                // the current/last UI-mode target (0 = none)
    std::uint32_t GetToggleKey();          // the host's own UI-mode toggle scancode (Magelight.json)
    bool IsUIModeActive();

    // Fired on EVERY UI-mode exit (programmatic, Escape, toggle key, load
    // boundary, render-death drop), on the GAME thread, with the view that
    // was active. This is the host's own menu-close notification — SA's close
    // bookkeeping (text-entry release, pending teleports, preview drain,
    // onMenuClosed) hangs off it.
    using UIModeExitFn = void (*)(ViewId view);
    // Multicast (0.9.1+): each registrant hears every exit with the exiting
    // view; filter by the views you own. No unregister in v1-v3.
    void SetUIModeExitCallback(UIModeExitFn cb);

    // ── Texture-backed images (Ultralight 1.4 ImageSource; GPU path only) ──
    // The replacement for "punch a transparent hole and draw into the
    // backbuffer": the caller renders into a D3D11 texture it owns (on the
    // game's device) and registers its SRV under a NAME; the host reserves a
    // driver texture id, wraps it in an ImageSource, and writes
    // <runtime>/images/<name>.imgsrc — the two-line file a page references
    // wherever it would use an image URL (ImageUrl gives the file:/// URL).
    // The page then places it as a plain <img>/background and it clips,
    // scrolls, rounds, fades and transforms like any content.
    //
    // Contract: sRGB-encoded bytes behind a UNORM (not _SRGB) SRV, premultiplied
    // alpha (opaque a=1 is fine); bilinear/no-mip sampling, so keep the texture
    // within ~2x the CSS box. Render into it any time on the game thread, then
    // InvalidateImage — Ultralight repaints only invalidated images. Resize =
    // UpdateTextureImage with the new SRV (same id, same name); the intrinsic
    // w/h are fixed per registration — size the element with CSS.
    // All functions marshal to the render thread; callable from any thread.
    // Everything returns 0 / no-ops on the CPU-surface path
    // (IsGpuAccelerated() == false) — keep a fallback.
    using ImageId = std::uint32_t;                      // 0 = invalid
    bool IsGpuAccelerated();
    ImageId RegisterTextureImage(const char* name, ID3D11ShaderResourceView* srv,
                                 std::uint32_t width, std::uint32_t height);
    void UpdateTextureImage(ImageId image, ID3D11ShaderResourceView* srv);
    void InvalidateImage(ImageId image);
    void UnregisterImage(ImageId image);
    const char* ImageUrl(ImageId image);  // stable for the image's lifetime; "" if invalid

}  // namespace Magelight
