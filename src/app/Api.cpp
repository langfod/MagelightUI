// App API - the public host surface (stage 4.9).
//
// Every function here is declared in src/include/magelight/Magelight.h and
// reached by the host's other TUs, by the plugin itself, and by embedding
// plugins through api/MagelightUI_API.h.
//
// The split is a MOVE, not a rewrite: the bodies are byte-identical to what
// Magelight.cpp held before. Only the location changed. Two rules made that
// possible and must not be undone here:
//
//   - The view registry, its mutex and every *Locked helper live in
//     src/host/Views.cpp (invariant 13: a view is erased ONLY in
//     ApplyPendingLifecycle). This TU LOCKS that mutex but never erases a
//     view or mutates the registry outside a lock.
//   - The image registry lives in src/host/Views.cpp too. Images are
//     registered and mutated here under s_imagesMutex only (invariant 11: an
//     image is never unregistered from under a page that may still name it).
//
// The public surface is any-thread by contract. It marshals game state with
// GameTask::Post and never touches RE:: on a render or API thread.

#include "Magelight.h"

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <system_error>
#include <vector>

#include "host/Platform.h"
#include "host/Render.h"
#include "host/State.h"
#include "host/Views.h"

namespace Magelight {

// The public host surface, moved verbatim from src/Magelight.cpp. The section
// banner comments are kept deliberately - they are how a reader navigates the
// file and what stage 4's later steps key off.

    ViewId CreateView(const char* htmlPath, int x, int y, int w, int h,
                      DomReadyFn onDomReady, bool clickThrough, bool startVisible,
                      const char* sessionName, NetLevel netLevel)
    {
        // w == 0 && h == 0 = FULLSCREEN: the view tracks the backbuffer size
        // (sized on the render thread before materialization, for SA's
        // implicitly-fullscreen self-scaling pages).
        const bool fullscreen = (w == 0 && h == 0);
        if (!fullscreen && (w <= 0 || h <= 0)) return 0;
        auto v = std::make_unique<MlView>();
        v->id = s_nextViewId.fetch_add(1);
        v->htmlPath = htmlPath ? htmlPath : "";
        v->root = PageRootFor(v->htmlPath);
        NotePageRoot(v->root);   // 0.28.1: MlFileSystem serves it too
        v->x = x; v->y = y; v->w = w; v->h = h;
        v->fullscreen = fullscreen;
        v->visible = startVisible;
        v->clickThrough = clickThrough;
        v->onDomReady = onDomReady;
        v->sessionName = sessionName ? sessionName : "";
        v->netLevel = netLevel;
        const ViewId id = v->id;
        v->order = s_nextViewOrder.fetch_add(1);
        {
            std::lock_guard<std::mutex> lk(s_viewsMutex);
            s_views.push_back(std::move(v));
            SortViewsLocked();
        }
        if (fullscreen)
            SKSE::log::info("Magelight: view {} registered ('{}' fullscreen{})",
                id, htmlPath ? htmlPath : "", clickThrough ? ", click-through" : "");
        else
            SKSE::log::info("Magelight: view {} registered ('{}' {}x{} at {},{}{})",
                id, htmlPath ? htmlPath : "", w, h, x, y, clickThrough ? ", click-through" : "");
        return id;
    }

    bool IsViewValid(ViewId view)
    {
        std::lock_guard<std::mutex> lk(s_viewsMutex);
        MlView* v = FindViewLocked(view);
        return v && !v->destroyPending;
    }

    void ShowView(ViewId view, bool show)
    {
        std::lock_guard<std::mutex> lk(s_viewsMutex);
        if (MlView* v = FindViewLocked(view)) {
            if (v->visible != show) v->hiddenSince = show ? 0 : GetTickCount64();
            v->visible = show;
            // 0.31.0 load-stagger: a view SHOWN before its first load skips the
            // one-per-frame queue. A page the player is about to see must not
            // wait behind views nobody has asked for yet.
            if (show && !v->ul) v->loadNow = true;
            // Reopened: the page reports its cursor again on the first mouse move.
            if (!show) v->pageCursor = 0;
        }
    }

    void SetViewCutout(ViewId view, int x, int y, int w, int h)
    {
        std::lock_guard<std::mutex> lk(s_viewsMutex);
        if (MlView* v = FindViewLocked(view)) {
            if (w <= 0 || h <= 0) { v->cutW = v->cutH = 0; return; }
            v->cutX = static_cast<float>(x); v->cutY = static_cast<float>(y);
            v->cutW = static_cast<float>(w); v->cutH = static_cast<float>(h);
        }
    }

