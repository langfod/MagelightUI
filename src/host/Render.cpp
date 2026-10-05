// The host's D3D resources and renderer — stage 4 of the split. See Render.h
// for what these objects are and the three rules that keep them correct.
//
// DEFINITIONS LIVE HERE for a reason that is not tidiness: once the render
// resources move into their own TU, every other section reaches them, and a
// definition in one TU's anonymous namespace would give each of those sections
// its own null pointer. State.cpp does not exist because these are the only
// definitions State.h needs a home for that is not Telemetry.cpp's.

#include "host/Render.h"

#include "Magelight.h"

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include <d3d11.h>
#include <dxgi1_2.h>

#pragma warning(push)
#pragma warning(disable : 4100)
#include <AppCore/Platform.h>
#include <Ultralight/ConsoleMessage.h>
#include <Ultralight/ImageSource.h>
#include <Ultralight/RenderTarget.h>
#include <Ultralight/Ultralight.h>
#include <Ultralight/platform/Config.h>
#include <Ultralight/platform/Logger.h>
#include <Ultralight/platform/Platform.h>
#include <Ultralight/platform/Surface.h>
#pragma warning(pop)

#include "MagelightApi4.h"
#include "MagelightDevWatch.h"
#include "MagelightGameTask.h"
#include "MagelightSound.h"
#include "MagelightVR.h"
#include "magelight/MagelightCursorArt.h"
#include "host/Input.h"
#include "host/Platform.h"
#include "host/State.h"
#include "host/StateBackup.h"
#include "host/Views.h"

namespace Magelight {

    PresentFn s_origPresent = nullptr;

    // Render-thread-owned (created lazily inside the hook; never touched
    // elsewhere after that).
    bool                           s_renderInit = false;
    std::atomic<bool>              s_rendererUp{ false };
    ultralight::RefPtr<ultralight::Renderer> s_ulRenderer;
    ID3D11Device*                  s_device = nullptr;   // from the swapchain's GetDevice; never Release (a wrapper may not AddRef)
    ID3D11DeviceContext*           s_context = nullptr;  // owned ref
    ID3D11VertexShader*            s_vs = nullptr;
    ID3D11PixelShader*             s_ps = nullptr;
    ID3D11PixelShader*             s_psStraight = nullptr;   // VR copy pass (premultiplied -> straight)
    ID3D11InputLayout*             s_layout = nullptr;
    ID3D11Buffer*                  s_vb = nullptr;
    ID3D11Buffer*                  s_cb = nullptr;   // PS b0: the cutout rect
    ID3D11SamplerState*            s_sampler = nullptr;
    ID3D11BlendState*              s_blend = nullptr;
    ID3D11BlendState*              s_blendStraight = nullptr;   // VR copy pass: straight-alpha 'over'
    ID3D11Texture2D*               s_cursorTex = nullptr;
    ID3D11ShaderResourceView*      s_cursorSrv = nullptr;
    ID3D11Texture2D*               s_dotTex = nullptr;      // VR laser-end pointer (0.26.12)
    ID3D11ShaderResourceView*      s_dotSrv = nullptr;

    std::atomic<int> s_cursorPosX{ 0 };
    std::atomic<int> s_cursorPosY{ 0 };
    std::atomic<bool> s_mouseDown{ false };   // set by the window proc (host/Frame.cpp); the drawn cursor's press
    std::atomic<int> s_backbufferW{ 0 }, s_backbufferH{ 0 };

    // ── Per-view cursors, and the drawn cursor (0.31.0 / 0.31.1) ─────────────
    //
    // Two layers per view (MlView::cursorOwn over cursorMod), selected by
    // PickCursorAt from the page's own CSS cursor under the pointer. The drawn
    // arrow comes from MagelightCursorArt.h (engine-free, returns premultiplied
    // BGRA) rather than the baked mask, so a view can RECOLOUR it — that is the
    // whole of the 0.31.1 tint feature.

    // The cursor a page asks for (MlView::pageCursor, from
    // ViewListener::OnChangeCursor in host/Draw.cpp) picks the drawn cursor's
    // shape. It lives in host/Views.h because both this file's pick code and
    // the listener that WRITES MlView::pageCursor must agree on it — a second
    // copy in Draw.cpp is exactly the kind of drift the split forbids. The page
    // reports a plain int, so MlView needs no knowledge of the type.

    // The page at (x, y): the topmost visible interactive view there, else the
    // UI-mode view (the same hit order DrainInputQueue uses, so it IS the page
    // the mouse events go to). Hiding needs the pointer INSIDE that page:
    // outside every view the host cursor always draws, so a stale "none" never
    // leaves the player without a pointer. Precedence:
    // cursorForce > the view's own set > its mod's default > the host cursor;
    // modCursors false drops the images and the tint.
    struct CursorPick {
        PageCursor kind = PageCursor::Arrow;   // for the host cursor (None and Custom read as Arrow)
        bool hide = false;                     // the page under the pointer draws its own
        std::shared_ptr<CursorImage> image;    // the view's image for the state; null = the host cursor
        CursorTintSet tint;                    // the drawn cursor's colours (0.31.1); empty = the host's
    };

    // ── The drawn cursor (MagelightCursorArt.h) ─────────────────────────────
    // Built on the render thread at the pixel height it is drawn at, and rebuilt
    // when that height changes (a resolution change). kArtGlowSteps arrow
    // textures share one size and hotspot: the hover glow fades by stepping
    // through them, with no blend state of its own.
    static constexpr int kArtGlowSteps = 4;
    struct CursorArtTex {
        ID3D11Texture2D*          tex = nullptr;
        ID3D11ShaderResourceView* srv = nullptr;
        int w = 0, h = 0;
        float hotX = 0, hotY = 0;
    };
    // One set per tint in use (the host's own colours are one), at most
    // kArtSets: the set drawn least recently gives its slot to a new tint. A
    // height change rebuilds a set when it is next drawn.
    struct CursorArtSet {
        std::uint64_t key = 0;               // TintKey; 0 = the host's colours
        int height = 0;                      // the height built for; 0 = an empty slot
        std::uint64_t used = 0;              // s_artStamp at the last draw
        CursorArtTex arrow[kArtGlowSteps];
        CursorArtTex ibeam;
    };
    static constexpr int kArtSets = 4;
    static CursorArtSet  s_artSets[kArtSets];
    static std::uint64_t s_artStamp = 0;
    static bool          s_artFailed = false; // a texture creation failed: the baked arrow draws instead

    // The tint's identity: 0 for the host's colours, else a hash of the colours it
    // sets (alpha 0 = unset).
    static std::uint64_t TintKey(const CursorTintSet& t)
    {
        if (t.Empty()) return 0;
        std::uint64_t h = 1469598103934665603ull;
        for (std::uint32_t c : { t.lit, t.shade, t.ink, t.glow, t.ibeam }) {
            const std::uint32_t v = (c & 0xFF000000u) ? (c | 0xFF000000u) : 0u;
            for (int i = 0; i < 4; ++i) {
                h ^= (v >> (8 * i)) & 0xFF;
                h *= 1099511628211ull;
            }
        }
        return h ? h : 1;
    }

    static CursorArt::Palette PaletteFor(const CursorTintSet& t)
    {
        CursorArt::Palette p;
        const auto apply = [](std::uint32_t argb, CursorArt::Rgb& c) {
            if (!(argb & 0xFF000000u)) return;
            c = { ((argb >> 16) & 0xFF) / 255.0f, ((argb >> 8) & 0xFF) / 255.0f, (argb & 0xFF) / 255.0f };
        };
        apply(t.lit, p.lit);
        apply(t.shade, p.shade);
        apply(t.ink, p.ink);
        apply(t.glow, p.glow);
        apply(t.ibeam, p.ibeam);
        return p;
    }

    static bool UploadCursorArt(const CursorArt::Image& img, CursorArtTex& out)
    {
        if (out.srv) { out.srv->Release(); out.srv = nullptr; }
        if (out.tex) { out.tex->Release(); out.tex = nullptr; }
        D3D11_TEXTURE2D_DESC td{};
        td.Width = static_cast<UINT>(img.w); td.Height = static_cast<UINT>(img.h);
        td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_IMMUTABLE;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA sub{ img.px.data(), static_cast<UINT>(img.w) * sizeof(std::uint32_t), 0 };
        if (FAILED(s_device->CreateTexture2D(&td, &sub, &out.tex))) return false;
        if (FAILED(s_device->CreateShaderResourceView(out.tex, nullptr, &out.srv))) {
            out.tex->Release();
            out.tex = nullptr;
            return false;
        }
        out.w = img.w; out.h = img.h; out.hotX = img.hotX; out.hotY = img.hotY;
        return true;
    }

