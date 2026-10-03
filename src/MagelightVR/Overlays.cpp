// VR presenter — the overlay RECORD's lifecycle: create, place, follow, submit,
// release. This file owns OverlayRec's invariants.
//
// What that means concretely: a handle is created with the bind-generation key,
// sized to the view, placed from a Placement, and released with the runtime's
// reference dropped BEFORE ours. Laser.cpp reaches into the same records for
// per-frame interaction (it writes pos/fwd/world during a drag), so the record
// is shared — but the discipline around each operation lives with the code that
// owns it. State.h rule 1 says the same thing.
//
// Present thread throughout.

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include "MagelightVR.h"
#include "Magelight.h"          // CopyViewToTarget
#include "MagelightPresenter.h" // PresentedView, CursorMark
#include "MagelightVR/State.h"

#include <algorithm>
#include <cmath>

#include <d3d11.h>
#include <windows.h>

namespace Magelight::VR {

    std::map<ViewId, OverlayRec> s_overlays;
    std::mutex s_placementMutex;
    std::map<ViewId, Placement> s_placementOverrides;
    std::map<ViewId, bool>      s_recenterRequests;
    std::map<ViewId, bool>      s_placementResetRequests;
    std::map<ViewId, Placement> s_placementSnapshot;
    std::uint64_t s_bindGeneration = 0;

    // VR submit accounting. Logged periodically while a panel is up so a
    // field log SAYS whether the overlay path is doing per-frame work,
    // instead of us inferring it from symptoms.
    std::uint64_t s_vrFrames = 0, s_vrSubmits = 0, s_vrReported = 0;

    // ── Placement maths ─────────────────────────────────────────────────────