    void SetViewScale(ViewId view, float scale)
    {
        // Below 1 Ultralight clips the page to scale squared of the view (a 0.8 scale draws only the top-left
        // 64%), so a smaller page takes a CSS transform instead.
        if (scale < 1.0f)
            SKSE::log::info("Magelight: view {} device scale {:.3f} raised to 1.000 (below 1 clips the page)", view, scale);
        scale = std::clamp(scale, 1.0f, 3.0f);
        std::lock_guard<std::mutex> lk(s_viewsMutex);
        MlView* v = FindViewLocked(view);
        if (!v || v->destroyPending) return;
        v->deviceScale = scale;
        if (v->ul) {
            // Applied by the render thread in ApplyPendingResizes:
            // set_device_scale re-lays out the page synchronously, and this
            // is the API caller's thread (review 2026-09-09) — the same
            // intent-flag shape as a resize.
            v->scaleDirty  = true;
            v->boundsDirty = true;            // the Resize that follows re-rasterises
        }
        SKSE::log::info("Magelight: view {} device scale {:.3f}", view, scale);
    }

    void SetViewHibernate(ViewId view, std::uint32_t idleMs)
    {
        std::lock_guard<std::mutex> lk(s_viewsMutex);
        if (MlView* v = FindViewLocked(view)) v->hibernateMs = idleMs;
    }

    void SetViewBounds(ViewId view, int x, int y, int w, int h)
    {
        if (w <= 0 || h <= 0) return;
        std::lock_guard<std::mutex> lk(s_viewsMutex);
        if (MlView* v = FindViewLocked(view)) {
            v->fullscreen = false;  // explicit bounds end backbuffer tracking
            v->x = x; v->y = y;
            if (v->w != w || v->h != h) {
                v->w = w; v->h = h;
                v->boundsDirty = true;  // render thread applies the Resize
            }
        }
    }

    // Names the host's page script owns (see RegisterJSListener in Magelight.h).
    static bool RefuseReservedListenerName(ViewId view, const char* name)
    {
        if (!(name[0] == '_' && name[1] == '_') && std::strcmp(name, "magelight") != 0) return false;
        static std::mutex s_mx;
        static std::set<std::string> s_logged;
        bool log = false;
        {
            std::lock_guard<std::mutex> lk(s_mx);
            log = s_logged.size() < 64 && s_logged.insert(name).second;
        }
        if (log) SKSE::log::warn("Magelight: listener name '{}' refused (view {}) — 'magelight' and names starting "
                                 "with '__' belong to the host's page script (logged once per name)", name, view);
        return true;
    }

    bool RegisterJSListener(ViewId view, const char* name, JsListenerFn callback)
    {
        if (!name || !*name || !callback) return false;
        if (RefuseReservedListenerName(view, name)) return false;
        std::lock_guard<std::mutex> lk(s_viewsMutex);
        if (MlView* v = FindViewLocked(view)) {
            v->listeners[name] = MlView::Listener{ callback, nullptr, nullptr };
            // A registration after window-object-ready (SA's, from OnDomReady)
            // marks the view: the render thread re-creates __mlNative and every
            // listener shim, leaving window.magelight and its subscriptions alone.
            v->shimsDirty = true;
            return true;
        }
        return false;
    }

    void InteropCall(ViewId view, const char* functionName, const std::string& argument)
    {
        if (!functionName || !*functionName) return;
        std::lock_guard<std::mutex> lk(s_viewsMutex);
        if (MlView* v = FindViewLocked(view)) {
            if (v->outbound.size() < 256) v->outbound.push_back({ functionName, argument, false });
        }
    }

    void EvalJS(ViewId view, const std::string& script, JsResultFnInternal fn, void* user)
    {
        bool queued = false;
        {
            std::lock_guard<std::mutex> lk(s_viewsMutex);
            if (MlView* v = FindViewLocked(view); v && !v->destroyPending && v->evals.size() < 64) {
                v->evals.push_back({ script, fn, user });
                queued = true;
            }
        }
        if (!queued && fn) fn(view, "", "Magelight: view is gone or its eval queue is full", user);
    }

    void InvokeJS(ViewId view, const std::string& script)
    {
        std::lock_guard<std::mutex> lk(s_viewsMutex);
        if (MlView* v = FindViewLocked(view)) {
            if (v->outbound.size() < 256) v->outbound.push_back({ script, {}, true });
        }
    }

    void SetUIModeView(ViewId view)
    {
        s_uiModeView.store(view);
        s_toggleView.store(view);  // the toggle key opens this view
    }

    // ── v4 internals ────────────────────────────────────────────────────────