    // Render thread. The drawn cursor in `tint`'s colours at `height` pixels;
    // nullptr when it cannot be built (the caller then falls back).
    static const CursorArtSet* EnsureCursorArt(int height, const CursorTintSet& tint)
    {
        if (s_artFailed || !s_device) return nullptr;
        const std::uint64_t key = TintKey(tint);
        CursorArtSet* slot = nullptr;
        for (auto& set : s_artSets) {
            if (set.height != 0 && set.key == key) { slot = &set; break; }
        }
        if (!slot) {
            slot = &s_artSets[0];
            for (auto& set : s_artSets) {
                if (set.height == 0) { slot = &set; break; }
                if (set.used < slot->used) slot = &set;
            }
            slot->height = 0;   // rebuilt below: UploadCursorArt releases the old textures
        }
        slot->used = ++s_artStamp;
        if (slot->height == height) return slot;
        const CursorArt::Palette pal = PaletteFor(tint);
        bool ok = UploadCursorArt(CursorArt::IBeam(height, pal), slot->ibeam);
        for (int i = 0; ok && i < kArtGlowSteps; ++i)
            ok = UploadCursorArt(CursorArt::Arrow(height, static_cast<float>(i) / (kArtGlowSteps - 1), pal), slot->arrow[i]);
        if (!ok) {
            s_artFailed = true;
            SKSE::log::error("Magelight: drawn cursor - texture creation failed at {}px; the baked arrow draws instead", height);
            return nullptr;
        }
        slot->key = key;
        slot->height = height;
        SKSE::log::info("Magelight: drawn cursor built at {}px{}", height, key ? " (tinted)" : "");
        return slot;
    }

    // The image's SRV, decoding it on FIRST DRAW (D3D11 + WIC are render-thread
    // only here, which is why MakeCursorImage only validates and caches). The
    // decode result is memoised on the image; two views naming the same file
    // share one CursorImage through the cache, so they share one decode.
    static ID3D11ShaderResourceView* CursorImageSrv(const std::shared_ptr<CursorImage>& img)
    {
        if (!img) return nullptr;
        if (img->decoded) return img->srv;
        ID3D11Texture2D* tex = nullptr;
        ID3D11ShaderResourceView* srv = nullptr;
        int w = 0, h = 0;
        if (!DecodeCursorImageFile(img->file, &tex, &srv, w, h)) {
            SKSE::log::warn("Magelight: cursor image {} failed to decode - the host cursor draws instead",
                img->file.string());
            return nullptr;
        }
        img->tex = tex;
        img->srv = srv;
        img->w = w;
        img->h = h;
        img->decoded = true;
        return srv;
    }

    // Render thread. The page at (x, y) and what its cursor should be.
    static CursorPick PickCursorAt(int x, int y, float bw, float bh)
    {
        CursorPick pick;
        std::lock_guard<std::mutex> lk(s_viewsMutex);
        const MlView* view = nullptr;
        bool inside = false;
        for (auto it = s_views.rbegin(); it != s_views.rend(); ++it) {
            const MlView& v = **it;
            if (!v.ul || !v.visible || v.clickThrough) continue;
            float ex = 0, ey = 0;
            EffectivePos(v, bw, bh, ex, ey);
            if (x >= ex && y >= ey && x < ex + v.w && y < ey + v.h) { view = &v; inside = true; break; }
        }
        if (!view) view = FindViewLocked(static_cast<ViewId>(s_uiModeView.load()));
        if (!view) return pick;
        const PageCursor kind = static_cast<PageCursor>(view->pageCursor);
        pick.kind = (kind == PageCursor::None || kind == PageCursor::Custom) ? PageCursor::Arrow : kind;
        if (s_cursorForce.load()) return pick;
        const CursorSet& set = view->cursorOwn.Empty() ? view->cursorMod : view->cursorOwn;
        if (inside && (kind == PageCursor::None || set.none)) {
            pick.hide = true;
            return pick;
        }
        if (!s_modCursors.load()) return pick;
        pick.tint = view->cursorTint;
        if (set.none) return pick;
        const int state = kind == PageCursor::Clickable ? 1 : kind == PageCursor::Text ? 2 : 0;
        pick.image = set.image[state];
        // No art for "clickable" falls back to the arrow; text does NOT (the
        // caret hint matters more than the shape).
        if (!pick.image && state == 1) pick.image = set.image[0];
        return pick;
    }

    // The VR laser end with vr.cursorDot false: the view's image for its page
    // state, never "none" (the laser has to show where it points). Text falls
    // back to the arrow image: VR has no I-beam art. Any thread.
    static std::shared_ptr<CursorImage> VrCursorImageFor(ViewId id)
    {
        if (s_cursorForce.load() || !s_modCursors.load()) return nullptr;
        std::lock_guard<std::mutex> lk(s_viewsMutex);
        const MlView* v = FindViewLocked(id);
        if (!v) return nullptr;
        const CursorSet& set = v->cursorOwn.Empty() ? v->cursorMod : v->cursorOwn;
        const PageCursor kind = static_cast<PageCursor>(v->pageCursor);
        const int state = kind == PageCursor::Clickable ? 1 : kind == PageCursor::Text ? 2 : 0;
        return set.image[state] ? set.image[state] : set.image[0];
    }

    // The VR laser dot's tint for a view (0.31.1): empty under "cursorForce" or
    // "modCursors": false. Any thread.
    static CursorTintSet VrCursorTintFor(ViewId id)
    {
        if (s_cursorForce.load() || !s_modCursors.load()) return {};
        std::lock_guard<std::mutex> lk(s_viewsMutex);
        const MlView* v = FindViewLocked(id);
        return v ? v->cursorTint : CursorTintSet{};
    }


// Render, moved verbatim from src/Magelight.cpp. The banner comments are kept
// deliberately - several record a field-proven measurement.


    // ── UI-pass composite ──────────────────────────────────────────────────
    // With the composite in the game's UI pass, Present still updates and renders the views (into their own
    // textures) and records the quads it would have drawn; the carrier menu's PostDisplay, which the engine
    // calls inside its UI render with the UI's render target bound, draws them there one frame later.
    struct QueuedQuad {
        ID3D11ShaderResourceView* srv = nullptr;   // owned ref
        float u0 = 0, v0 = 0, u1 = 1, v1 = 1;
        float x = 0, y = 0, w = 0, h = 0;
        bool  hasCut = false;
        float cut[4] = { 0, 0, 0, 0 };
    };
    static std::mutex              s_uiPassMutex;
    static std::vector<QueuedQuad> s_uiPassQuads;            // the last published frame
    static float                   s_uiPassBackW = 0, s_uiPassBackH = 0;   // the back buffer they were laid out for
    static std::chrono::steady_clock::time_point s_uiPassPublished{};   // a set older than 250 ms is not drawn
    static std::atomic<std::uint64_t> s_compositeFrames{ 0 };          // FrameWork composites, deferred or not
    static std::atomic<std::uint64_t> s_lastUiPassFrame{ 0 };          // s_compositeFrames at the last PostDisplay
    static std::atomic<bool>          s_uiPassSeen{ false };
    // 0.31.0: the UI pass had nothing to draw on for 30 frames, so pages draw at
    // Present instead. Latched: once it has happened, stay on the fallback — a
    // menu that opens later still has no target, and NoteUiPassState stops
    // reporting (the state would otherwise flap).
    static std::atomic<bool>          s_uiPassBroken{ false };
    static bool                       s_overlayMenuRegistered = false;

    // Set when the late-composite fallback trips (30 vtable presents in a row that never composited: dxgi's
    // Present was not reached, or was reached on a device that is not ours). That is a RUNTIME PROOF that
    // drawing at Present does not reach the screen — which is what a frame generator does: the last present
    // belongs to its own D3D12 swapchain on a D3D12 device, while our quads live on the game's D3D11 one, so
    // the device check rejects every frame (Community Shaders + DLSS-G, log 2026-10-02). Skyrim Upscaler
    // misses the same way, with or without frame generation (2026-10-02).
    //
    // This is the ONLY signal that switches views to the UI pass under "auto". No module name is tested:
    // Skyrim Upscaler must be RENAMED to coexist with Community Shaders, so GetModuleHandleW of any fixed
    // name is null exactly in the install that needs the UI pass, and a user's chosen name defeats any
    // guess. Measured both ways on v1.0.2.0 — renamed, the detour drew and the UI pass was never needed;
    // unrenamed, 30 misses and nothing reached the screen at Present. A proxy is identified by what it
    // does to our frames, not by what it is called. ENB and ReShade are proxies too, but their compositing
    // does reach the screen, so they never trip this and keep drawing at Present.
    static std::atomic<bool>          s_uiPassFallback{ false };

    static void ReleaseQuads(std::vector<QueuedQuad>& qs)
    {
        for (auto& q : qs) if (q.srv) q.srv->Release();
        qs.clear();
    }

    // Present defers to the UI pass only while the carrier menu is actually drawing: its PostDisplay ran within
    // the last two composites. Menus hidden (the console's tm, a photo mode, loading screens) fall back to
    // Present at once, so the views never vanish with them.
    static bool UiPassActive()
    {
        if (!s_uiPassSeen.load() || s_uiPassBroken.load()) return false;
        return s_compositeFrames.load() - s_lastUiPassFrame.load() <= 2;
    }

