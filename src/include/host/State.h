#pragma once
// Host state shared across src/host/*.cpp — created by stage 4 of the split.
//
// WHY THIS FILE EXISTS
// The host was one 6123-line TU holding ~82 namespace-scope statics that its
// sections reach across their own boundaries. Splitting it naively gives every
// new TU its own copy of each one — the code would compile and would be
// silently broken (two mutexes, two view registries). So the state is declared
// here once, defined once in State.cpp, and every TU includes this.
//
// The namespace is unchanged (`Magelight`) on purpose: this split is a MOVE,
// not a rename. Nothing in the tree outside src/ can see these symbols, so
// keeping the namespace means the diff is pure relocation and there is no
// rename to get wrong — matching what stage 5 did for MagelightApi4 and
// MagelightVR.
//
// ── What the host is, and what its parts are ───────────────────────────────
//
// The host is one plugin with five responsibilities that share state but not
// meaning. The split gives each one a translation unit:
//
//   Telemetry.cpp   the crash VEH, the stall watchdog, and the runtime
//                   preload that binds the (namespaced) Ultralight modules and
//                   the optional GPU backend. The only code that runs BEFORE
//                   the renderer exists — which is why it also owns the
//                   definitions of the window handle and the devMode flag
//                   (constant initialisers, so order cannot change).
//
//   Views.h/.cpp    the TWO registries — s_views (one MlView per page) and
//                   s_images (one MlImage per texture-backed <img>) — plus
//                   the pure string helpers the JS bridge needs. This is the
//                   host's centre of gravity: eight other TUs reach into it,
//                   which is exactly why it cannot stay in an anonymous
//                   namespace.
//
//   Platform.h/.cpp what the host hands to ultralight::Platform::instance():
//                   our own FileSystem (invariant 5), the network sandbox,
//                   the clipboard, the logger. Ultralight calls all of it on
//                   WEBKIT threads, which is what makes its lock discipline
//                   different from everything else's.
//
//   ... (the render, input, hooks and appapi sections follow as they land)
//
// Sections still in src/Magelight.cpp keep their own statics `static` in its
// anonymous namespace until they are split; nothing reaches across yet.
//
// ── Rules that keep this correct ───────────────────────────────────────────
//
//  1. LOCK DISCIPLINE IS NOT ALTERED BY THE SPLIT. s_viewsMutex is declared
//     here and acquired from eight different TUs (56 acquisition sites). That
//     is correct today because a mutex serialises regardless of which code
//     holds it; the split must not move an acquisition, change a scope, or
//     introduce a nested lock. CLAUDE.md invariants 13 and 16 are about this
//     mutex and must survive byte-for-byte in behaviour.
//  2. `MlView` and the view registry's lock-held helpers (FindViewLocked,
//     SortViewsLocked, FindViewByUlLocked) are declared here so they travel
//     together. The record and the discipline around it are one unit
//     (invariant 16: OnBeginLoading runs INSIDE LoadURL under s_viewsMutex, so
//     a listener that takes the same non-recursive mutex throws and the SEH
//     guard disables the overlay for the session).
//  3. INITIALIZATION ORDER IS A NON-ISSUE, and was verified rather than
//     assumed: every namespace-scope static in the original had a
//     constant-initialiser, so relocating them between TUs cannot change
//     static-initialisation order semantics. (The one apparent exception,
//     `INT_MIN`, is a constant.) Do not introduce a namespace-scope object
//     whose construction calls into another — if that ever looks necessary,
//     make it a function-local static.
//  4. Anything a single TU uses alone stays `static` in that TU's anonymous
//     namespace. Only what is genuinely shared belongs here.

#include "Magelight.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#include <windows.h>
#include <dxgi.h>

namespace Magelight {

    // ── DXGI trampoline signatures (Hooks.cpp) ──────────────────────────────
//
// PresentFn is declared in host/Render.h instead — it is the render section's
// own pointer. The other two have no other user yet and belong with the hooks.

    using ResizeBuffersFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
    using Present1Fn      = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);

    // ── MagelightGPU.dll entry points (Telemetry.cpp binds, Render/D3D call) ─
    // The C ABI in gpu/MagelightGpuApi.h, reached only through these
    // pointers — the host never links the driver (CLAUDE.md invariant 4).

    using MgGpuLogFn      = void (*)(const char*);
    using MgGpuCreateFn   = void* (*)(ID3D11Device*, ID3D11DeviceContext*, MgGpuLogFn);
    using MgGpuDestroyFn  = void (*)(void*);
    using MgGpuGetDrvFn   = void* (*)(void*);
    using MgGpuHasFn      = int (*)(void*);
    using MgGpuDrawFn     = void (*)(void*);
    using MgGpuSrvFn      = void* (*)(void*, std::uint32_t);