    bool RegisterJSListenerEx(ViewId view, const char* name, JsListenerExFn callback, void* user)
    {
        if (!name || !*name || !callback) return false;
        if (RefuseReservedListenerName(view, name)) return false;
        std::lock_guard<std::mutex> lk(s_viewsMutex);
        if (MlView* v = FindViewLocked(view)) {
            v->listeners[name] = MlView::Listener{ nullptr, callback, user };
            v->shimsDirty = true;
            return true;
        }
        return false;
    }

    bool DestroyView(ViewId view)
    {
        // Refuse while UI mode is up on this view. A queued EnterUIModeEx
        // that lands after this point sees destroyPending and refuses, so
        // s_uiModeView can never name a destroyed id.
        if (s_focused.load() && s_uiModeView.load() == view) return false;
        std::lock_guard<std::mutex> lk(s_viewsMutex);
        MlView* v = FindViewLocked(view);
        if (!v || v->destroyPending) return false;
        v->visible = false;
        v->destroyPending = true;
        if (s_toggleView.load() == view) s_toggleView.store(0);
        if (s_uiModeView.load() == view) s_uiModeView.store(0);
        return true;
    }

    bool ReloadView(ViewId view)
    {
        std::lock_guard<std::mutex> lk(s_viewsMutex);
        MlView* v = FindViewLocked(view);
        if (!v || v->destroyPending) return false;
        v->reloadPending = true;
        v->domReady = false;   // hold outbound calls for the new page
        return true;
    }

    bool NavigateView(ViewId view, const char* urlOrPath)
    {
        if (!urlOrPath || !*urlOrPath) return false;
        std::string u = urlOrPath;
        if (u.rfind("file://", 0) != 0) {
            const bool absolute = u.size() > 1 && u[1] == ':';
            u = absolute ? PathToFileUrl(std::filesystem::path(u)) : ("file:///" + u);
        }
        std::lock_guard<std::mutex> lk(s_viewsMutex);
        MlView* v = FindViewLocked(view);
        if (!v || v->destroyPending) return false;
        v->navigateUrl = u;
        v->domReady = false;   // hold outbound calls for the new page
        return true;
    }

    void SetViewLayer(ViewId view, int layer)
    {
        std::lock_guard<std::mutex> lk(s_viewsMutex);
        if (MlView* v = FindViewLocked(view)) {
            v->layer = layer;
            v->order = s_nextViewOrder.fetch_add(1);
            SortViewsLocked();
        }
    }

    // ── 0.28.0 ──
    void SetViewEscapeCapture(ViewId view, bool capture)
    {
        std::lock_guard<std::mutex> lk(s_viewsMutex);
        if (MlView* v = FindViewLocked(view)) v->escapeCapture = capture;
    }
    void SetViewNetworkLevel(ViewId view, NetLevel level)
    {
        std::lock_guard<std::mutex> lk(s_viewsMutex);
        if (MlView* v = FindViewLocked(view)) v->netLevel = level;
    }

    void ShowInspectorFor(ViewId page, bool show)
    {
        if (show) { if (s_inspectorVisibleFor.load() != page) s_inspectorRequest.store(page); }
        else      { s_inspectorHideRequest.store(page); }
    }

    bool IsInspectorVisibleFor(ViewId page) { return s_inspectorVisibleFor.load() == page; }

    void SetViewOrder(ViewId view, std::uint64_t order)
    {
        std::lock_guard<std::mutex> lk(s_viewsMutex);
        if (MlView* v = FindViewLocked(view)) {
            v->order = order;
            // keep RaiseView's "above everything" promise
            std::uint64_t cur = s_nextViewOrder.load();
            while (cur <= order && !s_nextViewOrder.compare_exchange_weak(cur, order + 1)) {}
            SortViewsLocked();
        }
    }

    std::uint64_t GetViewOrder(ViewId view)
    {
        std::lock_guard<std::mutex> lk(s_viewsMutex);
        const MlView* v = FindViewLocked(view);
        return v ? v->order : 0;
    }

    void SetViewScrollStep(ViewId view, int px)
    {
        std::lock_guard<std::mutex> lk(s_viewsMutex);
        if (MlView* v = FindViewLocked(view)) v->scrollStep = std::clamp(px, 1, 1000);
    }

    void SetViewSounds(ViewId view, const char* open, const char* close)
    {
        std::lock_guard<std::mutex> lk(s_viewsMutex);
        if (MlView* v = FindViewLocked(view)) {
            v->soundOpen  = open  ? open  : "";
            v->soundClose = close ? close : "";
        }
    }

    void RaiseView(ViewId view)
    {
        std::lock_guard<std::mutex> lk(s_viewsMutex);
        if (MlView* v = FindViewLocked(view)) {
            v->order = s_nextViewOrder.fetch_add(1);
            SortViewsLocked();
        }
    }

