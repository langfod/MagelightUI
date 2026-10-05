#pragma once
// The host's D3D resources and renderer — stage 4 of the split.
//
// WHY THIS IS SEPARATE FROM State.h
// State.h is the small header: lifecycle flags, thread ids, GPU function
// pointers, a couple of paths. Nothing in it needs d3d11.h or Ultralight.
// This header does — it is a list of COM pointers and two RefPtrs — so it is
// kept apart so that a section which only wants s_runtimeDir or s_worldReady
// does not drag the whole D3D surface in.
//
// ── What these objects are ─────────────────────────────────────────────────
//
// One set of GPU resources, created lazily by the render-thread init inside
// the present hook and never touched again from anywhere else. They are NOT
// per-view: every view's texture is composited by the same shader pair and the
// same constant buffer, which is why they can be host-wide singletons at all.
//
//   s_vs / s_ps        the normal draw pair (premultiplied source-over)
//   s_psStraight       the VR copy pass (premultiplied -> straight)
//   s_layout / s_vb    the shared vertex layout and quad buffer
//   s_cb               PS b0: the compositor cutout rect
//   s_sampler          clamp + linear, shared
//   s_blend            premultiplied-alpha blend for the overlay pass
//   s_blendStraight    straight-alpha 'over' for the VR copy pass
//   s_cursorTex/Srv    our own cursor sprite (the game's cursor is drawn in
//                      the UI pass BEFORE Present, so it can never appear
//                      above our overlay — we draw ours on top)
//   s_dotTex/Srv       the VR laser-end pointer dot
//   s_device/s_context the GAME's device (never Released — a wrapper may not
//                      AddRef) and our own AddRef'd context
//   s_ulRenderer       the Ultralight renderer; every View is created from it
//
// ── Rules that keep this correct ───────────────────────────────────────────
//
//  1. RENDER-THREAD-OWNED after init. These are created lazily inside the
//     present hook and read only by the frame path. Nothing touches them
//     before EnsureRenderInit succeeds, and nothing touches them after except
//     to Release. `s_rendererUp` is the any-thread-readable latch for "the
//     renderer exists" — check THAT, not s_ulRenderer.
//  2. s_device is never Released. It came from the swapchain's GetDevice and a
//     wrapper is not obliged to AddRef it; releasing it on teardown would drop
//     a reference the game still owns.
//  3. The cursor and dot textures are owned here but their PIXELS are loaded
//     elsewhere (the cursor art decode is in the render section, the dot is
//     built by the VR presenter). Changing the cursor file must re-point
//     s_cursorSrv, never leak the old one — that is why these are named
//     pointers and not locals.

#include "Magelight.h"

#include <atomic>
#include <cstdint>

#include <d3d11.h>
#include <wrl/client.h>

#pragma warning(push)
#pragma warning(disable : 4100)
#include <Ultralight/Ultralight.h>
#pragma warning(pop)

namespace Magelight {