// MgGpu_SetSampleCount returns the sample count the driver ACTUALLY got, which
// may be lower than asked (the card's limit for multisampled page targets).
using MgGpuSamplesFn  = int (*)(void*, int);
    using MgGpuRegExtFn   = std::uint32_t (*)(void*, ID3D11ShaderResourceView*);
    using MgGpuSetExtFn   = int (*)(void*, std::uint32_t, ID3D11ShaderResourceView*);
    using MgGpuUnregExtFn = void (*)(void*, std::uint32_t);

    // ── Lifecycle + diagnostics (Telemetry.cpp owns, Render reads) ──────────

    extern std::atomic<bool> s_runtimeReady;
    extern std::atomic<bool> s_hookInstalled;

    // Set at kPostLoadGame/kNewGame. Ultralight is NOT created until the world
    // is up: Skyrim presents main-menu frames from a DIFFERENT thread than
    // in-game frames (field logs 0.3.4/0.3.5 — two distinct present TIDs per
    // session), and WebKit binds its "main thread" at Renderer::Create, then
    // RELEASE_ASSERTs isMainThread() in focus/event dispatch and thread-pool
    // startup. Its CRASH() is a fastfail: no exception dispatch, no VEH, no
    // crashlog — the entire milestone-3 CTD family. Creating on the in-game
    // render thread, and skipping frames from any other thread, is the fix.
    // (CLAUDE.md invariant 1.)
    extern std::atomic<bool> s_worldReady;

    extern std::atomic<bool> s_renderDead;   // latched on any init failure
    // Why the overlay died, for the v4 RenderDead event, RequestUIMode's refusal
    // and the log. Written ONCE and never allocated: it is filled from inside an
    // SEH filter, where allocating would be unsafe. s_renderDeadReasonState goes
    // 0 -> 1 (writing) -> 2 (readable, release) and readers test for 2 (acquire),
    // so a reader never sees a half-written reason.
    extern char             s_renderDeadReason[192];
    extern std::atomic<int> s_renderDeadReasonState;
    // Record why the renderer gave up. First caller wins: the earliest failure is
    // the cause, later ones are consequences. `what` and `detail` must be string
    // literals or pointers to static storage (no allocation — see above).
    void NoteRenderFailure(const char* what, const char* detail = nullptr);

    // Thread identity. CLAUDE.md invariant 1: ALL Ultralight work happens on
    // ONE thread, and a frame from any other skips every bit of it. These are
    // the checks that enforce it, so they are declared where every section can
    // see them rather than buried in one TU.
    extern DWORD s_ulThreadId;    // thread Ultralight was created on
    extern DWORD s_mainThreadId;  // captured at plugin load (game main thread)

    // Stall watchdog (0.28.3): the present hook stamps every frame; a thread
    // notices when the stamps stop and samples the stuck thread's stack. See
    // Telemetry.cpp. Magelight.json "stallWatchdog": false disables.
    extern std::atomic<long long> s_lastPresentMs;
    extern std::atomic<DWORD>     s_lastPresentTid;
    extern std::atomic<bool>      s_stallWatchdog;
    extern std::atomic<int>       s_stallThresholdMs;

    // 0.31.0 host settings, defined in src/host/Telemetry.cpp (the only TU that
    // runs before the renderer exists) and read by the sections that use them.
    //   freezeWorld         false turns SetViewFreezeWorld off for every mod
    //                       (QueryCapability("freezeworld") then answers 0)
    //   freezeWorldSkipCapture  keeps MagelightOverlay out of the frame the
    //                       engine captures as the frozen background, so a
    //                       translucent page cannot ghost over itself
    //   loadStagger/loadBudgetMs  hidden views start their first page load one
    //                       per frame (MaterializeViews)
    extern std::atomic<bool> s_freezeWorldCfg;
    extern std::atomic<bool> s_freezeSkipCapture;
    extern std::atomic<bool> s_loadStagger;
    extern std::atomic<int>  s_loadBudgetMs;