    static void UiPassComposite();   // fwd (defined below, frame section)

    // The carrier: an invisible menu that exists for its PostDisplay, open only while an interactive view is
    // visible. In the menu stack it reads to the engine as an open menu, so Escape no longer opens the Journal
    // while it is up: it must never stay open over plain gameplay, and a click-through HUD view draws at
    // Present instead. It takes no input; user events pass on.
    class MagelightOverlayMenu final : public RE::IMenu
    {
    public:
        static constexpr const char* MENU_NAME = "MagelightOverlay";

        MagelightOverlayMenu()
        {
            using F = RE::UI_MENU_FLAGS;
            depthPriority = 9;  // over the game's menus (3) and HUD, under the focus menu (10), which stays topmost
            menuFlags.set(F::kAllowSaving);
            if (auto* sm = RE::BSScaleformManager::GetSingleton()) {
                sm->LoadMovie(this, uiMovie, "magelightfocus");
            }
        }

        static RE::IMenu* Create() { return new MagelightOverlayMenu(); }

        RE::UI_MESSAGE_RESULTS ProcessMessage(RE::UIMessage& msg) override
        {
            if (msg.type == RE::UI_MESSAGE_TYPE::kUserEvent) return RE::UI_MESSAGE_RESULTS::kPassOn;
            return RE::IMenu::ProcessMessage(msg);
        }

        void PostDisplay() override { UiPassComposite(); }

        // VR object layout: see MagelightFocusMenu.
        REX::EnumSet<RE::UI_MENU_Unk09, std::uint32_t> vrUnk30{ RE::UI_MENU_Unk09::kNone };   // 30
        std::byte                                      vrUnk34{ 1 };                          // 34
        RE::BSFixedString                              vrMenuName{ MENU_NAME };               // 38
    };
    static_assert(sizeof(MagelightOverlayMenu) == 0x40, "MagelightOverlayMenu must match the VR IMenu size (0x40)");

    // Invariant 18's OBSERVABILITY (0.31.0). The world freeze rides
    // MagelightFocus's kFreezeFrameBackground flag, and the freeze is only sound
    // while that menu is drawing in the UI pass. If the pass stops, the menu is up
    // but invisible, the pages fall back to Present — and frame generation may
    // drop a frame drawn there. A frozen frame under a page that is not being
    // drawn is the bad case, so report the freeze state whenever the pass changes.
    // Ten consecutive frames of agreement before logging, so one late PostDisplay
    // does not produce a spurious pair of lines; capped at 20 stops per session so
    // a genuinely flapping pass cannot flood the log.
    // Lives after MagelightOverlayMenu because it reads that class's MENU_NAME.
    static void NoteUiPassState(bool defer, bool interactiveVisible)
    {
        if (!s_overlayMenuRegistered || !s_uiPassSeen.load() || s_uiPassBroken.load()) return;
        auto* ui = RE::UI::GetSingleton();
        if (!ui) return;
        static bool s_logged = true;   // last logged state: drawing in the UI pass
        static int  s_streak = 0, s_stops = 0;
        const bool carrierUp = interactiveVisible && ui->IsMenuOpen(MagelightOverlayMenu::MENU_NAME);
        const bool state = defer || !carrierUp;   // only a silent carrier counts as stopped
        if (state == s_logged) { s_streak = 0; return; }
        if (++s_streak < 10) return;
        s_streak = 0;
        s_logged = state;
        // The freeze rides MagelightFocus, which is TU-private to Input.cpp, so
        // that file answers the question rather than us reaching for the menu.
        const bool frozen = FocusMenuFreezeActive();
        if (s_stops > 20) return;   // capped: the state still tracks, silently
        if (!state) {
            if (++s_stops > 20) { SKSE::log::info("Magelight: UI pass stop lines capped for this session"); return; }
            SKSE::log::info("Magelight: UI pass stopped drawing (no PostDisplay for {} composites) - views draw at "
                            "Present; world freeze {}", s_compositeFrames.load() - s_lastUiPassFrame.load(),
                            frozen ? "ON" : "off");
        } else {
            SKSE::log::info("Magelight: UI pass drawing again; world freeze {}", frozen ? "ON" : "off");
        }
    }

    // Whether the carrier may OPEN. Registration is unconditional (below); this only decides whether a view
    // actually switches to the UI pass. Kept separate from s_compositeMode so that "never here" (a dead
    // swapchain proxy, VR, an explicit "present") can be told apart from "not yet" (no evidence either way).
    static bool UiCompositeWanted()
    {
        if (REL::Module::IsVR()) return false;           // the VR presenter has its own path
        if (s_compositeMode == "present") return false;  // the user asked for Present, whatever we observe
        if (s_compositeMode == "ui") return true;         // likewise the other way
        // "auto": the UI pass is used only once the composite has been SEEN failing at Present. No module
        // name is consulted — a swapchain proxy is identified by what it does to our frames, not by what it
        // is called, because Skyrim Upscaler has to be RENAMED to load alongside Community Shaders and a
        // name test is null exactly in the install that needs the UI pass (measured both ways, 2026-10-02:
        // renamed -> detour drew, never needed the UI pass; unrenamed -> 30 misses, could not composite at
        // Present without it).
        return s_uiPassFallback.load();
    }

    // "composite" (Magelight.json): WRITTEN by LoadHostSettings in
    // src/Magelight.cpp, READ by UiCompositeWanted above. Defined here so the
    // read side owns it; the write side sees the extern in host/Render.h.
    // Do NOT add a file-local copy in Magelight.cpp — MSVC's C4211 extension
    // keeps the original external linkage, both TUs would emit external
    // definitions, the code would link, and the two objects would carry
    // separate storage (that exact defect, commit 2c41fa7).
    std::string s_compositeMode = "auto";

    // The carrier menu's name as the ENGINE's map keys it, and the close
    // request. AfterCompose (the Present path) needs both when the renderer
    // dies: an open menu over a dead overlay host is read by the engine as a
    // menu, which is what makes Escape stop working.
    const char* const kOverlayMenuName = "MagelightOverlay";

    void HideOverlayCarrier()
    {
        if (!s_overlayMenuRegistered) return;
        // Posted, not immediate: this is called from the render thread inside a
        // present, and the engine's message queue is the UI thread's. A raw
        // AddMessage from here would race the engine's own menu bookkeeping.
        GameTask::Post([]() {
            if (auto* q = RE::UIMessageQueue::GetSingleton())
                q->AddMessage(kOverlayMenuName, RE::UI_MESSAGE_TYPE::kHide, nullptr);
        });
    }

    // Registering the carrier is deliberately UNCONDITIONAL. An engine menu that is registered but never
    // opened is inert: SyncOverlayMenu only shows it while a view is visible, so nothing appears, Escape
    // behaves, and no frame is drawn anywhere else. Registering it up front lets the composite switch over
    // the instant evidence arrives instead of at the next boot, and removes the last place a mod's file
    // name could influence the decision.
    void RegisterOverlayMenu()
    {
        if (s_overlayMenuRegistered || REL::Module::IsVR()) return;
        std::error_code ec;
        if (!std::filesystem::exists(GameRoot() / L"Data" / L"Interface" / L"magelightfocus.swf", ec)) {
            if (s_compositeMode != "auto")
                SKSE::log::warn("Magelight: magelightfocus.swf missing - views draw at Present");
            return;
        }
        if (auto* ui = RE::UI::GetSingleton()) {
            ui->Register(MagelightOverlayMenu::MENU_NAME, MagelightOverlayMenu::Create);
            s_overlayMenuRegistered = true;
        }
    }

    // Called from FrameWork, on the main thread, once the composite target is known. Logs the mode once, so
    // the log says which stage views are drawn at without the registration decision leaking into it.
    void LogCompositeMode()
    {
        SKSE::log::info("Magelight: composite '{}' - views draw {}", s_compositeMode,
            UiCompositeWanted() ? "in the game's UI pass" : "at Present");
    }

    // 30 vtable presents in a row never composited: Present does not reach the screen on this setup (a frame
    // generator presents its own swapchain, on its own device, after the frame is composed). Take the UI pass
    // from now on. Runs on the PRESENT thread, so the latch is set here and FrameWork does the rest — the menu
    // is opened by SyncOverlayMenu, never from here.
    void EnableUiPassOnFallback()
    {
        if (REL::Module::IsVR() || s_uiPassFallback.exchange(true)) return;
        GameTask::Post([]() { LogCompositeMode(); });
    }

