#pragma once
// The host's view and image registries — created by stage 4 of the split.
//
// WHY THIS IS ITS OWN HEADER, separate from State.h
// State.h holds the state that has no behaviour attached and no Ultralight or
// D3D types in it: lifecycle flags, thread ids, GPU function pointers. This
// header holds the registries, which are the opposite — they are the host's
// central data structures, and they cannot be declared without pulling in
// ultralight::RefPtr, ID3D11ShaderResourceView, JavaScriptCore's JsContextRef
// and the callback typedefs from Magelight.h. Keeping that weight out of
// State.h is what lets a section that only wants s_runtimeDir include a small
// header instead of the whole registry surface.
//
// ── What these registries are ──────────────────────────────────────────────
//
//   s_views   one MlView per page the host shows. Entries are CREATED from any
//             thread (CreateView queues intent) but the Ultralight View, its
//             texture and its SRV are materialized by the render thread and
//             owned by it thereafter. Everything else is guarded by
//             s_viewsMutex: a couple of short holds per frame plus API calls,
//             so one mutex is plenty.
//   s_images  one MlImage per texture-backed <img>. Registered from any thread,
//             materialized on the render thread, and NEVER erased — an
//             unregister is a tombstone, because a page may still hold the
//             driver id and the ImageSource* a live page references
//             (CLAUDE.md invariant 11). Tombstones are what make
//             UpdateTextureImage able to re-point an image instead of
//             unregistering one.
//
// ── Rules that keep this correct ───────────────────────────────────────────
//
//  1. The record and the lock discipline around it are ONE unit and live in
//     this file together (CLAUDE.md invariant 16): OnBeginLoading runs
//     INSIDE LoadURL, which MaterializeViews calls while holding
//     s_viewsMutex. s_viewsMutex is NOT recursive, so a listener that took it
//     again threw system_error, the SEH guard disabled the overlay, and the UI
//     was dead for the whole session (the 0.16.0 field log). The
//     Find*Locked / SortViewsLocked helpers below therefore stay lock-held:
//     their name is the assertion.
//  2. z-order IS (layer, order), maintained by SortViewsLocked. Insertion
//     order is never the draw order.
//  3. Views are erased ONLY in ApplyPendingLifecycle, on the render thread,
//     and the registry entry is erased BEFORE the MlView's RefPtr<View> drops
//     (CLAUDE.md invariant 13). Nothing else may erase from s_views or
//     s_images.

#include "Magelight.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <d3d11.h>

#include <JavaScriptCore/JavaScript.h>

#pragma warning(push)
#pragma warning(disable : 4100)
#include <Ultralight/ImageSource.h>
#include <Ultralight/Ultralight.h>
#pragma warning(pop)

namespace Magelight {

    // ── s_views: the per-page registry ─────────────────────────────────────

    // C++ -> JS: InteropCall queues {fn, arg} on the view; the render thread
    // drains per frame and calls window.fn(arg) through JavaScriptCore with
    // the payload as a REAL string argument (no JS-source inlining).
    struct OutboundCall { std::string fn; std::string arg; bool raw; };

    struct PendingEval { std::string script; JsResultFnInternal fn; void* user; };