// MSAA sample count requested for PAGE render targets (1, 2, 4 or 8). The
// driver's MgGpu_SetSampleCount is asked for this and returns what the card
// actually gave, so a card that cannot do 8x logs the lower count rather than
// silently rendering single-sampled.
extern std::atomic<int>  s_msaa;
    // Magelight.json "consoleLog": which page console messages reach the log.
    // A page logs whatever it likes (one wrote its config, API key included), so
    // by default only warnings and errors go in. -1 = unset: All in devMode,
    // else Warnings.
    enum ConsoleLog : int { kConsoleLogNone = 0, kConsoleLogErrors = 1, kConsoleLogWarnings = 2, kConsoleLogAll = 3 };
    extern std::atomic<int>  s_consoleLogCfg;
    // kConsoleLineMaxBytes / kConsoleLinesPerSec live in Views.h, beside the
    // ConsoleRate that enforces the second one -- Views.h deliberately does not
    // include this header, and the pair has no meaning apart from it.

    extern std::filesystem::path s_runtimeDir;   // Data/SKSE/Plugins/Magelight

    // ── GPU backend (Telemetry.cpp binds at preload; Render/D3D call) ──────

    extern MgGpuCreateFn   s_gpuCreate;
    extern MgGpuDestroyFn  s_gpuDestroy;
    extern MgGpuGetDrvFn   s_gpuGetDriver;
    extern MgGpuHasFn      s_gpuHas;
    extern MgGpuDrawFn     s_gpuDraw;
    extern MgGpuSrvFn      s_gpuSrv;
extern MgGpuSamplesFn  s_gpuSamples;
    extern MgGpuRegExtFn   s_gpuRegExt;
    extern MgGpuSetExtFn   s_gpuSetExtSrv;
    extern MgGpuUnregExtFn s_gpuUnregExt;
    extern void*           s_gpu;         // backend handle (render thread)
    extern bool            s_gpuActive;   // accelerated path in use

    // The driver's log callback: its lines arrive on ITS threads with no
    // prefix, so they are routed into ours. Defined in Telemetry.cpp beside the
    // binding that installs it; called from the D3D section that creates the
    // backend, so it is declared here rather than duplicated.
    void GpuLogBridge(const char* msg);

    // ── Small cross-TU helpers ─────────────────────────────────────────────

    // The game root (the directory holding SkyrimVR.exe).
    std::filesystem::path GameRoot();

    // ── Window focus + UI mode (set by the input sections; the stall watchdog
    //    reads them so its sample line says what the user was doing) ───────

    extern std::atomic<bool>     s_focused;      // the game window has keyboard focus
    extern std::atomic<std::uint64_t> s_uiModeView;   // UI mode's show/focus target (0 = off)

    // The game's OUTPUT window, captured from the swapchain-desc at hook
    // install (Hooks.cpp). Declared here because the platform bridges open the
    // clipboard against it — Ultralight never sets one itself, so without this
    // every text field silently ignores paste.
    extern HWND s_hwnd;

    // Magelight.json "devMode": hot reload + the JS error banner. Read by the
    // view listener (Platform.cpp paints the banner) and by the settings
    // parser and the public IsDevMode().
    extern std::atomic<bool> s_devMode;

    // ── Render settings ────────────────────────────────────────────────────
    // These are host CONFIGURATION, not render state: LoadHostSettings parses
    // Magelight.json into them and Draw.cpp (cursor art, font rasterization,
    // the CPU fallback) is their only consumer. They moved here when the D3D
    // section split, because leaving them in Magelight.cpp meant the renderer
    // could not reach them without a header that already had to exist.

    // "forceCpu": skip the GPU driver and run Ultralight's CPU rasterizer (the
    // surface path) — a diagnostic A/B and a user escape hatch when the
    // accelerated path misrenders on some load order.
    extern std::atomic<bool> s_forceCpu;

    // Text rasterization ("fontHinting": "smooth" | "normal" | "monochrome" |
    // "none", "fontGamma": number). Ultralight draws grayscale anti-aliased
    // text with a 1.8 gamma; Chromium-hosted UIs had subpixel text, so the
    // same page reads softer here (field 2026-09-05, flat 1440p).
    extern std::string s_fontHinting;
    extern double      s_fontGamma;

    // Cursor art: "cursorFile" (relative to the runtime dir, or absolute;
    // default cursor.png — staged from the repo's assets/ folder),
    // "cursorHeight" (drawn height in px at 1080p, scaled with resolution),
    // "cursorHotspotX/Y" (normalized 0..1 — the pixel that IS the pointer;
    // 0.5/1.0 = bottom-centre, the tip of a map-pin shape). A missing or
    // unreadable file keeps the baked arrow (hotspot 0,0 by construction).
    extern std::string s_cursorFile;
    extern float       s_cursorHeight;   // was 44 — a tad too big on flat at 1080p (owner, 2026-09-13); VR has its own vr.cursorScale
    // Shipped art: the College pin rotated so its point sits at ~10 o'clock
    // like a stock arrow; the tip pixel is (9,2) of 206x206.
    extern float       s_cursorHotX;
    extern float       s_cursorHotY;
    extern bool        s_cursorCustom;   // custom art loaded (render thread)
    extern int         s_cursorImgW;
    extern int         s_cursorImgH;