    // Opens the carrier while a view is visible and closes it a second after the last one hides. Called from
    // FrameWork on the main thread. A show the engine drops (a load closes every menu) is re-queued after two
    // seconds; a hide waits so a view flickering between pages does not open and close the menu every frame.
    // Gated on UiCompositeWanted, not just on registration: the carrier is registered on every install (it is
    // inert until opened), so without this the menu would also open for a plain Present composite — which
    // would make the engine read it as an open menu and stop Escape from opening the Journal.
    static void SyncOverlayMenu(bool wanted)
    {
        if (!s_overlayMenuRegistered || !UiCompositeWanted()) return;
        auto* ui = RE::UI::GetSingleton();
        auto* q = RE::UIMessageQueue::GetSingleton();
        if (!ui || !q) return;
        using Clock = std::chrono::steady_clock;
        static Clock::time_point s_nextShow{}, s_nextHide{}, s_lastWanted{};
        const auto now = Clock::now();
        const bool open = ui->IsMenuOpen(MagelightOverlayMenu::MENU_NAME);
        if (wanted) {
            s_lastWanted = now;
            s_nextHide = Clock::time_point{};
            if (!open && now >= s_nextShow) {
                s_nextShow = now + std::chrono::seconds(2);
                q->AddMessage(MagelightOverlayMenu::MENU_NAME, RE::UI_MESSAGE_TYPE::kShow, nullptr);
            }
        } else if (open && now - s_lastWanted >= std::chrono::seconds(1) && now >= s_nextHide) {
            s_nextHide = now + std::chrono::seconds(2);
            s_nextShow = Clock::time_point{};
            q->AddMessage(MagelightOverlayMenu::MENU_NAME, RE::UI_MESSAGE_TYPE::kHide, nullptr);
        }
    }

    // Declared in host/State.h: Draw.cpp's MaterializeViews re-queues the
    // focus marker when a hibernated view wakes, so it is cross-TU.
    void QueueInput(UINT msg, WPARAM w, LPARAM l)
    {
        std::lock_guard<std::mutex> lk(s_inputMutex);
        if (s_inputQueue.size() < 512) s_inputQueue.push_back({ msg, w, l });
    }


    // Page console output into Magelight.log — a JS error, refused module, or
    // missing asset names itself instead of rendering as a blank panel.
    // 1.4: the virtual collapsed from 7 args to (View*, const ConsoleMessage&).
    // Without `override` the old signature would still compile as an unrelated
    // function and console logging would silently vanish — keep `override` on
    // every listener method, always. (MessageLevel/MessageSource also
    // RENUMBERED in 1.4 — compare by enum NAME only, never by stored int.)
    ultralight::RefPtr<ultralight::View> HostInspectorView(ultralight::View* caller);

    // MlJsonQuote / JsonEscape / JSStringToStd are declared in
    // host/Views.h and defined in host/Views.cpp — page-boundary helpers
    // shared by the bridge, the listener and the appapi surface. The generic
    // ones are elsewhere: PathToFileUrl and the UTF-8/UTF-16 conversions are
    // in host/Platform.{h,cpp}.

    // devMode: a JS error paints a banner on the page itself, so a broken
    // handler is seen where it happened instead of only in the log. Fixed,
    // click-through, self-removing; the page's own DOM is untouched.
    void ShowDevErrorOverlay(ViewId view, const std::string& text, const std::string& source, std::uint32_t line)
    {
        std::string where = source;
        if (auto slash = where.find_last_of("/\\"); slash != std::string::npos) where = where.substr(slash + 1);
        const std::string msg = text + "\n" + where + ":" + std::to_string(line);
        InvokeJS(view,
            "(function(){var t=" + MlJsonQuote(msg) + ";var d=document.getElementById('__ml_err');"
            "if(!d){d=document.createElement('div');d.id='__ml_err';d.style.cssText='position:fixed;left:0;right:0;top:0;"
            "z-index:2147483647;background:#5a1414;color:#ffd9d9;font:12px/1.4 monospace;padding:6px 10px;"
            "border-bottom:2px solid #ff5c5c;white-space:pre-wrap;max-height:40%;overflow:auto;pointer-events:none';"
            "(document.body||document.documentElement).appendChild(d);}"
            "d.textContent='[Magelight devMode] JS error\\n'+t;clearTimeout(d.__t);"
            "d.__t=setTimeout(function(){d.remove();},12000);})();");
    }

    // ── D3D helpers ─────────────────────────────────────────────────────────
    //
    // Stage 4 moved the whole D3D bring-up — the baked-arrow constants, the
    // shader source, cursor/dot rasterization, CreateDeviceResources,
    // CreateUltralight — into src/host/Draw.cpp, and the composite pass into
    // src/host/Frame.cpp. What remains here is the state they read:
    //
    //     kCursorW / kCursorH   host/Render.h   (the baked arrow's dimensions)
    //     struct Vertex         host/Render.h   (the quad layout)
    //     kDotSize             host/Draw.cpp

    // Absolute path → file:/// URL (PathToFileUrl) is in host/Platform.cpp.

    // Render thread. Persistent Ultralight Sessions by name — one per mod
    // (localStorage/IndexedDB/cookies isolated under cache_path/<name>).
    // The map and GetOrCreateSession are in host/Draw.cpp.
    static std::map<std::string, ultralight::RefPtr<ultralight::Session>> s_sessions;

    // ── Frame work (render thread) ──────────────────────────────────────────
    //
    // The pass itself is in src/host/Frame.cpp; Magelight.cpp drives it from
    // the present hook (FrameWork below).

    // ── Texture-backed images: render-thread materialization ────────────────
    // Runs every frame right before Renderer::Update(): registers pending
    // images with the driver + ImageSourceProvider, applies SRV re-points,
    // tombstones unregistered ones, and Invalidate()s dirty ones (Ultralight
    // only repaints an image that was invalidated).
    static void MaterializeImages()
    {
        if (!s_gpuActive || !s_gpuRegExt) return;
        std::lock_guard<std::mutex> lk(s_imagesMutex);
        for (auto& ip : s_images) {
            MlImage& im = *ip;
            if (im.removed) continue;
            if (!im.texId) {
                im.texId = s_gpuRegExt(s_gpu, im.srv);
                if (!im.texId) {
                    SKSE::log::error("Magelight: image '{}' — driver refused the external texture", im.name);
                    im.removed = true;
                    continue;
                }
                im.src = ultralight::ImageSource::CreateFromTexture(
                    im.w, im.h, im.texId, ultralight::Rect{ 0.0f, 0.0f, 1.0f, 1.0f });
                ultralight::ImageSourceProvider::instance().AddImageSource(im.name.c_str(), im.src);
                SKSE::log::info("Magelight: image '{}' registered (driver texture {}, {}x{}) -> {}",
                    im.name, im.texId, im.w, im.h, im.url);
            }
            if (im.pendingSrv) {
                if (s_gpuSetExtSrv(s_gpu, im.texId, im.pendingSrv)) {
                    im.srv->Release();
                    im.srv = im.pendingSrv;
                } else {
                    im.pendingSrv->Release();
                }
                im.pendingSrv = nullptr;
                im.dirty = true;
            }
            if (im.unregister) {
                ultralight::ImageSourceProvider::instance().RemoveImageSource(im.name.c_str());
                im.src = nullptr;
                // Tombstone: the driver entry and our SRV ref stay — a page
                // element may still resolve to this id, and a missing id leaves
                // whatever SRV was last bound in place.
                im.removed = true;
                SKSE::log::info("Magelight: image '{}' unregistered", im.name);
                continue;
            }
            if (im.dirty && im.src) {
                im.src->Invalidate();
                im.dirty = false;
            }
        }
    }

    // ── ImageSource probe (Magelight.json "imageProbe": true) ───────────────
    // A 512x512 dynamic texture the host repaints on the CPU every frame
    // (moving stripes + a red disc), registered as image "probe" and placed
    // eight ways by views/probe/index.html. Proves the whole chain without a
    // consumer: driver id, .imgsrc resolution, per-frame Invalidate, and
    // which CSS features the GPU path composites (clip/scroll/transform/
    // opacity) versus which fall back to a bitmap it doesn't have (filter,
    // canvas). Render thread.
    static ID3D11Texture2D*          s_probeTex = nullptr;
    static ID3D11ShaderResourceView* s_probeSrv = nullptr;
    static ImageId                   s_probeImage = 0;
    static std::uint32_t             s_probeFrame = 0;
    static constexpr std::uint32_t   kProbeSize = 512;