    // 3x4 [R|t] multiply with the implicit bottom row [0 0 0 1].
    vr::HmdMatrix34_t MatMul(const vr::HmdMatrix34_t& A, const vr::HmdMatrix34_t& B)
    {
        vr::HmdMatrix34_t C{};
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j)
                C.m[i][j] = A.m[i][0] * B.m[0][j] + A.m[i][1] * B.m[1][j] + A.m[i][2] * B.m[2][j];
            C.m[i][3] = A.m[i][0] * B.m[0][3] + A.m[i][1] * B.m[1][3] + A.m[i][2] * B.m[2][3] + A.m[i][3];
        }
        return C;
    }

    // Head-relative placement matrix: overlay centre `distance` metres along the
    // HMD's -Z, `heightOffset` below eye height, facing the HMD (identity
    // rotation in HMD space — the quad's +Z looks back at the viewer).
    vr::HmdMatrix34_t HeadRelative(const Placement& p)
    {
        vr::HmdMatrix34_t m{};
        m.m[0][0] = 1.0f; m.m[1][1] = 1.0f; m.m[2][2] = 1.0f;
        m.m[0][3] = 0.0f;
        m.m[1][3] = p.heightOffset;
        m.m[2][3] = -p.distanceMeters;
        return m;
    }

    // Overlay +Z faces the viewer: Z = -forward (back toward the head), Y up,
    // X = Y x Z.
    vr::HmdMatrix34_t LevelMatrix(const float fwd[2], const float pos[3])
    {
        const float dx = fwd[0], dz = fwd[1];
        vr::HmdMatrix34_t M{};
        M.m[0][0] = -dz; M.m[1][0] = 0.0f; M.m[2][0] = dx;    // X
        M.m[0][1] = 0.0f; M.m[1][1] = 1.0f; M.m[2][1] = 0.0f;  // Y
        M.m[0][2] = -dx; M.m[1][2] = 0.0f; M.m[2][2] = -dz;   // Z
        M.m[0][3] = pos[0]; M.m[1][3] = pos[1]; M.m[2][3] = pos[2];
        return M;
    }

    // Every overlay width goes through here. Magelight.json supplies
    // panelWidthM unvalidated, and a zero or NaN width is not a thing the
    // runtime is obliged to survive.
    float ClampWidthM(float w)
    {
        if (!std::isfinite(w)) return 1.6f;
        return std::clamp(w, 0.05f, 12.0f);
    }

    // ── Lazy follow (present thread) ────────────────────────────────────────
    // A LEVEL panel: forward = the HMD's forward projected onto XZ, position
    // = head + forward*distance + heightOffset up. Absolute transform in the
    // compositor's space; the same matrix is what the laser hits.
    namespace {
        constexpr float kFollowRate     = 7.0f;      // 1/s; ~150 ms time constant
        constexpr float kSnapAngleCos   = 0.99966f;  // cos 1.5 deg
        constexpr float kSnapDistM      = 0.02f;

        // The gaze may leave the panel's EDGE by followAngleDeg before the
        // glide starts, so the deadzone grows with the panel instead of being
        // a fixed cone around its centre — look at a wide page's far column
        // and it stays put, which a centre-relative threshold cannot do.
        float FollowCosThreshold(const Placement& p)
        {
            const float dist = std::max(0.2f, p.distanceMeters);
            const float halfDeg = std::atan2(std::max(0.05f, p.widthMeters * 0.5f), dist) * 57.2957795f;
            const float fromEdge = s_settings.followAngleDeg + halfDeg;
            const float deg = std::clamp(std::max(fromEdge, s_settings.followMinDeg), 5.0f, 110.0f);
            return std::cos(deg * 0.01745329252f);
        }

        bool LazyTarget(const vr::HmdMatrix34_t& hmd, const Placement& p, float fwd[2], float pos[3])
        {
            float fx = -hmd.m[0][2], fz = -hmd.m[2][2];
            const float n = std::sqrt(fx * fx + fz * fz);
            if (n < 1e-3f) return false;   // looking straight up/down: keep the old heading
            fx /= n; fz /= n;
            fwd[0] = fx; fwd[1] = fz;
            pos[0] = hmd.m[0][3] + fx * p.distanceMeters;
            pos[1] = hmd.m[1][3] + p.heightOffset;
            pos[2] = hmd.m[2][3] + fz * p.distanceMeters;
            return true;
        }
    }  // namespace

    void TickFollow(OverlayRec& r, const vr::HmdMatrix34_t& hmd, float dt)
    {
        float tf[2], tp[3];
        if (!LazyTarget(hmd, r.placement, tf, tp)) {
            if (!r.placed) return;   // can't place yet
            tf[0] = r.fwd[0]; tf[1] = r.fwd[1];   // keep heading, still track position
            tp[0] = hmd.m[0][3] + tf[0] * r.placement.distanceMeters;
            tp[1] = hmd.m[1][3] + r.placement.heightOffset;
            tp[2] = hmd.m[2][3] + tf[1] * r.placement.distanceMeters;
        }
        bool write = false;
        if (!r.placed) {
            r.fwd[0] = tf[0]; r.fwd[1] = tf[1];
            r.pos[0] = tp[0]; r.pos[1] = tp[1]; r.pos[2] = tp[2];
            r.placed = true; r.following = false; write = true;
        } else if (r.placement.mode == Mode::LazyFollow) {
            const float cosang = r.fwd[0] * tf[0] + r.fwd[1] * tf[1];
            const float ddx = r.pos[0] - tp[0], ddy = r.pos[1] - tp[1], ddz = r.pos[2] - tp[2];
            const float dist = std::sqrt(ddx * ddx + ddy * ddy + ddz * ddz);
            if (!r.following &&
                (cosang < FollowCosThreshold(r.placement) ||
                 dist > std::max(0.05f, s_settings.followDistM)))
                r.following = true;
            if (r.following) {
                const float k = 1.0f - std::exp(-std::clamp(dt, 0.0f, 0.1f) * kFollowRate);
                float nx = r.fwd[0] + (tf[0] - r.fwd[0]) * k, nz = r.fwd[1] + (tf[1] - r.fwd[1]) * k;
                const float nn = std::sqrt(nx * nx + nz * nz);
                if (nn > 1e-4f) { r.fwd[0] = nx / nn; r.fwd[1] = nz / nn; }
                for (int i = 0; i < 3; ++i) r.pos[i] += (tp[i] - r.pos[i]) * k;
                const float c2 = r.fwd[0] * tf[0] + r.fwd[1] * tf[1];
                const float d2x = r.pos[0] - tp[0], d2y = r.pos[1] - tp[1], d2z = r.pos[2] - tp[2];
                if (c2 > kSnapAngleCos && std::sqrt(d2x * d2x + d2y * d2y + d2z * d2z) < kSnapDistM) {
                    r.fwd[0] = tf[0]; r.fwd[1] = tf[1];
                    r.pos[0] = tp[0]; r.pos[1] = tp[1]; r.pos[2] = tp[2];
                    r.following = false;
                }
                write = true;
            }
        }
        if (write) {
            r.world = LevelMatrix(r.fwd, r.pos);
            if (s_overlay && s_compositor && r.handle != vr::k_ulOverlayHandleInvalid)
                s_overlay->SetOverlayTransformAbsolute(r.handle, s_compositor->GetTrackingSpace(), &r.world);
        }
    }

    // ── Defaults and placement ──────────────────────────────────────────────

    // Host defaults by z-order tier (design §5): popups closer and smaller,
    // HUD widgets small and up to the right, panels the reading distance.
    Placement DefaultPlacementForLayer(int layer)
    {
        Placement p;
        switch (layer) {
        case 0:  p.mode = Mode::HeadLocked; p.distanceMeters = 1.5f; p.widthMeters = 0.45f; p.heightOffset = 0.30f; break;   // Hud: glued
        case 2:  p.distanceMeters = 1.2f; p.widthMeters = 0.90f; p.heightOffset = -0.10f; break;  // Popup
        case 3:  p.distanceMeters = 1.2f; p.widthMeters = 1.20f; p.heightOffset = 0.0f; break;    // System
        default:   // Panel — the full-page layer, driven by Magelight.json
            p.distanceMeters = s_settings.panelDistanceM;
            p.widthMeters    = s_settings.panelWidthM;
            p.heightOffset   = s_settings.panelHeightOffsetM;
            break;
        }
        // Every non-HUD surface honours the follow setting. A HUD widget
        // is glued to the head by definition and is not a "panel", so it
        // keeps HeadLocked regardless.
        if (layer != 0) {
            p.mode = s_settings.panelFollow ? Mode::LazyFollow : Mode::WorldLocked;
        }
        return p;
    }

    // TODO(VR-2): WorldLocked — read the HMD pose (GetLastPoses, standing
    // universe), compose HeadRelative into it with the yaw only (no roll/
    // pitch bake), SetOverlayTransformAbsolute, remember `world` for the
    // laser. HeadLocked uses SetOverlayTransformTrackedDeviceRelative and
    // reconstructs `world` every frame from the HMD pose for the laser.
    void ApplyPlacement(OverlayRec& r, const Placement& p)
    {
        r.placement = p;
        s_overlay->SetOverlayWidthInMeters(r.handle, ClampWidthM(p.widthMeters));
        switch (p.mode) {
        case Mode::HeadLocked: {
            const auto m = HeadRelative(p);
            s_overlay->SetOverlayTransformTrackedDeviceRelative(r.handle, vr::k_unTrackedDeviceIndex_Hmd, &m);
            break;
        }
        case Mode::LazyFollow:
        case Mode::WorldLocked:
            r.placed = false;   // TickFollow places it from the live HMD pose next frame
            r.following = false;
            return;
        case Mode::HandLocked:
            // TODO(later)
            break;
        }
        r.placed = true;
    }

    // ── Handle and texture lifecycle ────────────────────────────────────────

    void ReleaseRecLocked(OverlayRec& r)
    {
        if (s_overlay && r.handle != vr::k_ulOverlayHandleInvalid) {
            s_overlay->HideOverlay(r.handle);
            s_overlay->ClearOverlayTexture(r.handle);   // release the runtime's reference FIRST
            s_overlay->DestroyOverlay(r.handle);
            r.handle = vr::k_ulOverlayHandleInvalid;
        }
        if (r.srv) { r.srv->Release(); r.srv = nullptr; }
        if (r.rtv) { r.rtv->Release(); r.rtv = nullptr; }
        if (r.tex) { r.tex->Release(); r.tex = nullptr; }
        r.texW = r.texH = 0;
        r.shown = r.placed = false;
    }

    void ReleaseAllOverlays()
    {
        for (auto& [id, r] : s_overlays) ReleaseRecLocked(r);
        s_overlays.clear();
    }

    bool EnsureTexture(OverlayRec& r, int w, int h)
    {
        if (r.tex && r.texW == w && r.texH == h) return true;
        if (r.srv) { r.srv->Release(); r.srv = nullptr; }
        if (r.rtv) { r.rtv->Release(); r.rtv = nullptr; }
        if (r.tex) { r.tex->Release(); r.tex = nullptr; }
        D3D11_TEXTURE2D_DESC td{};
        td.Width = static_cast<UINT>(w);
        td.Height = static_cast<UINT>(h);
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;   // sRGB bytes, ColorSpace_Gamma on submit
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        // TODO(VR-2, verify): SteamVR's vrclient copies in-process, so no
        // MISC_SHARED is needed; flip it on if either runtime refuses.
        if (FAILED(s_device->CreateTexture2D(&td, nullptr, &r.tex))) return false;
        if (FAILED(s_device->CreateRenderTargetView(r.tex, nullptr, &r.rtv))) return false;
        if (FAILED(s_device->CreateShaderResourceView(r.tex, nullptr, &r.srv))) return false;
        // Transparent until the copy pass (VR-2) fills it: a fresh D3D
        // texture's contents are undefined, and undefined must never
        // reach the headset.
        const float clear[4] = { 0, 0, 0, 0 };
        s_context->ClearRenderTargetView(r.rtv, clear);
        r.texW = w; r.texH = h;
        return true;
    }

    bool EnsureOverlay(ViewId id, OverlayRec& r, int layer)
    {
        if (r.handle != vr::k_ulOverlayHandleInvalid) return true;
        // The key carries the bind generation: if a previous overlay of
        // ours ever survives teardown (a runtime restart mid-destroy), the
        // next CreateOverlay must not collide with its corpse.
        const std::string key = kOverlayKeyPrefix + std::to_string(id) +
                                "." + std::to_string(s_bindGeneration);
        const std::string name = "Magelight view " + std::to_string(id);
        const auto err = s_overlay->CreateOverlay(key.c_str(), name.c_str(), &r.handle);
        if (err != vr::VROverlayError_None) {
            // LATCHED. This runs per view per frame on a persistent
            // failure, and the logger is flush-on-trace — an unlatched
            // error here is a synchronous disk flush at frame rate, which
            // turns a cosmetic fault into a frame-rate collapse. Mirrors
            // the SetOverlayTexture failure path.
            static int s_logged = 0;
            if (s_logged++ < 4)
                SKSE::log::error("Magelight VR: CreateOverlay for view {} failed ({}) — "
                                 "logged at most 4x per session", id, static_cast<int>(err));
            r.handle = vr::k_ulOverlayHandleInvalid;
            return false;
        }
        s_overlay->SetOverlayWidthInMeters(r.handle, ClampWidthM(r.placement.widthMeters));
        s_overlay->SetOverlayTextureColorSpace(r.handle, vr::ColorSpace_Gamma);
        s_overlay->SetOverlayAlpha(r.handle, 1.0f);
        // Input is OURS (design §6): no VROverlayInputMethod_Mouse, no
        // runtime laser — OpenComposite would not deliver it anyway.
        s_overlay->SetOverlayInputMethod(r.handle, vr::VROverlayInputMethod_None);
        s_overlay->SetOverlayFlag(r.handle, vr::VROverlayFlags_SortWithNonSceneOverlays, true);
        s_overlay->SetOverlaySortOrder(r.handle, static_cast<std::uint32_t>(10 * (layer + 1)));
        SKSE::log::info("Magelight VR: overlay created for view {} ({}, layer {}, {:.2f}m wide at {:.2f}m)",
            id, key, layer, r.placement.widthMeters, r.placement.distanceMeters);
        return true;
    }

    // ── Per-frame submit ────────────────────────────────────────────────────

    // Copy the view into the overlay's texture and hand it to the runtime,
    // but ONLY when something actually changed. Returns true if it submitted.
    //
    // This used to run EVERY frame regardless: a full-panel render-target copy
    // plus SetOverlayTexture, which hands a texture across to the compositor
    // process. At 1600x900 that is ~5.8 MB per frame, ~90 times a second, for
    // a page that is usually perfectly still. The flat compositor has no
    // equivalent cost (it draws the SRV straight into a backbuffer it is
    // already writing), so this overlay-submission cost stayed invisible until
    // a page was driven from inside a headset.
    //
    // A view is resubmitted when its pixels changed (needs_paint, sampled
    // before Render cleared it), when the laser dot moved (we stamp that INTO
    // the texture), or when there is no valid submission yet. Everything else
    // the compositor already holds.
    bool SubmitOverlay(OverlayRec& r, const PresentedView& pv, bool uiModeOn)
    {
        // Cursor marks: last frame's laser hits on THIS view (per hand).
        CursorMark marks[2]; int nm = 0;
        if (uiModeOn)
            for (int h = 0; h < 2; ++h)
                if (s_hover[h].hit && s_hover[h].view == pv.id) { marks[nm].u = s_hover[h].u; marks[nm].v = s_hover[h].v; ++nm; }

        // Quantise to a texel BEFORE comparing, and stamp the quantised value,
        // so the mark the gate tests is the mark that gets drawn. Raw u/v come
        // straight from the ray-quad intersection, so hand tremor of a fraction
        // of a degree moved them every single frame — which made marksMoved
        // true whenever the laser touched a panel and defeated the whole
        // needs_paint gate exactly when the user was interacting.
        for (int i = 0; i < nm; ++i) {
            if (r.texW > 0) marks[i].u = std::lround(marks[i].u * r.texW) / static_cast<float>(r.texW);
            if (r.texH > 0) marks[i].v = std::lround(marks[i].v * r.texH) / static_cast<float>(r.texH);
        }
        bool marksMoved = (nm != r.lastMarkCount);
        for (int i = 0; !marksMoved && i < nm; ++i)
            marksMoved = (marks[i].u != r.lastMarkU[i]) || (marks[i].v != r.lastMarkV[i]);
        const bool needSubmit = pv.repainted || marksMoved || !r.everSubmitted || !r.shown;
        if (!needSubmit) return false;

        ++s_vrSubmits;
        r.lastMarkCount = nm;
        for (int i = 0; i < 2; ++i) {
            r.lastMarkU[i] = (i < nm) ? marks[i].u : -1.0f;
            r.lastMarkV[i] = (i < nm) ? marks[i].v : -1.0f;
        }

        // The copy pass (design §2): the runtime only ever sees r.tex.
        Magelight::CopyViewToTarget(pv, r.rtv, pv.w, pv.h, s_settings.straightAlpha, nm ? marks : nullptr, nm);

        vr::Texture_t tex{ r.tex, vr::TextureType_DirectX, vr::ColorSpace_Gamma };
        const auto err = s_overlay->SetOverlayTexture(r.handle, &tex);
        if (err == vr::VROverlayError_None) r.everSubmitted = true;
        if (err != vr::VROverlayError_None) {
            static int s_logged = 0;
            if (s_logged++ < 4)
                SKSE::log::error("Magelight VR: SetOverlayTexture view {} failed ({}) — PermissionDenied means the "
                                 "proxied interface, see design §7", pv.id, static_cast<int>(err));
            return false;
        }
        if (!r.shown) {
            const auto serr = s_overlay->ShowOverlay(r.handle);
            // Only latch on success: !r.shown is one of the terms that
            // forces a resubmit, so latching a FAILED show meant the view
            // was never retried and stayed invisible for good.
            r.shown = (serr == vr::VROverlayError_None);
            SKSE::log::info("Magelight VR: overlay shown for view {} ({}x{}, {})", pv.id, pv.w, pv.h, static_cast<int>(serr));
        }
        return true;
    }

    void ReportSubmitRate()
    {
        ++s_vrFrames;   // FRAMES, not view-frames — the denominator must be comparable
        if (s_vrFrames - s_vrReported < 600) return;
        SKSE::log::info("Magelight VR: submit rate — {} of the last {} presented frames "
                        "needed a texture upload ({}%)",
                        s_vrSubmits, s_vrFrames,
                        s_vrFrames ? (s_vrSubmits * 100 / s_vrFrames) : 0);
        s_vrReported = s_vrFrames;
        s_vrSubmits = 0;
        s_vrFrames = 0;
        s_vrReported = 0;
    }

}  // namespace Magelight::VR