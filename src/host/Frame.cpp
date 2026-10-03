// The per-frame render path - stage 4 of the split. See Render.h for the
// objects this drives.
//
// ─ What this file is───────────────────────────────────────────
//
// ONE function's worth of work, once per presented frame, on the ONE thread
// allowed to touch Ultralight (CLAUDE.md invariant 1). The order below is the
// contract and is not interchangeable:
//
//   1. DrainInputQueue      - queued WM_* messages become Ultralight events
//   2. ApplyPendingResizes  - boundsDirty / scaleDirty / shimsDirty
//   3. MaterializeViews     - a queued CreateView becomes a real ultralight::View
//   4. ApplyPendingLifecycle- destroy / reload / navigate intents (invariant 13)
//   5. HibernateIdleViews   - release the View of a long-hidden view
//   6. Renderer::Render     - paints every visible view
//   7. DrawOverlay          - composite the visible views over the game
//
// ─ Rules that keep this correct ───────────────────────────────────────────
//
//  1. NOTHING IN HERE MAY RUN ON A FOREIGN THREAD. A frame from a thread other
//     than s_ulThreadId skips every line of this file, not just the Ultralight
//     calls - the D3D state we set belongs to a context the game owns.
//  2. VIEWS ARE ERASED ONLY IN ApplyPendingLifecycle (invariant 13), and the
//     registry entry is erased BEFORE the MlView's RefPtr<View> drops. Doing it
//     the other way round leaves a listener able to find a view whose View is
//     already gone.
//  3. LoadURL / Reload / release on a LIVE page happen OUTSIDE s_viewsMutex
//     (invariant 16). They run page handlers synchronously, which re-enter the
//     locking listeners; taking the same non-recursive mutex there throws
//     system_error, the SEH guard disables the overlay, and the UI is dead for
//     the session.

#include "host/Render.h"
#include "host/State.h"
#include "host/Views.h"
#include "host/Platform.h"
#include "host/StateBackup.h"   // the D3D state capture/restore pair

#include "Magelight.h"
#include "MagelightApi4.h"
#include "MagelightSound.h"
#include "MagelightVR.h"

#include <JavaScriptCore/JavaScript.h>
#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
#include <d3d11.h>
#include <windows.h>
#include <windowsx.h>   // GET_X_LPARAM / GET_Y_LPARAM for the queued mouse messages
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#pragma warning(push)
#pragma warning(disable : 4100)
#include <Ultralight/ImageSource.h>
#include <Ultralight/RenderTarget.h>
#include <Ultralight/Ultralight.h>
#include <Ultralight/platform/Platform.h>
#pragma warning(pop)

namespace Magelight {

    // Modifier state via GetAsyncKeyState - the render thread has no message
    // queue, so thread-local GetKeyState would lie here.
    static unsigned CurrentKeyModifiers()
    {
        unsigned mods = 0;
        if (GetAsyncKeyState(VK_MENU) & 0x8000)    mods |= ultralight::KeyEvent::kMod_AltKey;
        if (GetAsyncKeyState(VK_CONTROL) & 0x8000) mods |= ultralight::KeyEvent::kMod_CtrlKey;
        if (GetAsyncKeyState(VK_SHIFT) & 0x8000)   mods |= ultralight::KeyEvent::kMod_ShiftKey;
        if ((GetAsyncKeyState(VK_LWIN) | GetAsyncKeyState(VK_RWIN)) & 0x8000)
            mods |= ultralight::KeyEvent::kMod_MetaKey;
        return mods;
    }

    // ── The queued-input queue ────────────────────────────────────────────
    // Producer: QueueInput (Magelight.cpp, the window proc side, plus the IME
    // shim and the VR keyboard hook). Consumer: DrainInputQueue below. Neither
    // side may touch a View here — that is what keeps the game thread, which
    // produces, away from Ultralight (invariant 1).
    std::mutex            s_inputMutex;
    std::vector<InputMsg> s_inputQueue;