    static void TickImageProbe()
    {
        if (!s_imageProbe || !s_gpuActive || !s_device) return;
        if (!s_probeTex) {
            D3D11_TEXTURE2D_DESC td{};
            td.Width = kProbeSize;
            td.Height = kProbeSize;
            td.MipLevels = 1;
            td.ArraySize = 1;
            td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DYNAMIC;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            if (FAILED(s_device->CreateTexture2D(&td, nullptr, &s_probeTex)) ||
                FAILED(s_device->CreateShaderResourceView(s_probeTex, nullptr, &s_probeSrv))) {
                SKSE::log::error("Magelight: image probe — texture creation failed; probe disabled");
                s_imageProbe = false;
                return;
            }
            s_probeImage = RegisterTextureImage("probe", s_probeSrv, kProbeSize, kProbeSize);
            SKSE::log::info("Magelight: image probe texture queued as image {}", s_probeImage);
            if (!s_probeImage) {
                s_imageProbe = false;
                return;
            }
        }
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(s_context->Map(s_probeTex, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            const std::uint32_t f = s_probeFrame++;
            const float cx = kProbeSize * (0.5f + 0.35f * std::cos(f * 0.020f));
            const float cy = kProbeSize * (0.5f + 0.35f * std::sin(f * 0.031f));
            for (std::uint32_t y = 0; y < kProbeSize; ++y) {
                auto* row = reinterpret_cast<std::uint32_t*>(
                    static_cast<std::uint8_t*>(m.pData) + y * m.RowPitch);
                for (std::uint32_t x = 0; x < kProbeSize; ++x) {
                    const bool stripe = (((x + y + f * 2) / 32) & 1) != 0;
                    std::uint32_t c = stripe ? 0xFF3A2D17u : 0xFFEFE3C8u;  // ink / parchment, opaque
                    const float dx = static_cast<float>(x) - cx;
                    const float dy = static_cast<float>(y) - cy;
                    if (dx * dx + dy * dy < 60.0f * 60.0f) c = 0xFFB33A2Fu;  // the red disc
                    row[x] = c;
                }
            }
            s_context->Unmap(s_probeTex, 0);
        }
        InvalidateImage(s_probeImage);
    }

    static void EnsureRenderInit(IDXGISwapChain* sc)
    {
        if (s_renderInit || s_renderDead.load()) return;
        s_renderInit = true;
        s_ulThreadId = GetCurrentThreadId();
        if (!CreateDeviceResources(sc) || !CreateUltralight()) {
            s_renderDead.store(true);
            // 0.30.5: say WHY. Each init step names itself as it fails, so this
            // line names the step rather than "something went wrong" (the field
            // log that motivated it had only the generic message).
            NoteRenderFailure("render-thread init failed");   // a no-op when a step already said why
            SKSE::log::error("Magelight: render-thread init failed — overlay disabled");
            return;
        }
        SKSE::log::info("Magelight: render-thread init complete (owning thread {})", s_ulThreadId);
        s_rendererUp.store(true);
        // VR presenter (docs/VR_PRESENTER.md): same thread, after the device
        // resources exist. Not a VR runtime = dormant, one log line.
        VR::Init(s_device, s_context);
    }

    // SEH telemetry (0.3.1 diagnostics): a fault anywhere in our frame or
    // input path becomes a LOG LINE + graceful overlay shutdown instead of a
    // silent CTD (crash loggers routinely miss faults raised inside foreign
    // frames like a Present hook or a window proc).
    //
    // External since stage 4.6: src/host/Input.cpp wraps its own entry points
    // (SetUIMode, the window proc) in this same filter. Declared in host/State.h.
    LONG LogSehAndDisable(const char* where, unsigned long code, void* addr)
    {
        SKSE::log::error(
            "Magelight: SEH exception 0x{:08X} at {} in {} — overlay disabled, UI mode will drop",
            code, addr, where);
        // 0.30.5: a static literal, so no allocation happens inside an SEH filter.
        NoteRenderFailure("an exception in the frame or input path");
        s_renderDead.store(true);
        return EXCEPTION_EXECUTE_HANDLER;
    }

    // ── Dev loop: Web Inspector ───────────────────────────────────────────
    // The inspector is an ordinary Ultralight view WE host: Ultralight asks
    // for one (OnCreateInspectorView) and loads file:///inspector/Main.html
    // from the FileSystem root, so the SDK's inspector/ folder ships under the
    // runtime dir. It takes UI mode while open (input routes to one view), so
    // the inspector toggle flips focus between the page and its inspector; the page keeps
    // running underneath, and the console can drive it.
    ultralight::RefPtr<ultralight::View> HostInspectorView(ultralight::View* caller)
    {
        ViewId page = 0;
        std::filesystem::path pageRoot;
        {
            std::lock_guard<std::mutex> lk(s_viewsMutex);
            for (auto& vp : s_views) if (vp->ul && vp->ul.get() == caller) { page = vp->id; pageRoot = vp->root; break; }
        }
        int bw = 0, bh = 0;
        GetDisplaySize(bw, bh);
        if (bw <= 0 || bh <= 0 || !s_ulRenderer) return nullptr;
        ultralight::ViewConfig vc;
        vc.is_accelerated = s_gpuActive;
        vc.is_transparent = false;
        const int h = bh / 2;
        auto ul = s_ulRenderer->CreateView(static_cast<std::uint32_t>(bw), static_cast<std::uint32_t>(h), vc, nullptr);
        if (!ul) return nullptr;
        // Sandboxed like any page, at File level: the runtime dir (where
        // file:///inspector/ lives) and the inspected page's own folder.
        ul->set_network_listener(&UlNetworkListener());
        auto v = std::make_unique<MlView>();
        v->id = s_nextViewId.fetch_add(1);
        v->htmlPath = "<inspector>";
        v->root = pageRoot;
        v->isInspector = true;
        v->x = 0; v->y = bh - h; v->w = bw; v->h = h;
        v->visible = true;
        v->layer = 3;   // System
        v->order = s_nextViewOrder.fetch_add(1);
        v->ul = ul;
        const ViewId id = v->id;
        {
            std::lock_guard<std::mutex> lk(s_viewsMutex);
            s_views.push_back(std::move(v));
            SortViewsLocked();
        }
        s_inspectorOf[page] = id;
        s_inspectorVisibleFor.store(page);
        SKSE::log::info("Magelight[dev]: inspector view {} hosted for view {} ({}x{}, lower half)", id, page, bw, h);
        return ul;
    }

    // Render thread, after MaterializeViews (the page must have its View).
    static void ProcessInspectorRequest()
    {
        if (const ViewId hidePage = static_cast<ViewId>(s_inspectorHideRequest.exchange(0))) {   // 0.28.0 ShowInspector(false)
            if (auto it = s_inspectorOf.find(hidePage); it != s_inspectorOf.end()) {
                ShowView(it->second, false);
                SwitchUIModeView(hidePage);
                s_inspectorVisibleFor.store(0);
            }
        }
        const ViewId target = static_cast<ViewId>(s_inspectorRequest.exchange(0));
        if (!target) return;
        for (const auto& [page, insp] : s_inspectorOf) {
            if (insp != target) continue;
            // A toggle on the inspector itself: back to the page, inspector hidden.
            ShowView(insp, false);
            SwitchUIModeView(page);
            s_inspectorVisibleFor.store(0);
            SKSE::log::info("Magelight[dev]: inspector {} hidden — focus back to view {}", insp, page);
            return;
        }
        if (auto it = s_inspectorOf.find(target); it != s_inspectorOf.end() && IsViewValid(it->second)) {
            ShowView(it->second, true);
            SwitchUIModeView(it->second);
            s_inspectorVisibleFor.store(target);
            return;
        }
        if (!InspectorAvailable()) {
            SKSE::log::warn("Magelight[dev]: inspector assets missing — expected {}", (s_runtimeDir / "inspector" / "Main.html").string());
            return;
        }
        ultralight::RefPtr<ultralight::View> ul;
        {
            std::lock_guard<std::mutex> lk(s_viewsMutex);
            if (MlView* v = FindViewLocked(target)) ul = v->ul;
        }
        if (!ul) return;
        ul->CreateLocalInspectorView();   // synchronous: OnCreateInspectorView -> HostInspectorView
        if (auto it = s_inspectorOf.find(target); it != s_inspectorOf.end()) SwitchUIModeView(it->second);
    }

    static void FrameWork(IDXGISwapChain* sc)
    {
        // Diagnostic: name every present-thread change (bounded — a session
        // sees at most a couple). This is what proved the two-thread pattern.
        const DWORD tid = GetCurrentThreadId();
        static DWORD lastTid = 0;
        static int tidLogs = 0;
        if (tid != lastTid && tidLogs < 8) {
            SKSE::log::info("Magelight: present thread now {} (was {})", tid, lastTid);
            lastTid = tid;
            ++tidLogs;
        }

        // Init on the MAIN thread only. Menus and loading screens present from
        // a separate render thread; gameplay presents from the game's main
        // loop — in both field runs the in-game present TID equals the TID
        // that loaded the plugin (11664 in 0.3.6, 16452 in 0.3.7). Heuristics
        // failed twice here: kPostLoadGame fires while the loading screen is
        // still presenting (0.3.6), and that loading screen presents many
        // SECONDS of stable frames, so a stability streak passes on the wrong
        // thread too (0.3.7). Thread identity is the deterministic signal.
        if (!s_renderInit) {
            // World-ready FALLBACK (0.17.9): SKSEVR never delivered kNewGame for
            // an Alternate Perspective new game (field 2026-09-03: SKSE dispatched
            // 4 and 5 only; no 3, no 7), so the renderer never started and every
            // hotkey suspended the controls over nothing. The player standing in
            // a loaded cell with no main/loading menu up IS the world being
            // ready; the thread-identity gate below still decides WHEN to init.
            // Main-thread frames only (never the loading-screen thread), polled
            // about twice a second.
            if (!s_worldReady.load() && tid == s_mainThreadId) {
                static int s_wrPolls = 0;
                if ((++s_wrPolls % 30) == 0) {
                    auto* pc = RE::PlayerCharacter::GetSingleton();
                    auto* ui = RE::UI::GetSingleton();
                    if (pc && ui && pc->GetParentCell() && pc->Is3DLoaded() &&
                        !ui->IsMenuOpen(RE::MainMenu::MENU_NAME) && !ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME)) {
                        SKSE::log::info("Magelight: world ready (fallback: player in a loaded cell — no kPostLoadGame/kNewGame arrived)");
                        NotifyWorldReady();
                    }
                }
            }
            if (!s_worldReady.load() || tid != s_mainThreadId) return;
            EnsureRenderInit(sc);
        }
        if (s_renderDead.load()) return;
        if (tid != s_ulThreadId) {
            // A frame from a foreign thread (loading screens etc.): touching
            // Ultralight here is the fastfail. Skip the whole frame.
            static bool warned = false;
            if (!warned) {
                warned = true;
                SKSE::log::warn("Magelight: frame on foreign thread {} (owner {}) — skipping Ultralight work",
                    tid, s_ulThreadId);
            }
            return;
        }
        DXGI_SWAP_CHAIN_DESC scd{};
        float bw = 0, bh = 0;
        if (SUCCEEDED(sc->GetDesc(&scd))) {
            bw = static_cast<float>(scd.BufferDesc.Width);
            bh = static_cast<float>(scd.BufferDesc.Height);
            const int prevW = s_backbufferW.exchange(static_cast<int>(bw));  // for the cursor clamp (input sink)
            const int prevH = s_backbufferH.exchange(static_cast<int>(bh));
            if (prevW > 0 && prevH > 0 && (prevW != static_cast<int>(bw) || prevH != static_cast<int>(bh)))
                Emit(HostEvent::DisplayResized, 0, static_cast<int>(bw), static_cast<int>(bh));
        }

        // Fullscreen views track the backbuffer: sized here BEFORE
        // materialization (a 0x0 view never materializes), re-Resized via
        // the boundsDirty path if the backbuffer ever changes.
        if (bw > 0 && bh > 0) {
            // In VR the backbuffer is the desktop mirror window, whose aspect
            // is unrelated to the panel in the headset — take the configured
            // panel resolution there instead (see VR::Settings::panelWidth).
            int ibw = static_cast<int>(bw), ibh = static_cast<int>(bh);
            int panelW = 0, panelH = 0;
            if (VR::PanelSizeForFullscreen(panelW, panelH)) { ibw = panelW; ibh = panelH; }
            std::lock_guard<std::mutex> lk(s_viewsMutex);
            for (auto& vp : s_views) {
                MlView& v = *vp;
                if (!v.fullscreen || (v.w == ibw && v.h == ibh)) continue;
                v.w = ibw;
                v.h = ibh;
                if (v.ul) v.boundsDirty = true;
            }
        }

        HibernateIdleViews();
        MaterializeViews();
        ApplyPendingResizes();
        Dev::Tick();                 // hot reload: queues ReloadView for changed folders
        ProcessInspectorRequest();   // inspector toggle (unbound for now)
        ApplyPendingLifecycle();
        // Listeners registered AFTER a page loaded (the OnDomReady pattern)
        // get their window shims here, a frame later — the listener shims
        // only; the page core stays as the page loaded it. Raw MlView*
        // snapshots are safe here because the registry is erased ONLY by
        // ApplyPendingLifecycle, on this thread,
        // which already ran this frame — nothing erases between the collect
        // and the install. InstallBridgeShims takes the lock itself, so
        // collect first, install outside.
        {
            std::vector<MlView*> dirty;
            {
                std::lock_guard<std::mutex> lk(s_viewsMutex);
                for (auto& vp : s_views)
                    if (vp->ul && vp->shimsDirty) dirty.push_back(vp.get());
            }
            for (MlView* v : dirty) InstallBridgeShims(*v, false);
        }
        DrainInputQueue(bw, bh);
        DrainBridgeQueues();
        // The CursorMenu's kShow processes a frame or two after SetUIMode hid
        // the vanilla sprite and may have reset it — re-assert once, well
        // after the menu has settled.
        static int uiFrames = 0;
        if (s_focused.load()) {
            if (++uiFrames == 30) {
                GameTask::Post([]() {
                    if (!s_focused.load()) return;   // an exit landed first and restored the cursor
                    auto* mc = RE::MenuCursor::GetSingleton();
                    if (!mc) {
                        SKSE::log::warn("Magelight: no MenuCursor — cannot hide the vanilla cursor; "
                                        "two cursors will show until one re-asserts");
                        return;
                    }
                    // TWO things, because there are TWO cursors. SetCursorVisibility
                    // covers the Windows cursor; the sprite is the CursorMenu's
                    // Scaleform movie (HideVanillaCursor in Input.cpp) and the
                    // engine re-shows that movie every frame, so it needs the
                    // movie-root hide and not a SetVisible. This frame is the first
                    // one where the movie can exist: the show was queued at entry
                    // and processes async.
                    //
                    // MenuCursor exposes ONLY SetCursorVisibility, no getter, so
                    // this records that the re-assert RAN and was applied — not
                    // what the engine thought the state was. Two cursors visible
                    // in a report, with this line present, means the engine
                    // un-hid it LATER (a CursorMenu kShow, or a menu opening and
                    // closing) — CursorMenuWatchdog re-asserts every tick after
                    // this, so that window is now covered too.
                    mc->SetCursorVisibility(false);
                    HideVanillaCursor(true);
                    SKSE::log::info("Magelight: vanilla cursor hidden again (UI frame {}) — "
                                    "the engine may turn it back on; one cursor is ours", 30);
                });
            }
            // Every ~second: the menus we depend on are still up (see
            // CursorMenuWatchdog). Game thread, cheap.
            if (uiFrames > 30 && (uiFrames % 60) == 0) {
                GameTask::Post([]() { CursorMenuWatchdog(); });
                // 0.31.0 world freeze: reconcile. Every other mod that pauses the
                // game does so by bumping numPausesGame, and one that closes its
                // menu while ours is open can take the pause with it — leaving
                // our freeze flag set over a RUNNING game, which hangs it
                // (invariant 18). ApplyFreezeWorld re-checks that the pause is
                // really there and drops the flag if not.
                if (FreezeWorldAvailable()) {
                    GameTask::Post([]() { if (s_focused.load()) ApplyFreezeWorld("reconcile"); });
                }
            }
        } else {
            uiFrames = 0;
        }
        // Texture images: repaint the probe (if on), then register/re-point/
        // invalidate — BEFORE Update() so this frame's Render() sees them.
        TickImageProbe();
        MaterializeImages();
        // ── Present-path stage timing ────────────────────────────────
        // Four rounds of reasoning about VR frame cost have now been wrong,
        // twice confidently. The submit gate is provably working (0% upload
        // on an idle panel) and the game still stalls when the UI is up, so
        // the cost is somewhere this has never measured. Time each stage and
        // print the worst frame once a second: cheap, and it ends the
        // guessing rather than adding to it.
        using ClockT = std::chrono::steady_clock;
        struct StageAcc {
            double ulUpdate = 0, ulRender = 0, composite = 0, vrSubmit = 0;
            double worst = 0;
            int    frames = 0;
            ClockT::time_point lastReport{};
        };
        static StageAcc s_acc;
        const auto stageT0 = ClockT::now();
        auto stageMs = [](ClockT::time_point a, ClockT::time_point b) {
            return std::chrono::duration<double, std::milli>(b - a).count();
        };

        s_ulRenderer->Update();
        // 1.4 pump contract: requestAnimationFrame, CSS animations/transitions
        // and smooth scrolling are driven per-display by RefreshDisplay —
        // Update()+Render() alone still paints, but every animation silently
        // freezes without this call. Our views all ride default display 0.
        s_ulRenderer->RefreshDisplay(0);
        const auto stageAfterUpdate = ClockT::now();
        // 0.31.0 load-stagger: the queued first loads gate on this. A page's parse
        // and first script run land in THIS call, so its cost is the honest signal
        // for "the last load has settled and there is room for the next one".
        s_lastUlUpdateMs = std::chrono::duration<double, std::milli>(stageAfterUpdate - stageT0).count();
        double frameUlRender = 0.0;   // THIS frame's render, for the composite subtraction
        PresentedFrame frame;   // this frame's views, for every presenter (flat + VR)
        // While the UI pass is live the composite records its quads for it instead of drawing them over the
        // back buffer; published below every frame, an empty set included, so a hidden view leaves no trace.
        SyncOverlayMenu(AnyInteractiveViewVisible());
        s_compositeFrames.fetch_add(1);
        const bool interactiveVisible = AnyInteractiveViewVisible();
        const bool defer = s_overlayMenuRegistered && UiPassActive();
        NoteUiPassState(defer, interactiveVisible);
        // The first deferred frame also draws here: the UI pass already ran this frame with nothing queued.
        static bool s_deferredLast = false;
        const bool drawNow = !defer || !s_deferredLast;
        s_deferredLast = defer;
        std::vector<QueuedQuad> queued;
        auto emit = [&](ID3D11ShaderResourceView* qsrv, float u0, float v0, float u1, float v1,
                        float x, float y, float w, float h, const float* cutUV) {
            if (drawNow) DrawOverlay(sc, qsrv, u0, v0, u1, v1, x, y, w, h, cutUV);
            if (!defer || !qsrv) return;
            QueuedQuad q;
            q.srv = qsrv; qsrv->AddRef();
            q.u0 = u0; q.v0 = v0; q.u1 = u1; q.v1 = v1; q.x = x; q.y = y; q.w = w; q.h = h;
            if (cutUV) { q.hasCut = true; std::memcpy(q.cut, cutUV, sizeof(q.cut)); }
            queued.push_back(q);
        };
        if (AnyViewVisible()) {
            // RenderOnly(visible), not Render(): Render() paints EVERY view
            // whose page marked itself dirty, hidden ones included — a closed
            // popup with a spinner, a closed dashboard with a blinking caret,
            // repainted every frame for as long as some OTHER view (a HUD
            // badge, SkyrimNet's chat) kept the loop rendering. The docs'
            // RenderOnly takes the subset; hidden pages keep their dirty flag
            // and paint on their next Show. needs_paint is sampled first —
            // rendering clears it — and only for the views we hand over.
            std::vector<ultralight::View*> toRender;
            {
                std::lock_guard<std::mutex> lk(s_viewsMutex);
                toRender.reserve(s_views.size());
                for (auto& vp : s_views) {
                    MlView& v = *vp;
                    const bool paint = v.visible && v.ul;
                    v.repaintedThisFrame = paint && v.ul->needs_paint();
                    if (paint) toRender.push_back(v.ul.get());
                }
            }
            if (!toRender.empty()) s_ulRenderer->RenderOnly(toRender.data(), toRender.size());
            frameUlRender = stageMs(stageAfterUpdate, ClockT::now());
            s_acc.ulRender += frameUlRender;
            // One state capture brackets everything we do to the pipeline this
            // frame — the GPU driver's command list (which freely rebinds
            // RTs/shaders/scissor) and every composite draw alike.
            StateBackup backup;
            backup.Capture(s_context);
            StateBackup::Neutralize(s_context);
            if (s_gpuActive && s_gpuHas(s_gpu)) s_gpuDraw(s_gpu);
            // Composite visible views in z-order ((layer, order) — the
            // registry is kept sorted by SortViewsLocked). Registry
            // mutation only happens on our own threads — the brief lock walks
            // are safe against the API surface.
            {
                std::lock_guard<std::mutex> lk(s_viewsMutex);
                for (auto& vp : s_views) {
                    MlView& v = *vp;
                    if (!v.visible || !v.ul) continue;
                    float ex = 0, ey = 0;
                    EffectivePos(v, bw, bh, ex, ey);
                    // Presenter seam: the same reading the VR submitter gets.
                    PresentedView pv;
                    pv.id = v.id; pv.ul = v.ul; pv.x = ex; pv.y = ey; pv.w = v.w; pv.h = v.h;
                    pv.layer = v.layer; pv.isInspector = v.isInspector; pv.gpuPath = s_gpuActive;
                    pv.clickThrough = v.clickThrough;   // B3: the laser skips click-through views like hitTest does
                    pv.repainted = v.repaintedThisFrame;
                    // Cutout: view pixels -> this quad's uv range (the GPU
                    // target may be padded, so map through its uv rect).
                    float cut[4] = { 0, 0, 0, 0 };
                    const float* cutPtr = nullptr;
                    if (v.cutW > 0 && v.cutH > 0 && v.w > 0 && v.h > 0) cutPtr = cut;
                    auto mapCut = [&](float u0, float v0, float u1, float v1) {
                        const float fx0 = v.cutX / v.w, fy0 = v.cutY / v.h;
                        const float fx1 = (v.cutX + v.cutW) / v.w, fy1 = (v.cutY + v.cutH) / v.h;
                        cut[0] = u0 + fx0 * (u1 - u0); cut[1] = v0 + fy0 * (v1 - v0);
                        cut[2] = u0 + fx1 * (u1 - u0); cut[3] = v0 + fy1 * (v1 - v0);
                    };
                    // Mirror fit: a VR panel is sized for the headset, not
                    // for the desktop window, so scale it to FIT the mirror
                    // rather than drawing it 1:1 and cropping the right edge.
                    float mx = static_cast<float>(ex), my = static_cast<float>(ey);
                    float mw = static_cast<float>(v.w), mh = static_cast<float>(v.h);
                    if (VR::IsLive() && v.fullscreen && bw > 0 && bh > 0 && v.w > 0 && v.h > 0 &&
                        (v.w != static_cast<int>(bw) || v.h != static_cast<int>(bh))) {
                        const float s = std::min(static_cast<float>(bw) / v.w,
                                                 static_cast<float>(bh) / v.h);
                        mw = v.w * s; mh = v.h * s;
                        mx = (static_cast<float>(bw) - mw) * 0.5f;
                        my = (static_cast<float>(bh) - mh) * 0.5f;
                    }
                    if (s_gpuActive) {
                        const auto rt = v.ul->render_target();
                        auto* srv = static_cast<ID3D11ShaderResourceView*>(
                            s_gpuSrv(s_gpu, rt.texture_id));
                        if (cutPtr) mapCut(rt.uv_coords.left, rt.uv_coords.top, rt.uv_coords.right, rt.uv_coords.bottom);
                        if (VR::MirrorEnabled())
                            emit(srv, rt.uv_coords.left, rt.uv_coords.top,
                                 rt.uv_coords.right, rt.uv_coords.bottom,
                                 mx, my, mw, mh, cutPtr);
                        pv.srv = srv;
                        pv.u0 = rt.uv_coords.left; pv.v0 = rt.uv_coords.top;
                        pv.u1 = rt.uv_coords.right; pv.v1 = rt.uv_coords.bottom;
                    } else {
                        UploadSurfaceIfDirty(v);
                        if (cutPtr) mapCut(0.0f, 0.0f, 1.0f, 1.0f);
                        if (VR::MirrorEnabled())
                            emit(v.srv, 0.0f, 0.0f, 1.0f, 1.0f, mx, my, mw, mh, cutPtr);
                        pv.srv = v.srv;
                    }
                    if (cutPtr) { pv.hasCutout = true; std::memcpy(pv.cutUV, cut, sizeof(cut)); }
                    frame.push_back(std::move(pv));
                }
            }
            // Our own cursor, topmost. The vanilla cursor draws in the game's
            // UI pass (pre-Present) and lands UNDER the overlay; its sprite is
            // hidden while UI mode is up (SetUIModeImpl) and this quad tracks
            // the same MenuCursor position. Scaled with resolution like the
            // vanilla cursor (~2x at 1440p).
            if (s_focused.load() && s_cursorSrv && bh > 0 && !VR::IsLive()) {
                const float px = static_cast<float>(s_cursorPosX.load());
                const float py = static_cast<float>(s_cursorPosY.load());
                // 0.31.0 per-view cursor + 0.31.1 tint. Precedence: the page's own
                // "none" > a mod image > the drawn art in the view's tint > the
                // configured cursor.png > the baked arrow.
                const int artHeight = static_cast<int>(std::lround(s_cursorHeight * (bh / 1080.0f)));
                // The glow fades in over a clickable element (the drawn art) and a
                // press shrinks the cursor about its hotspot (the drawn arrow, and a
                // mod image that asked for it).
                static float glow = 0.0f, press = 0.0f;
                static auto last = std::chrono::steady_clock::now();
                const auto now = std::chrono::steady_clock::now();
                const float dt = std::min(std::chrono::duration<float>(now - last).count(), 0.1f);
                last = now;
                const auto approach = [dt](float v, float target, float seconds) {
                    const float step = dt / seconds;
                    return target > v ? std::min(target, v + step) : std::max(target, v - step);
                };
                const CursorPick pick = PickCursorAt(static_cast<int>(px), static_cast<int>(py), bw, bh);
                const PageCursor kind = pick.kind;
                glow  = approach(glow, kind == PageCursor::Clickable ? 1.0f : 0.0f, 0.12f);
                press = approach(press, s_mouseDown.load() ? 1.0f : 0.0f, 0.06f);
                ID3D11ShaderResourceView* modSrv = (!pick.hide && pick.image) ? CursorImageSrv(pick.image) : nullptr;
                if (pick.hide) {
                    // The page under the pointer draws its own (CSS cursor: none, or
                    // its view's "none").
                } else if (modSrv) {
                    // A mod's image: its height at 1080p (else its own) scaled with
                    // the backbuffer, aspect kept, the hotspot pixel on the position.
                    const CursorImage& ci = *pick.image;
                    const float h = (ci.height > 0.0f ? ci.height : static_cast<float>(ci.h)) * (bh / 1080.0f);
                    const float w = h * (static_cast<float>(ci.w) / static_cast<float>(ci.h));
                    const float scale = ci.press ? 1.0f - 0.14f * press : 1.0f;
                    float x = px - ci.hotX * (w / ci.w) * scale, y = py - ci.hotY * (h / ci.h) * scale;
                    if (scale == 1.0f) { x = std::round(x); y = std::round(y); }
                    emit(modSrv, 0.0f, 0.0f, 1.0f, 1.0f, x, y, w * scale, h * scale, nullptr);
                } else if (const CursorArtSet* art = (s_cursorCustom && s_cursorImgH > 0) ? nullptr
                                                          : EnsureCursorArt(artHeight, pick.tint)) {
                    // The drawn cursor, in the view's tint. At rest it sits on whole
                    // pixels (1:1 with its texture when the target is the back buffer).
                    const bool text = kind == PageCursor::Text;
                    const CursorArtTex& t = text ? art->ibeam
                        : art->arrow[std::clamp(static_cast<int>(std::lround(glow * (kArtGlowSteps - 1))), 0, kArtGlowSteps - 1)];
                    const float scale = text ? 1.0f : 1.0f - 0.14f * press;
                    float x = px - t.hotX * scale, y = py - t.hotY * scale;
                    if (scale == 1.0f) { x = std::round(x); y = std::round(y); }
                    emit(t.srv, 0.0f, 0.0f, 1.0f, 1.0f, x, y, t.w * scale, t.h * scale, nullptr);
                } else if (s_cursorCustom && s_cursorImgH > 0) {
                    // Custom art: configured height at 1080p scaled with the
                    // backbuffer, aspect preserved, drawn so the hotspot
                    // pixel sits exactly on the cursor position.
                    const float h = s_cursorHeight * (bh / 1080.0f);
                    const float w = h * (static_cast<float>(s_cursorImgW) / s_cursorImgH);
                    emit(s_cursorSrv, 0.0f, 0.0f, 1.0f, 1.0f,
                         px - s_cursorHotX * w, py - s_cursorHotY * h, w, h, nullptr);
                } else {
                    const float h = s_cursorHeight * (bh / 1080.0f);
                    emit(s_cursorSrv, 0.0f, 0.0f, 1.0f, 1.0f,
                         px, py, h * (static_cast<float>(kCursorW) / kCursorH), h, nullptr);
                }
            }
            backup.Restore(s_context);
        }
        {
            std::lock_guard<std::mutex> lk(s_uiPassMutex);
            ReleaseQuads(s_uiPassQuads);
            if (defer) {
                s_uiPassQuads = std::move(queued);
                s_uiPassBackW = bw;
                s_uiPassBackH = bh;
                s_uiPassPublished = std::chrono::steady_clock::now();
            }
        }
        // VR presenter: runs every frame (an empty frame HIDES stale overlays);
        // no-op unless the runtime is live. Same thread as everything above.
        const auto stageBeforeVR = ClockT::now();
        VR::SubmitFrame(frame, s_focused.load(), static_cast<ViewId>(s_uiModeView.load()));
        const auto stageEnd = ClockT::now();

        s_acc.ulUpdate  += stageMs(stageT0, stageAfterUpdate);
        // Subtract THIS frame's render, not the running accumulator — the
        // latter grew every frame and drove the reported composite deeply
        // negative, which made the one number meant to localise cost useless.
        s_acc.composite += (std::max)(0.0, stageMs(stageAfterUpdate, stageBeforeVR) - frameUlRender);
        s_acc.vrSubmit  += stageMs(stageBeforeVR, stageEnd);
        const double whole = stageMs(stageT0, stageEnd);
        if (whole > s_acc.worst) s_acc.worst = whole;
        ++s_acc.frames;
        if (s_acc.lastReport.time_since_epoch().count() == 0) s_acc.lastReport = stageEnd;
        if (stageEnd - s_acc.lastReport >= std::chrono::seconds(2) && s_acc.frames > 0) {
            const double n = static_cast<double>(s_acc.frames);
            SKSE::log::info(
                "Magelight: present cost over {} frames — ul.update {:.2f}ms, ul.render {:.2f}ms, "
                "composite(+mirror) {:.2f}ms, vr.submit {:.2f}ms, total avg {:.2f}ms, WORST {:.2f}ms "
                "(uiMode={}, views={})",
                s_acc.frames, s_acc.ulUpdate / n, s_acc.ulRender / n, s_acc.composite / n,
                s_acc.vrSubmit / n,
                (s_acc.ulUpdate + s_acc.ulRender + s_acc.composite + s_acc.vrSubmit) / n,
                s_acc.worst, s_focused.load(), frame.size());
            s_acc = StageAcc{};
            s_acc.lastReport = stageEnd;
        }
    }