// 0.31.0 per-view cursors. cursorForce: the host's own cursor always wins (the
// player's "I want my arrow"); modCursors false drops mod images AND tints.
// Both are read on the render thread by PickCursorAt and the VR helpers.
extern std::atomic<bool> s_cursorForce;
extern std::atomic<bool> s_modCursors;
    extern bool s_imageProbe;

    // "toggleKey": 201 — the DirectInput scancode that opens and closes UI mode.
    // It is read from Magelight.json by LoadHostSettings (src/Magelight.cpp,
    // the game thread) and read by the input sink on every keyboard event
    // (src/host/Input.cpp), so it lives here. Atomic for the same reason every
    // other setting is: the write is a game-thread store and the read is on the
    // input-dispatch path.
    extern std::atomic<std::uint32_t> s_toggleKey;

    // "demoViews": true — the playground and badge dev pages. Read only at
    // InstallHook (src/Magelight.cpp), but declared here with the rest of the
    // settings so one grep answers "what does the host read from JSON". See
    // stage 7 (local_projects/stage-7-dev-surfaces.md) for the argument that
    // this and imageProbe do not belong in the shipping pipeline at all.
    extern bool s_demoViews;

    // Milestone-3 playground: hover styling, click counters, a text field, and
    // a scrollable list — one widget per input path (mouse move/down/up, key,
    // char, wheel). The clock keeps proving the JS engine ticks. Defined in
    // Draw.cpp: it is the fallback page the runtime shows when no URL resolves.
    extern const char* const kBannerHTML;

    // ── The queued-input path ──────────────────────────────────────────────
    // Window messages arrive on the game's thread and are turned into
    // Ultralight events on the present thread (Frame.cpp). The queue and its
    // mutex live here because QueueInput (Input.cpp, the window proc side)
    // and DrainInputQueue (Frame.cpp, the consumer) are different files.
    struct InputMsg {
        UINT msg;       // WM_*; 0 = control marker (wparam 1 = focus, 0 = unfocus)
        WPARAM wparam;
        LPARAM lparam;
    };
    extern std::mutex s_inputMutex;
    extern std::vector<InputMsg> s_inputQueue;

    // The producer. Called from the window proc, the IME shim and the VR
    // keyboard hook — all on the game's thread — so it must not touch a
    // View. msg 0 with wparam 1/0 is the focus/unfocus marker.
    void QueueInput(UINT msg, WPARAM w, LPARAM l);

    // ── The input surface (src/host/Input.cpp) ────────────────────────────
    // Stage 4.6 moved the window proc, UI mode, the IME and the engine-side
    // sinks out of Magelight.cpp. These are the things the rest of the host
    // asks of that TU. The statics are here because the hooks and the render
    // pass read them, and duplicating them per-TU is the exact failure the
    // whole split exists to prevent.

    // The SEH filter every host entry point wraps itself in: logs the fault
    // and disables the overlay. Shared with the render pass (Frame.cpp), which
    // must never enter a frame after the kill switch is thrown.
    LONG LogSehAndDisable(const char* where, unsigned long code, void* addr);

    // Game thread. Re-raises the CursorMenu and the focus menu if the engine
    // dropped them while UI mode is on — without the CursorMenu the engine
    // stops driving MenuCursor and our cursor sprite stands still. Posted from
    // the render pass about once a second while focused.
    void CursorMenuWatchdog();

    // Does the UI-mode page own Escape right now? (SetEscapeCapture / the
    // '__escapecapture' channel.) Read by the sinks and the focus menu.
    bool EscapeCapturedByUiView();

    // The window subclass InstallHook put in place, and the ANSI shim on top
    // of it. Read by the subclass teardown path.
    extern WNDPROC               s_origWndProc;
    extern std::atomic<WNDPROC>  s_ansiShimPrev;   // HookWndProc as an A-callable value (AnsiShimWndProc)

    // The focus menu's registration state and its pause flag for the NEXT
    // open. Read by SetUIModePause and the sinks.
    extern std::atomic<bool> s_focusMenuPause;   // flag for the NEXT open
    extern bool              s_focusMenuRegistered;

    // Registered once by InstallHook. The focus menu's ProcessMessage checks
    // the registration flag, so it cannot outlive the registration.
    void RegisterFocusMenu();

    // The quad's Vertex layout is NOT here: it belongs with the D3D objects in
    // host/Render.h, which is what Draw.cpp and Frame.cpp include for it.

}  // namespace Magelight