    // Translate queued Windows messages into Ultralight events. Render thread
    // only — the sole thread allowed to touch views.
    // Routing: keys/text -> the UI-mode (focused) view; mouse -> the topmost
    // visible non-clickThrough view under that event's coordinates, falling
    // back to the UI-mode view so drags that leave the panel keep working.
    void DrainInputQueue(float bw, float bh)
    {
        std::vector<InputMsg> msgs;
        {
            std::lock_guard<std::mutex> lk(s_inputMutex);
            msgs.swap(s_inputQueue);
        }
        if (msgs.empty()) return;

        ultralight::RefPtr<ultralight::View> uiUl;
        int uiX = 0, uiY = 0;
        {
            std::lock_guard<std::mutex> lk(s_viewsMutex);
            if (MlView* v = FindViewLocked(static_cast<ViewId>(s_uiModeView.load()))) {
                uiUl = v->ul;
                float ex = 0, ey = 0;
                EffectivePos(*v, bw, bh, ex, ey);
                uiX = static_cast<int>(ex);
                uiY = static_cast<int>(ey);
            }
        }

        // Topmost visible interactive view containing (mx,my); falls back to
        // the UI-mode view. Outputs the target's screen origin.
        // Also hands back the target's device scale and wheel step (0.28.5):
        // one lock and one scan per event - two lambdas used to re-lock and
        // re-scan for what this loop already had in hand (review 2026-09-09).
        auto hitTest = [&](int mx, int my, int& tx, int& ty, float& ds, int& step) -> ultralight::RefPtr<ultralight::View> {
            std::lock_guard<std::mutex> lk(s_viewsMutex);
            auto take = [&](const MlView& v) {
                ds   = v.deviceScale > 0.f ? v.deviceScale : 1.f;
                step = v.scrollStep > 0 ? v.scrollStep : 40;   // 0.28.0 per-view wheel step
            };
            for (auto it = s_views.rbegin(); it != s_views.rend(); ++it) {
                MlView& v = **it;
                if (!v.ul || !v.visible || v.clickThrough) continue;
                float ex = 0, ey = 0;
                EffectivePos(v, bw, bh, ex, ey);
                if (mx >= ex && my >= ey && mx < ex + v.w && my < ey + v.h) {
                    tx = static_cast<int>(ex);
                    ty = static_cast<int>(ey);
                    take(v);
                    return v.ul;
                }
            }
            tx = uiX; ty = uiY;
            if (const MlView* u = FindViewLocked(static_cast<ViewId>(s_uiModeView.load()))) take(*u);
            return uiUl;
        };

        // Ultralight takes mouse coordinates in LOGICAL (CSS) pixels: AppCore
        // converts window pixels with its DPI scale before FireMouseEvent, so
        // a view rendered at deviceScale S needs (px / S). Backbuffer-space px
        // reach here from both the Win32 sink and the VR laser (it posts into
        // the same queue), so this one spot covers both. `ds` below is the
        // hit view's scale, from hitTest.

        using UL = ultralight::MouseEvent;
        for (const auto& e : msgs) {
            switch (e.msg) {
            case 0:  // focus marker from SetUIMode - targets the UI-mode view
                if (!uiUl) break;
                if (e.wparam) uiUl->Focus();
                else uiUl->Unfocus();
                SKSE::log::info("Magelight: view {}", e.wparam ? "focused" : "unfocused");
                break;
            case WM_MOUSEMOVE: {
                int tx = 0, ty = 0, step = 40; float ds = 1.f;
                auto target = hitTest(GET_X_LPARAM(e.lparam), GET_Y_LPARAM(e.lparam), tx, ty, ds, step);
                if (!target) break;
                UL evt{ UL::kType_MouseMoved,
                        static_cast<int>((GET_X_LPARAM(e.lparam) - tx) / ds), static_cast<int>((GET_Y_LPARAM(e.lparam) - ty) / ds),
                        UL::kButton_None };
                target->FireMouseEvent(evt);
                break;
            }
            case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK:
            case WM_RBUTTONDOWN: case WM_MBUTTONDOWN: {
                const auto btn = (e.msg == WM_MBUTTONDOWN) ? UL::kButton_Middle
                    : (e.msg == WM_RBUTTONDOWN) ? UL::kButton_Right : UL::kButton_Left;
                int tx = 0, ty = 0, step = 40; float ds = 1.f;
                auto target = hitTest(GET_X_LPARAM(e.lparam), GET_Y_LPARAM(e.lparam), tx, ty, ds, step);
                if (!target) break;
                UL evt{ UL::kType_MouseDown,
                        static_cast<int>((GET_X_LPARAM(e.lparam) - tx) / ds), static_cast<int>((GET_Y_LPARAM(e.lparam) - ty) / ds), btn };
                target->FireMouseEvent(evt);
                break;
            }
            case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP: {
                const auto btn = (e.msg == WM_MBUTTONUP) ? UL::kButton_Middle
                    : (e.msg == WM_RBUTTONUP) ? UL::kButton_Right : UL::kButton_Left;
                int tx = 0, ty = 0, step = 40; float ds = 1.f;
                auto target = hitTest(GET_X_LPARAM(e.lparam), GET_Y_LPARAM(e.lparam), tx, ty, ds, step);
                if (!target) break;
                UL evt{ UL::kType_MouseUp,
                        static_cast<int>((GET_X_LPARAM(e.lparam) - tx) / ds), static_cast<int>((GET_Y_LPARAM(e.lparam) - ty) / ds), btn };
                target->FireMouseEvent(evt);
                break;
            }
            case WM_MOUSEWHEEL: {
                // ~40 px per notch (WHEEL_DELTA = 120). Scrolls whatever sits
                // under the cursor, like a real browser.
                int tx = 0, ty = 0, step = 40; float ds = 1.f;
                auto target = hitTest(GET_X_LPARAM(e.lparam), GET_Y_LPARAM(e.lparam), tx, ty, ds, step);
                if (!target) break;
                const int delta = GET_WHEEL_DELTA_WPARAM(e.wparam) * step / WHEEL_DELTA;   // default 40px per notch
                ultralight::ScrollEvent evt{ ultralight::ScrollEvent::kType_ScrollByPixel, 0, delta };
                target->FireScrollEvent(evt);
                break;
            }
            // Key fields are populated manually (0.3.4) rather than via the
            // SDK's Windows KeyEvent ctor. (The 0.3.1-0.3.5 fastfail family
            // was the wrong-thread WebKit main-thread assert - see
            // s_worldReady - not the events themselves.)
            case WM_KEYDOWN: case WM_SYSKEYDOWN: {
                if (!uiUl) break;
                ultralight::KeyEvent evt;
                evt.type = ultralight::KeyEvent::kType_RawKeyDown;
                evt.virtual_key_code = static_cast<int>(e.wparam);
                evt.native_key_code = static_cast<int>((e.lparam >> 16) & 0x1FF);
                // key_identifier is what the page's e.key / e.keyIdentifier
                // reads for non-printing keys (arrows, Escape, Enter) -
                // without it they are "Unidentified" and focus navigation
                // in pages breaks.
                ultralight::GetKeyIdentifierFromVirtualKeyCode(evt.virtual_key_code, evt.key_identifier);
                evt.modifiers = CurrentKeyModifiers();
                evt.is_system_key = (e.msg == WM_SYSKEYDOWN);
                evt.is_auto_repeat = (e.lparam & (1 << 30)) != 0;
                evt.is_keypad = false;
                uiUl->FireKeyEvent(evt);
                if ((e.wparam == VK_BACK || e.wparam == VK_DELETE) && !evt.is_auto_repeat) {
                    static std::atomic<int> s_n{ 0 };
                    if (s_n.fetch_add(1) < 8)
                        SKSE::log::info("Magelight: RawKeyDown vk=0x{:02X} id='{}' fired at view {}",
                            evt.virtual_key_code, ultralight::String(evt.key_identifier).utf8().data(),
                            static_cast<std::uint64_t>(s_uiModeView.load()));
                }
                break;
            }
            case WM_KEYUP: case WM_SYSKEYUP: {
                if (!uiUl) break;
                ultralight::KeyEvent evt;
                evt.type = ultralight::KeyEvent::kType_KeyUp;
                evt.virtual_key_code = static_cast<int>(e.wparam);
                evt.native_key_code = static_cast<int>((e.lparam >> 16) & 0x1FF);
                ultralight::GetKeyIdentifierFromVirtualKeyCode(evt.virtual_key_code, evt.key_identifier);
                evt.modifiers = CurrentKeyModifiers();
                evt.is_system_key = (e.msg == WM_SYSKEYUP);
                evt.is_keypad = false;
                uiUl->FireKeyEvent(evt);
                break;
            }
            case WM_CHAR: {
                if (!uiUl) break;
                const ultralight::Char16 wch = static_cast<ultralight::Char16>(e.wparam);
                // Control characters (Ctrl+C = 0x03, Ctrl+V = 0x16, ...) are
                // editing commands already delivered by the RawKeyDown with
                // kMod_CtrlKey; as text they would insert garbage. Tab and
                // Enter stay text (textarea newline / tab).
                if (wch < 0x20 && wch != L'\t' && wch != L'\r' && wch != L'\n') break;
                // A character outside the BMP (emoji, rare ideographs) arrives as two
                // WM_CHARs; a lone half fired on its own is U+FFFD in the page. Hold the
                // high half and fire the pair together (0.27.0).
                static ultralight::Char16 s_pendingHigh = 0;
                ultralight::Char16 units[2]; unsigned n = 0;
                if (wch >= 0xD800 && wch <= 0xDBFF) { s_pendingHigh = wch; break; }
                if (wch >= 0xDC00 && wch <= 0xDFFF) { if (!s_pendingHigh) break; units[n++] = s_pendingHigh; s_pendingHigh = 0; }
                units[n++] = wch;
                ultralight::String16 s16(units, n);
                ultralight::KeyEvent evt;
                evt.type = ultralight::KeyEvent::kType_Char;
                evt.modifiers = CurrentKeyModifiers();
                evt.text = ultralight::String(s16);
                evt.unmodified_text = evt.text;
                uiUl->FireKeyEvent(evt);
                break;
            }
            default:
                break;
            }
        }
    }