    struct MlView {
        ViewId id = 0;
        std::string htmlPath;   // relative to the runtime dir; "" = inline banner
        // Host pinning: the ONE folder file:/// requests from this page may
        // read (its mod folder under Data\Magelight\<Mod>, or the runtime
        // dir for host pages). Everything else is refused in
        // MlNetworkListener — see PageRootFor.
        std::filesystem::path root;
        int x = 0, y = 0;       // negative = anchored from right/bottom edge
        int w = 0, h = 0;
        bool visible = false;
        bool clickThrough = false;
        bool isInspector = false;   // a hosted Web Inspector: visible only while UI mode is active
        float cutX = 0, cutY = 0, cutW = 0, cutH = 0;   // compositor cutout, view pixels (w<=0 = none)
        std::uint32_t hibernateMs = 0;  // 0 = never; else release the View after this long hidden
        std::uint64_t hiddenSince = 0;  // GetTickCount64 at the last hide (0 = visible / never hidden)
        bool dormant = false;           // hibernated: no View until shown again
        bool domReady = false;          // main frame reached DOM ready since its last load
        bool fullscreen = false;    // w/h track the backbuffer (0,0 at CreateView)
        // Ultralight device scale: CSS px -> view px. The page lays out and
        // rasterizes at this scale (real DPI — sharp), instead of a consumer
        // CSS transform that resamples a 1x raster (soft). 1 = 100%.
        float deviceScale = 1.0f;
        // View::needs_paint() sampled BEFORE Renderer::Render() clears it —
        // "this view's pixels change this frame". The VR submitter uses it to
        // skip a copy + compositor upload the runtime does not need.
        bool repaintedThisFrame = true;
        bool boundsDirty = false;   // render thread applies Resize
        bool scaleDirty = false;    // render thread applies set_device_scale (0.28.5)
        bool shimsDirty = false;    // render thread re-installs bridge shims
        DomReadyFn onDomReady = nullptr;
        struct Listener { JsListenerFn fn = nullptr; JsListenerExFn fnEx = nullptr; void* user = nullptr; };
        std::map<std::string, Listener> listeners;
        std::vector<OutboundCall> outbound;
        std::vector<PendingEval> evals;     // EvalJS requests; drained with outbound
        // v4: z-order = (layer, order); lifecycle intents the render thread applies.
        int layer = 1;
        std::uint64_t order = 0;
        bool escapeCapture = false;   // 0.28.0: the page owns Escape (delivered as a key, no UI-mode exit)
        NetLevel netLevel = NetLevel::File;   // the owning mod's NetworkPolicy; File when no mod owns the view
        int  scrollStep = 40;         // 0.28.0: px per wheel notch
        std::string soundOpen, soundClose;   // 0.29.0: host-played on UI-mode enter/exit for this view ("" = none)
        bool destroyPending = false;
        bool reloadPending = false;
        bool reloadedFlag = false;      // tag the next DOM-ready as a ViewReloaded
        std::string navigateUrl;        // non-empty = LoadURL next frame
        std::string sessionName;        // "" = default session; else a per-mod persistent session
        // Render-thread-owned:
        ultralight::RefPtr<ultralight::View> ul;
        ID3D11Texture2D* tex = nullptr;             // CPU-surface path only
        ID3D11ShaderResourceView* srv = nullptr;
        int texW = 0, texH = 0;
    };

    extern std::mutex s_viewsMutex;
    extern std::vector<std::unique_ptr<MlView>> s_views;  // z-order = (layer, order), see SortViewsLocked
    extern std::atomic<std::uint64_t> s_nextViewId;
    // s_uiModeView is declared in host/State.h (Telemetry's watchdog reads it).
    // The view the TOGGLE KEY opens — set only by SetUIModeView (the host's
    // own/demo UI). EnterUIMode deliberately does NOT set it: a consumer's
    // (SA's) view must never be re-opened by our key behind the consumer's
    // open bookkeeping — the key stays exit-only for consumer views.
    extern std::atomic<std::uint64_t> s_toggleView;
    extern std::atomic<std::uint64_t> s_nextViewOrder;
    extern std::atomic<std::uint64_t> s_inspectorRequest;   // inspector toggle request: page (or inspector) view id; render thread consumes (no key bound yet)
    extern std::map<ViewId, ViewId> s_inspectorOf;               // page -> hosted inspector view (render thread only)
    extern std::atomic<std::uint64_t> s_inspectorVisibleFor;   // page whose inspector is showing (0 = none); any thread reads
    extern std::atomic<std::uint64_t> s_inspectorHideRequest;  // 0.28.0: page whose inspector should hide; render thread
    extern std::atomic<HostEventFn> s_hostEventSink;

    void Emit(HostEvent type, ViewId view, int x = 0, int y = 0, const char* detail = "");

    // s_viewsMutex must be held. Keeps the registry in z-order.
    void SortViewsLocked();

    // s_viewsMutex must be held.
    MlView* FindViewLocked(ViewId id);

    // s_viewsMutex must be held. Maps an Ultralight View* back to its entry —
    // the only way a listener callback (which receives a View*, not a ViewId)
    // reaches the page's own state.
    MlView* FindViewByUlLocked(ultralight::View* ul);

    // True when any view wants pixels this frame.
    bool AnyViewVisible();

    // A visible view the player can click: the UI-pass carrier opens only for
    // one of these (see MagelightOverlayMenu), so a HUD view that stays up
    // never holds it open over gameplay.
    bool AnyInteractiveViewVisible();

    // Resolve the anchored position against the current backbuffer size.
    void EffectivePos(const MlView& v, float bw, float bh, float& outX, float& outY);

    // ── s_images: the texture-backed <img> registry ─────────────────────────