    // The carrier's PostDisplay: draw the published quads into the render target the game's UI pass has bound,
    // scaled from the back buffer they were laid out for. Main thread, inside the engine's UI render.
    static void UiPassCompositeImpl()
    {
        s_lastUiPassFrame.store(s_compositeFrames.load());
        s_uiPassSeen.store(true);
        if (!s_context || !s_device || s_renderDead.load()) return;
        std::vector<QueuedQuad> quads;
        float backW = 0, backH = 0;
        {
            std::lock_guard<std::mutex> lk(s_uiPassMutex);
            // Present stopped composing (a load, a stalled hook): draw nothing rather than a frozen frame.
            if (s_uiPassQuads.empty() ||
                std::chrono::steady_clock::now() - s_uiPassPublished > std::chrono::milliseconds(250)) return;
            quads = s_uiPassQuads;
            for (auto& q : quads) q.srv->AddRef();
            backW = s_uiPassBackW;
            backH = s_uiPassBackH;
        }
        StateBackup backup;
        backup.Capture(s_context);
        ID3D11RenderTargetView* target = backup.rtvs[0];
        float tw = 0, th = 0;
        if (target) {
            ID3D11Resource* res = nullptr;
            target->GetResource(&res);
            ID3D11Texture2D* tex = nullptr;
            if (res && SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex))) && tex) {
                D3D11_TEXTURE2D_DESC td{};
                tex->GetDesc(&td);
                tw = static_cast<float>(td.Width);
                th = static_cast<float>(td.Height);
                tex->Release();
                // Which target the UI pass draws into is the whole question on an unfamiliar setup: log each change.
                static void* s_lastTarget = nullptr;
                static int   s_targetLogs = 0;
                if (res != s_lastTarget && s_targetLogs < 10) {
                    s_lastTarget = res;
                    ++s_targetLogs;
                    SKSE::log::info("Magelight: UI pass draws into a {}x{} target, format {} (back buffer {}x{})",
                        td.Width, td.Height, static_cast<int>(td.Format), backW, backH);
                }
            }
            if (res) res->Release();
        }
        if (target && tw > 0 && th > 0 && backW > 0 && backH > 0) {
            StateBackup::Neutralize(s_context);
            const float sx = tw / backW, sy = th / backH;
            for (const auto& q : quads)
                DrawQuadTo(target, tw, th, q.srv, q.u0, q.v0, q.u1, q.v1,
                           q.x * sx, q.y * sy, q.w * sx, q.h * sy, q.hasCut ? q.cut : nullptr);
        } else {
            static std::atomic<int> s_noTargetLogs{ 0 };
            if (s_noTargetLogs.fetch_add(1) < 3)
                SKSE::log::warn("Magelight: UI pass had no usable render target bound - nothing drawn this frame");
            // 30 consecutive passes with nothing to draw on: stop believing the UI
            // pass will draw again, so pages composite at Present instead. A page
            // drawn at Present (frame generation may drop or smear it) beats an
            // open menu nobody can see. exchange() so the warning is once, not
            // every frame from here on.
            static int s_noTargetFrames = 0;
            if (!s_overlayMenuRegistered && ++s_noTargetFrames >= 30 && !s_uiPassBroken.exchange(true))
                SKSE::log::warn("Magelight: the UI pass has had no render target for 30 frames - "
                                "views draw at Present");
        }
        backup.Restore(s_context);
        ReleaseQuads(quads);
    }

    void UiPassComposite()
    {
        __try {
            UiPassCompositeImpl();
        } __except (LogSehAndDisable("UI pass", GetExceptionCode(),
                        (GetExceptionInformation())->ExceptionRecord->ExceptionAddress)) {
        }
    }

    // SEH wrapper — no locals with destructors allowed in here.
    void GuardedFrameWork(IDXGISwapChain* sc)
    {
        __try {
            FrameWork(sc);
        } __except (LogSehAndDisable("frame work", GetExceptionCode(),
                        (GetExceptionInformation())->ExceptionRecord->ExceptionAddress)) {
        }
    }

}  // namespace Magelight
