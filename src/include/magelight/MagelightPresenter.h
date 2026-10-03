#pragma once
// Magelight UI — the presenter seam (docs/VR_PRESENTER.md §3).
//
// FrameWork builds ONE list of the views to present this frame and hands it
// to every consumer: the flat compositor (backbuffer quads — the behaviour
// the host has had since 0.3) and, on a VR runtime, the OpenVR overlay
// submitter (MagelightVR.h). The two must read the registry identically or
// the headset and the mirror drift; this struct is the single reading.
//
// Internal only (not part of api/MagelightUI_API.h). Built on the Ultralight
// thread inside FrameWork, consumed on the same thread, never stored across
// frames: `ul` is a strong ref for the duration of the frame so no consumer
// can outlive the view it is presenting (the tearing-down-texture rule).

#include <cstdint>
#include <vector>

#pragma warning(push)
#pragma warning(disable : 4100)
#include <Ultralight/RefPtr.h>
#include <Ultralight/View.h>
#pragma warning(pop)

#include "Magelight.h"

struct ID3D11ShaderResourceView;
struct ID3D11RenderTargetView;

namespace Magelight {

    struct PresentedView {
        ViewId id = 0;
        ultralight::RefPtr<ultralight::View> ul;   // live for this frame
        ID3D11ShaderResourceView* srv = nullptr;   // driver RT (GPU path) or the host's CPU texture
        float u0 = 0, v0 = 0, u1 = 1, v1 = 1;      // rt.uv_coords on the GPU path; 0..1 on CPU
        float x = 0, y = 0;                        // swapchain pixels (EffectivePos)
        int w = 0, h = 0;                          // view pixels
        bool hasCutout = false;
        float cutUV[4] = { 0, 0, 0, 0 };           // {x0,y0,x1,y1} in this view's uv space
        int layer = 1;                             // Layer enum value (z-order tier)
        bool gpuPath = true;                       // premultiplied BGRA either way; see the alpha switch
        bool clickThrough = false;                 // host hitTest skips these; the VR laser must too (B3)
        bool isInspector = false;                  // never presented in VR
        // Sampled from View::needs_paint() BEFORE Renderer::Render() clears
        // it — i.e. "this view's pixels change this frame". The flat
        // compositor redraws regardless (it costs one quad into a backbuffer
        // it is already writing); the VR submitter uses it to skip work that
        // is anything but free there. See the note on the VR copy pass.
        bool repainted = true;
    };

    using PresentedFrame = std::vector<PresentedView>;

    // A pointer mark to stamp INTO the copy target (VR laser hit): view uv.
    // Pages never draw a cursor (the OS does on flat; nothing does on VR), so
    // the host paints its cursor sprite into the panel texture at the hit.
    struct CursorMark { float u = 0.0f, v = 0.0f; };

    // Host copy pass (Magelight.cpp): draws one presented view into a target
    // the caller owns, at 1:1, cutout applied, optionally premultiplied ->
    // straight alpha, then up to `markCount` cursor sprites at the given uvs.
    // Ultralight thread only; brackets its own pipeline state.
    void CopyViewToTarget(const PresentedView& pv, ID3D11RenderTargetView* rtv, int w, int h, bool straightAlpha,
                          const CursorMark* marks = nullptr, int markCount = 0);

}  // namespace Magelight