    // Registered from ANY thread (RegisterTextureImage queues intent); the
    // render thread materializes (driver id + ImageSource + provider entry)
    // and drains invalidations in FrameWork, exactly like the view registry.
    // Entries are never erased (an unregistered image is tombstoned) so
    // ImageUrl's pointer and the driver id stay valid for pages that still
    // reference them (invariant 11).
    struct MlImage {
        ImageId id = 0;
        std::string name;
        std::uint32_t w = 0, h = 0;
        ID3D11ShaderResourceView* srv = nullptr;         // our own AddRef'd ref
        ID3D11ShaderResourceView* pendingSrv = nullptr;  // UpdateTextureImage, applied on the render thread
        bool dirty = false;
        bool unregister = false;
        bool removed = false;
        std::string url;
        // Render-thread-owned:
        std::uint32_t texId = 0;
        ultralight::RefPtr<ultralight::ImageSource> src;
    };

    extern std::mutex s_imagesMutex;
    extern std::vector<std::unique_ptr<MlImage>> s_images;
    extern std::atomic<std::uint32_t> s_nextImageId;

    MlImage* FindImageLocked(ImageId id);

    // ── Small string helpers shared with the page bridge ───────────────────
    // All pure, and all view-adjacent: every one of these converts something
    // that crosses the page boundary (a JavaScriptCore string, a JSON payload
    // for a listener, a path a view will load). That is why they live with the
    // registry rather than in host/Platform.{h,cpp} — a second copy of
    // JSStringToStd would be a second copy of the UTF-8 length dance that gets
    // subtly wrong.
    //
    // The GENERIC ones are NOT here: PathToFileUrl and the UTF-8/UTF-16
    // conversions moved to host/Platform.{h,cpp} (stage: path/text split),
    // because their callers span the input path, the VR presenter and this
    // bridge — and PathToFileUrl is the inverse of MlFileSystem::UrlDecode,
    // which is there.
    std::string JSStringToStd(JSStringRef s);
    std::string JsonEscape(const std::string& s);
    std::string MlJsonQuote(const std::string& in);

    // ── The render-thread view pass (host/Draw.cpp) ────────────────────────
    // Driven by the present hook in Magelight.cpp, in this order and no other:
    // the first three bring the registry in line with the API's intent, the
    // lifecycle pass runs last so a destroy never races the others.
    void HibernateIdleViews();
    void MaterializeViews();
    void ApplyPendingResizes();
    void ApplyPendingLifecycle();

    // ── Page-bridge internals (Magelight.cpp) ─────────────────────────────
    // The listener callbacks in host/Draw.cpp call these; they stay in
    // Magelight.cpp because they reach the page's registered JS listeners and
    // the outbound bridge queue, both of which are that file's business.

    // Installs window.magelight and one shim per registered JS listener. The
    // load listener runs it on a view whose document is ready; `full` false
    // refreshes only the shims for listeners registered after the load
    // (invariant 16: OnBeginLoading is synchronous inside LoadURL).
    void InstallBridgeShims(MlView& v, bool full);

    // ── Page-bridge surface (src/app/Bridge.cpp) ─────────────────────────
    // The JS <-> C++ bridge lives in its own TU. These are the three things
    // the rest of the host asks of it; everything else is file-local there.

    // Render thread, once per frame: deliver queued C++ -> JS calls per view.
    void DrainBridgeQueues();

    // Raise/lower the host's own on-panel keyboard page (views/keyboard). The
    // UI-mode section calls this when UI mode goes off and when a text field
    // in the UI-mode view takes or loses focus. The page itself never takes UI
    // mode, so the view being typed into keeps key focus.
    void ShowOwnKeyboard(bool on);

    // True once the keyboard page exists, so the dispatcher can tell the
    // host keyboard's own key messages from a page's.
    std::uint64_t OwnKeyboardViewId();

    // The IME section's halves of text-field focus. The bridge detects focus and
    // the caret from the page's own events and calls these; they are declared
    // here rather than in an IME header because the IME TU has not been split
    // out yet (stage 4.6), and this seam is what lets the two halves talk until
    // it is. Remove both declarations when src/host/Input.cpp lands and the
    // bridge includes that instead.
    void ImeOnCaret(ViewId id, const std::string& arg);
    void ImeOnTextFocus(bool focused);

    // The host's own inspector view for `caller`, created once per host view.
    ultralight::RefPtr<ultralight::View> HostInspectorView(ultralight::View* caller);

    // The in-page error surface for a script that threw, with its source and
    // line so the page can show where.
    void ShowDevErrorOverlay(ViewId view, const std::string& text,
                             const std::string& source, std::uint32_t line);

}  // namespace Magelight