    // The DXGI trampoline the hook installs. Declared here because the render
    // section's init is what decides it exists (the hook itself is elsewhere).
    using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);

    extern PresentFn s_origPresent;

    extern bool                           s_renderInit;
    extern std::atomic<bool>              s_rendererUp;   // EnsureRenderInit succeeded (any-thread read)
    extern ultralight::RefPtr<ultralight::Renderer> s_ulRenderer;

    extern ID3D11Device*                s_device;   // from the swapchain's GetDevice; never Release
    extern ID3D11DeviceContext*         s_context;  // owned ref
    extern ID3D11VertexShader*          s_vs;
    extern ID3D11PixelShader*           s_ps;
    extern ID3D11PixelShader*           s_psStraight;   // VR copy pass (premultiplied -> straight)
    extern ID3D11InputLayout*           s_layout;
    extern ID3D11Buffer*                s_vb;
    extern ID3D11Buffer*                s_cb;      // PS b0: the cutout rect
    extern ID3D11SamplerState*          s_sampler;
    extern ID3D11BlendState*            s_blend;
    extern ID3D11BlendState*            s_blendStraight;   // VR copy pass: straight-alpha 'over'
    extern ID3D11Texture2D*             s_cursorTex;
    extern ID3D11ShaderResourceView*    s_cursorSrv;
    extern ID3D11Texture2D*             s_dotTex;  // VR laser-end pointer (0.26.12)
    extern ID3D11ShaderResourceView*    s_dotSrv;

    // Image file (anything WIC reads) -> premultiplied BGRA rows. No device work: any thread with COM up.
    bool DecodeImagePixels(const std::filesystem::path& path, unsigned maxPx, std::vector<std::uint8_t>& px,
                           int& outW, int& outH, std::string* why);
    // Rows from DecodeImagePixels -> immutable texture + SRV. Render thread (uses s_device).
    bool UploadPixels(const std::vector<std::uint8_t>& px, int w, int h,
                      ID3D11Texture2D** outTex, ID3D11ShaderResourceView** outSrv);
    // Both in one call, capped at 512: the player's cursorFile at device init. Render thread.
    bool DecodeCursorImageFile(const std::filesystem::path& path,
                               ID3D11Texture2D** outTex, ID3D11ShaderResourceView** outSrv,
                               int& outW, int& outH);

    // Cursor position mirrored from MenuCursor by the input sink (game side);
    // the render thread reads it to draw our own cursor sprite. The vanilla
    // cursor is drawn in the game's UI pass — BEFORE Present — so it can never
    // appear above our overlay; we draw our own on top instead.
    extern std::atomic<int> s_cursorPosX;
    extern std::atomic<int> s_cursorPosY;
    // A mouse button is down somewhere (set by the window proc's button cases in
    // host/Frame.cpp, read by the drawn cursor's press animation). Any thread:
    // the window proc is not the render thread.
    extern std::atomic<bool> s_mouseDown;

    // Backbuffer size for the cursor clamp (render thread writes, input sink reads).
    extern std::atomic<int> s_backbufferW;
    extern std::atomic<int> s_backbufferH;

    // The game swapchain is not a plain dxgi D3D11 swapchain, so assumptions about it
    // are unsafe: its IDXGISwapChain1 vtable may be too short to hold slot 22 (Community
    // Shaders' frame-generation proxy has 18), its GetBuffer may hand out its buffer
    // WITHOUT adding a reference, and that buffer may be on another device. Set once by
    // InstallHook's vtable guard (src/host/Hooks.cpp) and never cleared -- a swapchain
    // does not grow a vtable back. DrawOverlay reads it to decide whether it may Release
    // the back buffer. Read-only here: written on the game thread at hook install.
    extern std::atomic<bool> s_untrustedChain;

    // ── The baked fallback cursor ──────────────────────────────────────────
    // Draw.cpp rasterizes this arrow once into s_cursorTex; Frame.cpp needs its
    // aspect ratio for the quad when no custom art is loaded. These are the
    // BAKED arrow's fixed dimensions, NOT the loaded art's — those are measured
    // at load time into s_cursorImgW / s_cursorImgH (State.h).
    inline constexpr int kCursorW = 12;
    inline constexpr int kCursorH = 19;

    // ── Vertex layout ──────────────────────────────────────────────────────
    // The quad's four corners. Draw.cpp sizes the input-layout buffer with it
    // and Frame.cpp fills it, so the two halves of the render path must agree
    // on it exactly — hence the header, not a copy in each.
    struct Vertex { float x = 0.0f, y = 0.0f, u = 0.0f, v = 0.0f; };

    // ── Bring-up (host/Draw.cpp) ───────────────────────────────────────────
    // Called from the present hook on the game thread the first time the
    // swapchain is seen. Both are idempotent: CreateDeviceResources binds the
    // game's device + context and creates the shader pair; CreateUltralight
    // configures the Platform and creates the Renderer. Nothing here runs on
    // any thread but s_ulThreadId (invariant 1).
    bool CreateDeviceResources(IDXGISwapChain* sc);
    bool CreateUltralight();

    // MlView is declared in host/Views.h, which includes this header (for the
    // SRV and texture members), so only a forward declaration is needed here.
    struct MlView;

    // ── The per-frame pass (host/Frame.cpp) ────────────────────────────────
    // Drained once per presented frame, in this order: queued input becomes
    // Ultralight events, then surfaces are uploaded, then the overlay is
    // composited. Render thread only.
    void DrainInputQueue(float bw, float bh);
    bool EnsureCpuTexture(MlView& v);
    void UploadSurfaceIfDirty(MlView& v);
    // Swapchain-target guards used by DrawOverlay, and by the host when the chain is
    // untrusted. Render thread only.
    //
    // GetBufferGaveReference caches its answer per swapchain and ASSUMES a reference when it
    // cannot tell (false is the safe answer: leaking one reference beats freeing the game's
    // buffer). OnOurDeviceObject answers false when s_device is null, which makes DrawOverlay
    // skip drawing rather than draw into foreign memory.
    bool GetBufferGaveReference(IDXGISwapChain* sc, ID3D11Texture2D* first);
    void WarnNothingToDrawOn(const char* why);
    void DrawOverlay(IDXGISwapChain* sc, ID3D11ShaderResourceView* srv,
                     float u0, float v0, float u1, float v1,
                     float dx, float dy, float dw, float dh,
                     const float* cutUV = nullptr);

    // One textured quad into `rtv`. Pipeline state save/restore is the
    // CALLER's job (wrap it in StateBackup). `cutUV` is {x0,y0,x1,y1} in this
    // quad's own uv space; null composites the whole texture.
    void DrawQuadTo(ID3D11RenderTargetView* rtv, float bw, float bh,
                    ID3D11ShaderResourceView* srv,
                    float u0, float v0, float u1, float v1,
                    float dx, float dy, float dw, float dh,
                    const float* cutUV);

    // ── The composite decision (host/Render.cpp) ───────────────────────────
    // Stage 4.7 moved the UI-pass composite and the frame pass here; stage 4.8
    // moved the Present path that calls them into host/Hooks.cpp. These two are
    // called from InstallHook (src/host/Hooks.cpp), which is why they are
    // declared rather than left file-local.
    //
    // Registration is UNCONDITIONAL and inert until a view needs it, so the
    // engine reads it as an open menu only while it is actually open — which is
    // what keeps Escape opening the Journal.
    void RegisterOverlayMenu();

    // Logs which stage views start at, BEFORE any observation, so the log says
    // where views are drawn without the registration decision leaking into it.
    void LogCompositeMode();

    // "composite": "auto" | "present" | "ui" — where views are drawn. WRITTEN
    // by LoadHostSettings (src/host/Hooks.cpp), READ by UiCompositeWanted here.
    // Defined in this TU so the read side owns it; see the note at the old
    // site about not re-adding a file-local copy (the C4211 trap).
    extern std::string s_compositeMode;

    // The carrier menu's name, and the request to close it. AfterCompose (the
    // Present path, src/host/Hooks.cpp) must hide the carrier when the
    // renderer dies — an open menu over a dead overlay host leaves the engine
    // reading it as a menu, which is what stops Escape working.
    //
    // Rather than export the menu class, the render side owns the two things
    // that are actually needed. HideOverlayCarrier posts the hide through the
    // game task pump, so it is safe to call from the render thread.
    extern const char* const kOverlayMenuName;
    void HideOverlayCarrier();

    // The frame pass, entered from the Present path. Declared here so the hooks
    // can call it without knowing which TU it lives in.
    void GuardedFrameWork(IDXGISwapChain* sc);

    // 30 vtable presents in a row never composited: Present does not reach the
    // screen on this setup (a frame generator presents its own swapchain, on
    // its own device, after the frame is composed). Take the UI pass from now
    // on. Called from EndPresent on the present thread, so it only sets the
    // latch and posts the log line — the menu is opened by SyncOverlayMenu.
    void EnableUiPassOnFallback();

}  // namespace Magelight