    // CPU-surface path: (re)create the view's dynamic texture to match its
    // current size. Render thread.
    bool EnsureCpuTexture(MlView& v)
    {
        if (v.tex && v.texW == v.w && v.texH == v.h) return true;
        if (v.srv) { v.srv->Release(); v.srv = nullptr; }
        if (v.tex) { v.tex->Release(); v.tex = nullptr; }
        D3D11_TEXTURE2D_DESC td{};
        td.Width = static_cast<UINT>(v.w);
        td.Height = static_cast<UINT>(v.h);
        td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DYNAMIC;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(s_device->CreateTexture2D(&td, nullptr, &v.tex))) return false;
        if (FAILED(s_device->CreateShaderResourceView(v.tex, nullptr, &v.srv))) {
            v.tex->Release(); v.tex = nullptr;
            return false;
        }
        v.texW = v.w; v.texH = v.h;
        return true;
    }

    void UploadSurfaceIfDirty(MlView& v)
    {
        auto* surface = static_cast<ultralight::BitmapSurface*>(v.ul->surface());
        if (!surface) return;
        if (surface->dirty_bounds().IsEmpty()) return;
        if (!EnsureCpuTexture(v)) return;

        ultralight::RefPtr<ultralight::Bitmap> bmp = surface->bitmap();
        if (!bmp) return;
        const void* px = bmp->LockPixels();
        if (px) {
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (SUCCEEDED(s_context->Map(v.tex, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
                const auto rowBytes = bmp->row_bytes();
                const auto rows = bmp->height();
                const auto copyBytes = (rowBytes < mapped.RowPitch) ? rowBytes : mapped.RowPitch;
                auto* src = static_cast<const std::uint8_t*>(px);
                auto* dst = static_cast<std::uint8_t*>(mapped.pData);
                for (std::uint32_t y = 0; y < rows; ++y) {
                    std::memcpy(dst + y * mapped.RowPitch, src + y * rowBytes, copyBytes);
                }
                s_context->Unmap(v.tex, 0);
            }
        }
        bmp->UnlockPixels();
        surface->ClearDirtyBounds();
    }

    // Everything we touch gets saved and restored - the game's renderer must

    // Composite one textured quad over the backbuffer. `srv` is the view's
    // pixels — the CPU path's uploaded texture (uv 0..1) or the GPU path's
    // render-target texture (uv from RenderTarget::uv_coords, since the target
    // may be padded). Pipeline state save/restore is the CALLER's job.
    //
    // DrawQuadTo and DrawOverlay are declared in host/Render.h. They do not
    // recurse, so no forward declaration is needed here.
    void DrawOverlay(IDXGISwapChain* sc, ID3D11ShaderResourceView* srv,
                     float u0, float v0, float u1, float v1,
                     float dx, float dy, float dw, float dh,
                     const float* cutUV)   // {x0,y0,x1,y1} in this quad's uv space
    {
        DXGI_SWAP_CHAIN_DESC scd{};
        if (FAILED(sc->GetDesc(&scd))) return;
        const float bw = static_cast<float>(scd.BufferDesc.Width);
        const float bh = static_cast<float>(scd.BufferDesc.Height);
        if (bw <= 0.0f || bh <= 0.0f || !srv) return;
        ID3D11Texture2D* back = nullptr;
        if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back))))
            return;
        ID3D11RenderTargetView* rtv = nullptr;
        const HRESULT rtvHr = s_device->CreateRenderTargetView(back, nullptr, &rtv);
        back->Release();
        if (FAILED(rtvHr)) return;
        DrawQuadTo(rtv, bw, bh, srv, u0, v0, u1, v1, dx, dy, dw, dh, cutUV);
        rtv->Release();
    }

    // One textured quad into `rtv`, a bw x bh target. Pipeline state save/restore is the CALLER's job.
    // Not static: the VR UI pass in Magelight.cpp composites its quads through
    // this too, so the declaration lives in host/Render.h.
    void DrawQuadTo(ID3D11RenderTargetView* rtv, float bw, float bh, ID3D11ShaderResourceView* srv,
                    float u0, float v0, float u1, float v1,
                    float dx, float dy, float dw, float dh,
                    const float* cutUV)
    {
        if (!rtv || bw <= 0.0f || bh <= 0.0f || !srv) return;

        // Quad in NDC at (dx, dy), dw x dh pixels.
        const float x0 = -1.0f + 2.0f * (dx / bw);
        const float y0 =  1.0f - 2.0f * (dy / bh);
        const float x1 = -1.0f + 2.0f * ((dx + dw) / bw);
        const float y1 =  1.0f - 2.0f * ((dy + dh) / bh);
        const Vertex quad[4] = {
            { x0, y0, u0, v0 },
            { x1, y0, u1, v0 },
            { x0, y1, u0, v1 },
            { x1, y1, u1, v1 },
        };
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(s_context->Map(s_vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return;
        std::memcpy(mapped.pData, quad, sizeof(quad));
        s_context->Unmap(s_vb, 0);

        const UINT stride = sizeof(Vertex), offset = 0;
        const FLOAT bf[4] = { 0, 0, 0, 0 };
        D3D11_VIEWPORT vp{ 0, 0, bw, bh, 0.0f, 1.0f };
        s_context->OMSetRenderTargets(1, &rtv, nullptr);
        s_context->RSSetViewports(1, &vp);
        s_context->RSSetState(nullptr);
        s_context->OMSetBlendState(s_blend, bf, 0xFFFFFFFF);
        s_context->OMSetDepthStencilState(nullptr, 0);
        s_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        s_context->IASetInputLayout(s_layout);
        s_context->IASetVertexBuffers(0, 1, &s_vb, &stride, &offset);
        s_context->VSSetShader(s_vs, nullptr, 0);
        s_context->PSSetShader(s_ps, nullptr, 0);
        s_context->GSSetShader(nullptr, nullptr, 0);
        s_context->HSSetShader(nullptr, nullptr, 0);
        s_context->DSSetShader(nullptr, nullptr, 0);
        s_context->PSSetShaderResources(0, 1, &srv);
        s_context->PSSetSamplers(0, 1, &s_sampler);
        {
            const float none[4] = { 0, 0, 0, 0 };
            D3D11_MAPPED_SUBRESOURCE cm{};
            if (SUCCEEDED(s_context->Map(s_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &cm))) {
                std::memcpy(cm.pData, cutUV ? cutUV : none, sizeof(none));
                s_context->Unmap(s_cb, 0);
            }
            s_context->PSSetConstantBuffers(0, 1, &s_cb);
        }
        s_context->Draw(4, 0);
    }

    // VR copy pass (docs/VR_PRESENTER.md §2): draw one presented view into a
    // HOST-OWNED render target at 1:1 — resolves the driver target's uv
    // padding, applies the cutout, and (straightAlpha) divides the
    // premultiplied colour out. No blending: the target is cleared first and
    // the quad covers it. Brackets its own pipeline state; Ultralight thread.
    void CopyViewToTarget(const PresentedView& pv, ID3D11RenderTargetView* rtv, int w, int h, bool straightAlpha,
                          const CursorMark* marks, int markCount)
    {
        if (!rtv || !pv.srv || w <= 0 || h <= 0 || !s_context) return;
        StateBackup backup;
        backup.Capture(s_context);
        StateBackup::Neutralize(s_context);

        const float clear[4] = { 0, 0, 0, 0 };
        s_context->ClearRenderTargetView(rtv, clear);
        const Vertex quad[4] = {
            { -1.0f,  1.0f, pv.u0, pv.v0 },
            {  1.0f,  1.0f, pv.u1, pv.v0 },
            { -1.0f, -1.0f, pv.u0, pv.v1 },
            {  1.0f, -1.0f, pv.u1, pv.v1 },
        };
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(s_context->Map(s_vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            std::memcpy(mapped.pData, quad, sizeof(quad));
            s_context->Unmap(s_vb, 0);
            const UINT stride = sizeof(Vertex), offset = 0;
            D3D11_VIEWPORT vp{ 0, 0, static_cast<float>(w), static_cast<float>(h), 0.0f, 1.0f };
            s_context->OMSetRenderTargets(1, &rtv, nullptr);
            s_context->RSSetViewports(1, &vp);
            s_context->RSSetState(nullptr);
            s_context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);   // overwrite, no blend
            s_context->OMSetDepthStencilState(nullptr, 0);
            s_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
            s_context->IASetInputLayout(s_layout);
            s_context->IASetVertexBuffers(0, 1, &s_vb, &stride, &offset);
            s_context->VSSetShader(s_vs, nullptr, 0);
            s_context->PSSetShader((straightAlpha && s_psStraight) ? s_psStraight : s_ps, nullptr, 0);
            s_context->GSSetShader(nullptr, nullptr, 0);
            s_context->HSSetShader(nullptr, nullptr, 0);
            s_context->DSSetShader(nullptr, nullptr, 0);
            ID3D11ShaderResourceView* srv = pv.srv;
            s_context->PSSetShaderResources(0, 1, &srv);
            s_context->PSSetSamplers(0, 1, &s_sampler);
            const float none[4] = { 0, 0, 0, 0 };
            D3D11_MAPPED_SUBRESOURCE cm{};
            if (SUCCEEDED(s_context->Map(s_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &cm))) {
                std::memcpy(cm.pData, pv.hasCutout ? pv.cutUV : none, sizeof(none));
                s_context->Unmap(s_cb, 0);
            }
            s_context->PSSetConstantBuffers(0, 1, &s_cb);
            s_context->Draw(4, 0);

            // Cursor marks (VR laser hits): the host's own cursor sprite, sized
            // like the flat cursor (36px @1080p, never under 24px), hotspot on
            // the hit pixel, composited over the (now straight-alpha) page.
            if (marks && markCount > 0 && s_cursorSrv) {
                // VR pointer sprite: a FRACTION of the panel, not the flat
                // cursor's 1080p-relative size. The old 24px floor was ~10% of a
                // 240px panel and read as a huge arrow stuck on the beam.
                // The laser-end pointer: a centred dot by default (0.26.12),
                // the cursor art (arrow / custom PNG, its own hotspot) if
                // "cursorDot": false or the dot texture failed.
                const bool dot = VR::CursorDot() && s_dotSrv;
                const float ch = std::clamp(static_cast<float>(h) * VR::CursorScale(), dot ? 4.0f : 8.0f, 32.0f);
                float cw = ch, hotX = 0.0f, hotY = 0.0f;
                if (dot) {
                    hotX = hotY = 0.5f;
                } else if (s_cursorCustom && s_cursorImgH > 0) {
                    cw = ch * (static_cast<float>(s_cursorImgW) / s_cursorImgH);
                    hotX = s_cursorHotX; hotY = s_cursorHotY;
                } else {
                    cw = ch * (static_cast<float>(kCursorW) / kCursorH);
                }
                const FLOAT bf[4] = { 0, 0, 0, 0 };
                s_context->OMSetBlendState((straightAlpha && s_blendStraight) ? s_blendStraight : s_blend, bf, 0xFFFFFFFF);
                s_context->PSSetShaderResources(0, 1, dot ? &s_dotSrv : &s_cursorSrv);
                if (SUCCEEDED(s_context->Map(s_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &cm))) {
                    std::memcpy(cm.pData, none, sizeof(none));
                    s_context->Unmap(s_cb, 0);
                }
                for (int i = 0; i < markCount; ++i) {
                    const float px = marks[i].u * w - hotX * cw;
                    const float py = marks[i].v * h - hotY * ch;
                    const float x0 = -1.0f + 2.0f * (px / w), y0 = 1.0f - 2.0f * (py / h);
                    const float x1 = -1.0f + 2.0f * ((px + cw) / w), y1 = 1.0f - 2.0f * ((py + ch) / h);
                    const Vertex cq[4] = { { x0, y0, 0, 0 }, { x1, y0, 1, 0 }, { x0, y1, 0, 1 }, { x1, y1, 1, 1 } };
                    if (SUCCEEDED(s_context->Map(s_vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
                        std::memcpy(mapped.pData, cq, sizeof(cq));
                        s_context->Unmap(s_vb, 0);
                        s_context->Draw(4, 0);
                    }
                }
            }
            // Unbind the target so the driver never samples a bound RT.
            ID3D11RenderTargetView* nullRtv = nullptr;
            s_context->OMSetRenderTargets(1, &nullRtv, nullptr);
        }
        backup.Restore(s_context);
    }

}  // namespace Magelight