    bool GetViewInfo(ViewId view, ViewInfo& out)
    {
        std::lock_guard<std::mutex> lk(s_viewsMutex);
        MlView* v = FindViewLocked(view);
        if (!v || v->destroyPending) return false;
        out.x = v->x; out.y = v->y; out.w = v->w; out.h = v->h;
        out.visible = v->visible; out.fullscreen = v->fullscreen; out.clickThrough = v->clickThrough;
        out.layer = v->layer;
        out.htmlPath = v->htmlPath;
        out.deviceScale = v->deviceScale;
        return true;
    }

    void GetDisplaySize(int& w, int& h)
    {
        w = s_backbufferW.load();
        h = s_backbufferH.load();
    }

    bool IsRenderDead()
    {
        return s_renderDead.load();
    }

    // 0.30.5: WHY it died, for the v4 RenderDead event and RequestUIMode's
    // refusal. Empty when nothing has recorded a reason. Paired with
    // IsRenderDead above because both are "the renderer's state" readers.
    const char* RenderDeadReason()
    {
        return s_renderDeadReasonState.load(std::memory_order_acquire) == 2 ? s_renderDeadReason : "";
    }

    std::filesystem::path GameRootPath()
    {
        return GameRoot();
    }

    bool PathIsUnderGameRoot(const std::filesystem::path& p)
    {
        return PathIsUnder(p, GameRoot());
    }

    bool PathIsUnderDir(const std::filesystem::path& p, const std::filesystem::path& base)
    {
        return PathIsUnder(p, base);
    }

    void SetHostEventSink(HostEventFn fn)
    {
        s_hostEventSink.store(fn);
    }

    // ── Texture-backed images (public API) ──────────────────────────────────

    bool IsGpuAccelerated()
    {
        return s_gpuActive;
    }

    // The name becomes a file name AND a page-visible identifier — keep it
    // to a safe alphabet.
    static bool SafeImageName(const std::string& n)
    {
        if (n.empty() || n.size() > 64) return false;
        for (const char c : n) {
            const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                            (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
            if (!ok) return false;
        }
        return true;
    }

    ImageId RegisterTextureImage(const char* name, ID3D11ShaderResourceView* srv,
                                 std::uint32_t width, std::uint32_t height)
    {
        const std::string n = name ? name : "";
        if (!SafeImageName(n) || !srv || !width || !height) return 0;
        if (!s_gpuRegExt) {
            SKSE::log::warn("Magelight: RegisterTextureImage('{}') — GPU backend lacks external-texture support", n);
            return 0;
        }
        // The .imgsrc the page references in place of an image URL: two
        // lines, magic + identifier (ImageSource.h).
        std::error_code ec;
        const auto dir = s_runtimeDir / L"images";
        std::filesystem::create_directories(dir, ec);
        const auto file = dir / (n + ".imgsrc");
        {
            std::ofstream f(file, std::ios::binary | std::ios::trunc);
            if (!f) {
                SKSE::log::error("Magelight: RegisterTextureImage('{}') — cannot write {}", n, file.string());
                return 0;
            }
            f << "IMGSRC-V1\n" << n;
        }
        srv->AddRef();
        auto im = std::make_unique<MlImage>();
        im->id = s_nextImageId.fetch_add(1);
        im->name = n;
        im->w = width;
        im->h = height;
        im->srv = srv;
        im->url = PathToFileUrl(file);
        const ImageId id = im->id;
        {
            std::lock_guard<std::mutex> lk(s_imagesMutex);
            s_images.push_back(std::move(im));
        }
        SKSE::log::info("Magelight: image '{}' queued as image {} ({}x{})", n, id, width, height);
        return id;
    }

    void UpdateTextureImage(ImageId image, ID3D11ShaderResourceView* srv)
    {
        if (!srv) return;
        std::lock_guard<std::mutex> lk(s_imagesMutex);
        MlImage* im = FindImageLocked(image);
        if (!im || im->removed) return;
        srv->AddRef();
        if (im->pendingSrv) im->pendingSrv->Release();
        im->pendingSrv = srv;
    }

    void InvalidateImage(ImageId image)
    {
        std::lock_guard<std::mutex> lk(s_imagesMutex);
        if (MlImage* im = FindImageLocked(image)) im->dirty = true;
    }

    void UnregisterImage(ImageId image)
    {
        std::lock_guard<std::mutex> lk(s_imagesMutex);
        if (MlImage* im = FindImageLocked(image)) im->unregister = true;
    }

    const char* ImageUrl(ImageId image)
    {
        std::lock_guard<std::mutex> lk(s_imagesMutex);
        MlImage* im = FindImageLocked(image);
        return im ? im->url.c_str() : "";
    }

}  // namespace Magelight
