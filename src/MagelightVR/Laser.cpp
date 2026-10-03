// VR presenter — per-frame interaction with placed panels: ray hit-testing,
// beams, hover, the runtime keyboard, grab-and-move.
//
// This is the other half of the pair Overlays.cpp starts. Overlays owns a
// record's lifecycle; this file reaches into the same records to answer "what
// is the user pointing at" and to write back while a panel is being dragged.
// Keeping the two apart is worth the shared record because each is doing one
// thing: Overlays decides where a panel IS, this decides what the hands DO to
// it.
//
// Present thread throughout, except NoteTextFocus (the render thread's JS
// dispatcher) and BeginViewDrag/EndViewDrag (the page's drag bridge).

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include "MagelightVR.h"
#include "Magelight.h"          // QueueSyntheticInput
#include "MagelightPresenter.h" // PresentedFrame, PresentedView
#include "State.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>

#include <windows.h>

namespace Magelight::VR {

    HoverState s_hover[2];
    ViewId s_kbView = 0;
    bool   s_kbShown = false;

    namespace {
        // Trigger click LEVEL per hand, present thread only (M6: level, not the
        // net edge counter which loses a press+release inside one frame).
        bool s_lastTriggerDown[2] = { false, false };
        bool s_gripWas[2] = { false, false };            // polled grip level, rising-edge latch
        bool s_gripSeeded[2] = { false, false };         // latch seeded from the live level on UI-mode entry
        int  s_lastPx[2] = { 0, 0 }, s_lastPy[2] = { 0, 0 };   // last hit pixel per hand (the UP lands here)
        float s_scrollAccum[2] = { 0.0f, 0.0f };         // thumbstick -> wheel notches
        constexpr float kStickDeadzone = 0.30f;
        constexpr float kScrollNotchesPerSec = 6.0f;     // at full deflection
        constexpr std::uint64_t kStickStaleMs = 250;     // no stick event this long = centred

        // Grab-and-move state (present thread; the request comes from the page).
        std::atomic<std::uint64_t> s_dragRequest{ 0 };   // view the page asked to drag (0 = none)
        ViewId s_dragView = 0;                           // active drag
        int    s_dragHand = -1;
        float  s_dragDist = 1.1f;

        // ── Aim frame ──────────────────────────────────────────────────────
        // Preferred: the controller's render-model "tip" component. OpenVR
        // defines its local frame with -Z out of the surface, which is exactly
        // the ray convention IntersectQuad uses, and it is per-controller
        // correct — a Touch, an Index and a Vive wand all hold differently, so
        // one hard-coded tilt can only ever suit one of them. Resolved once per
        // device (the component is static; the model name changes only if the
        // controller does) and cached. Fallback when a runtime serves no render
        // models or the model has no tip: the fixed local-X pitch below.
        struct AimCache {
            std::string model;                 // Prop_RenderModelName_String
            vr::HmdMatrix34_t tip{};
            bool haveTip = false;
            bool resolved = false;
            int  recheck = 0;   // frames until the model name is re-queried
        };
        AimCache s_aim[2];

        void ResolveAim(int hand, vr::TrackedDeviceIndex_t idx, const vr::VRControllerState_t& st)
        {
            if (!s_system) return;
            // The property query below is an IPC round-trip to the runtime and
            // it sat ABOVE the cache check, so it ran twice a frame forever to
            // answer a question whose answer changes only when a controller is
            // swapped. Re-ask about once a second; the cached aim is used in
            // between. (Audit finding B5, 2026-09-04.)
            AimCache& cached = s_aim[hand];
            if (cached.resolved && cached.recheck > 0) { --cached.recheck; return; }
            // Render-model names are short; k_unMaxPropertyStringSize is 32 KB
            // and has no business on the present thread's stack.
            char name[256]{};
            vr::ETrackedPropertyError perr = vr::TrackedProp_Success;
            s_system->GetStringTrackedDeviceProperty(idx, vr::Prop_RenderModelName_String,
                                                     name, sizeof(name), &perr);
            const std::string model = (perr == vr::TrackedProp_Success) ? name : std::string();
            AimCache& a = s_aim[hand];
            if (a.resolved && a.model == model) { a.recheck = 90; return; }   // unchanged
            a = AimCache{};
            a.model = model;
            a.resolved = true;
            a.recheck = 90;
            if (model.empty() || !s_renderModels || !s_settings.aimUseTip) {
                SKSE::log::info("Magelight VR: hand {} aim = fixed pitch {:.1f} deg (model '{}')",
                    hand, s_settings.aimPitchDeg, model);
                return;
            }
            if (!s_renderModels->RenderModelHasComponent(model.c_str(), vr::k_pch_Controller_Component_Tip)) {
                SKSE::log::info("Magelight VR: hand {} model '{}' has no tip component — fixed pitch {:.1f} deg",
                    hand, model, s_settings.aimPitchDeg);
                return;
            }
            vr::RenderModel_ComponentState_t cs{};
            vr::RenderModel_ControllerMode_State_t mode{};
            if (s_renderModels->GetComponentState(model.c_str(), vr::k_pch_Controller_Component_Tip,
                                                  &st, &mode, &cs)) {
                a.tip = cs.mTrackingToComponentLocal;   // -Z out of the surface
                a.haveTip = true;
                SKSE::log::info("Magelight VR: hand {} aim = render-model tip of '{}'", hand, model);
            } else {
                SKSE::log::info("Magelight VR: hand {} tip state unavailable for '{}' — fixed pitch {:.1f} deg",
                    hand, model, s_settings.aimPitchDeg);
            }
        }

        vr::HmdMatrix34_t ApplyLocalPitch(const vr::HmdMatrix34_t& m, float deg)
        {
            if (deg == 0.0f) return m;
            const float r = deg * 0.01745329252f, c = std::cos(r), sn = std::sin(r);
            vr::HmdMatrix34_t o = m;   // X (col 0) and translation (col 3) unchanged
            for (int i = 0; i < 3; ++i) {
                const float y = m.m[i][1], z = m.m[i][2];
                o.m[i][1] =  c * y + sn * z;
                o.m[i][2] = -sn * y + c * z;
            }
            return o;
        }

        // ── Beam overlays (one per hand, SESSION-GLOBAL like the probe) ──
        // A static gradient texture set ONCE (never recreated while the
        // runtime references it — recreating one it still holds is a known
        // teardown fault); per frame only width + an absolute transform that
        // billboards the strip toward the HMD so it is never seen edge-on.
        // Released in Shutdown only, never in ReleaseView.
        vr::VROverlayHandle_t s_beam[2] = { vr::k_ulOverlayHandleInvalid, vr::k_ulOverlayHandleInvalid };
        bool                  s_beamShown[2] = { false, false };
        ID3D11Texture2D*      s_beamTex = nullptr;
        // Wider texture = THINNER beam: the overlay's height is
        // width(metres) * texH/texW, so 512x2 is half the thickness 256x2 gave
        // (~0.8 cm at 2 m). White, fading toward the tip — the thin pale beam
        // the runtime's own pointer uses, rather than a fat coloured one.
        constexpr int         kBeamTexW = 512, kBeamTexH = 2;
        constexpr float       kBeamIdleLen = 2.0f;   // metres when nothing is hit

        bool EnsureBeamTexture()
        {
            if (s_beamTex || !s_device) return s_beamTex != nullptr;
            static std::uint32_t px[kBeamTexW * kBeamTexH];
            for (int y = 0; y < kBeamTexH; ++y)
                for (int x = 0; x < kBeamTexW; ++x) {
                    const float t = static_cast<float>(x) / (kBeamTexW - 1);
                    const std::uint32_t a = static_cast<std::uint32_t>(std::lround(200.0f * (1.0f - t * t)));
                    px[y * kBeamTexW + x] = (a << 24) | (0xFFu << 16) | (0xFFu << 8) | 0xFFu;   // straight BGRA: white
                }
            D3D11_TEXTURE2D_DESC td{};
            td.Width = kBeamTexW; td.Height = kBeamTexH; td.MipLevels = 1; td.ArraySize = 1;
            td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_IMMUTABLE; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            D3D11_SUBRESOURCE_DATA sub{ px, kBeamTexW * sizeof(std::uint32_t), 0 };
            if (FAILED(s_device->CreateTexture2D(&td, &sub, &s_beamTex))) { s_beamTex = nullptr; return false; }
            return true;
        }

        bool EnsureBeam(int hand)
        {
            if (s_beam[hand] != vr::k_ulOverlayHandleInvalid) return true;
            if (!s_overlay || !EnsureBeamTexture()) return false;
            const char* key  = hand ? "severause.magelight.beam.right" : "severause.magelight.beam.left";
            const char* name = hand ? "Magelight beam R" : "Magelight beam L";
            const auto err = s_overlay->CreateOverlay(key, name, &s_beam[hand]);
            if (err != vr::VROverlayError_None) {
                s_beam[hand] = vr::k_ulOverlayHandleInvalid;
                static int logged = 0;
                if (logged++ < 2) SKSE::log::error("Magelight VR: beam overlay {} failed ({})", key, static_cast<int>(err));
                return false;
            }
            vr::Texture_t tex{ s_beamTex, vr::TextureType_DirectX, vr::ColorSpace_Gamma };
            s_overlay->SetOverlayTexture(s_beam[hand], &tex);   // once; static texture
            s_overlay->SetOverlayWidthInMeters(s_beam[hand], kBeamIdleLen);
            s_overlay->SetOverlayAlpha(s_beam[hand], std::clamp(s_settings.beamAlpha, 0.05f, 1.0f));
            s_overlay->SetOverlayInputMethod(s_beam[hand], vr::VROverlayInputMethod_None);
            s_overlay->SetOverlayFlag(s_beam[hand], vr::VROverlayFlags_SortWithNonSceneOverlays, true);
            s_overlay->SetOverlaySortOrder(s_beam[hand], 100);
            SKSE::log::info("Magelight VR: beam overlay created ({})", key);
            return true;
        }

        void HideBeam(int hand)
        {
            if (s_beamShown[hand] && s_overlay && s_beam[hand] != vr::k_ulOverlayHandleInvalid)
                s_overlay->HideOverlay(s_beam[hand]);
            s_beamShown[hand] = false;
        }

        // Strip from the controller along the (pitched) ray, `len` metres,
        // billboarded toward the HMD. X = ray, Z = toward head (orthogonal to
        // the ray), Y = Z x X. Absolute transform in the compositor's space.
        void UpdateBeam(int hand, const vr::HmdMatrix34_t& ctrl, const vr::HmdMatrix34_t& hmd, float len)
        {
            if (!s_settings.beam) { HideBeam(hand); return; }
            if (!EnsureBeam(hand)) return;
            const float px = ctrl.m[0][3], py = ctrl.m[1][3], pz = ctrl.m[2][3];
            const float dx = -ctrl.m[0][2], dy = -ctrl.m[1][2], dz = -ctrl.m[2][2];
            const float cx = px + dx * len * 0.5f, cy = py + dy * len * 0.5f, cz = pz + dz * len * 0.5f;
            float vx = hmd.m[0][3] - cx, vy = hmd.m[1][3] - cy, vz = hmd.m[2][3] - cz;
            const float along = vx * dx + vy * dy + vz * dz;
            vx -= dx * along; vy -= dy * along; vz -= dz * along;
            const float n = std::sqrt(vx * vx + vy * vy + vz * vz);
            float zx, zy, zz;
            if (n < 1e-4f) { zx = ctrl.m[0][1]; zy = ctrl.m[1][1]; zz = ctrl.m[2][1]; }   // degenerate: controller Y
            else { zx = vx / n; zy = vy / n; zz = vz / n; }
            // Y = Z x X
            const float yx = zy * dz - zz * dy, yy = zz * dx - zx * dz, yz = zx * dy - zy * dx;
            vr::HmdMatrix34_t M{};
            M.m[0][0] = dx; M.m[1][0] = dy; M.m[2][0] = dz;
            M.m[0][1] = yx; M.m[1][1] = yy; M.m[2][1] = yz;
            M.m[0][2] = zx; M.m[1][2] = zy; M.m[2][2] = zz;
            M.m[0][3] = cx; M.m[1][3] = cy; M.m[2][3] = cz;
            // `len` is the ray-hit distance, and IntersectQuad only guarantees
            // it is > 0 — as the hand approaches the panel plane it tends to
            // zero, and reaching THROUGH the panel makes it denormal-small.
            // OpenVR wants a positive width and a runtime is entitled to do
            // anything with a zero one; a beam that thin is invisible anyway.
            // Suspected in the "reaching out past a threshold freezes the
            // game" report (2026-09-04) — the threshold is the panel distance.
            if (!std::isfinite(len) || len < 0.02f) { HideBeam(hand); return; }
            s_overlay->SetOverlayWidthInMeters(s_beam[hand], len);
            s_overlay->SetOverlayTransformAbsolute(s_beam[hand], s_compositor->GetTrackingSpace(), &M);
            if (!s_beamShown[hand]) { s_overlay->ShowOverlay(s_beam[hand]); s_beamShown[hand] = true; }
        }

        // Ray/quad intersection against a view's authored world transform.
        // Returns uv in [0,1] (u right, v DOWN — view space) and the hit distance.
        bool IntersectQuad(const vr::HmdMatrix34_t& quad, float widthM, float heightM,
                           const vr::HmdMatrix34_t& ctrl, float& u, float& v, float& dist)
        {
            // Quad basis (columns of the rotation): X right, Y up, Z out of the panel.
            const float ox = quad.m[0][3], oy = quad.m[1][3], oz = quad.m[2][3];
            const float xx = quad.m[0][0], xy = quad.m[1][0], xz = quad.m[2][0];
            const float yx = quad.m[0][1], yy = quad.m[1][1], yz = quad.m[2][1];
            const float nx = quad.m[0][2], ny = quad.m[1][2], nz = quad.m[2][2];
            // Controller ray: origin = translation, direction = -Z of the pose.
            const float px = ctrl.m[0][3], py = ctrl.m[1][3], pz = ctrl.m[2][3];
            const float dx = -ctrl.m[0][2], dy = -ctrl.m[1][2], dz = -ctrl.m[2][2];
            const float denom = dx * nx + dy * ny + dz * nz;
            if (std::fabs(denom) < 1e-5f) return false;
            const float t = ((ox - px) * nx + (oy - py) * ny + (oz - pz) * nz) / denom;
            if (t <= 0.0f) return false;
            const float hx = px + dx * t - ox, hy = py + dy * t - oy, hz = pz + dz * t - oz;
            const float lx = hx * xx + hy * xy + hz * xz;   // metres along the quad's X
            const float ly = hx * yx + hy * yy + hz * yz;   // metres along Y (up)
            if (std::fabs(lx) > widthM * 0.5f || std::fabs(ly) > heightM * 0.5f) return false;
            u = lx / widthM + 0.5f;
            v = 0.5f - ly / heightM;
            dist = t;
            return true;
        }
    }  // namespace

    // Runtime restart (init token changed): every handle is dead. Forget them;
    // the texture is ours and survives.
    void ForgetBeams()
    {
        for (int h = 0; h < 2; ++h) { s_beam[h] = vr::k_ulOverlayHandleInvalid; s_beamShown[h] = false; }
    }

    void ReleaseBeams()
    {
        for (int h = 0; h < 2; ++h) {
            if (s_overlay && s_beam[h] != vr::k_ulOverlayHandleInvalid) {
                s_overlay->HideOverlay(s_beam[h]);
                s_overlay->ClearOverlayTexture(s_beam[h]);
                s_overlay->DestroyOverlay(s_beam[h]);
            }
            s_beam[h] = vr::k_ulOverlayHandleInvalid;
            s_beamShown[h] = false;
        }
        if (s_beamTex) { s_beamTex->Release(); s_beamTex = nullptr; }
    }

    void ForgetAim()
    {
        s_aim[0] = AimCache{}; s_aim[1] = AimCache{};
    }

    void BeginViewDrag(ViewId view) { s_dragRequest.store(view); }

    void EndViewDrag()
    {
        if (s_dragView) SKSE::log::info("Magelight VR: view {} released", s_dragView);
        s_dragRequest.store(0);
        s_dragView = 0;
        s_dragHand = -1;
    }

    void NoteTextFocus(ViewId view, bool focused)
    {
        // Render thread (the JS dispatcher). TickKeyboard acts on it next frame.
        if (focused) s_kbView = view;
        else if (s_kbView == view) s_kbView = 0;
    }

    // ── SteamVR virtual keyboard ───────────────────────────────────────────
    void TickKeyboard()
    {
        if (!s_overlay) return;
        if (!s_settings.runtimeKeyboard) {
            static bool told = false;
            if (!told && s_kbView) {
                told = true;
                SKSE::log::info("Magelight VR: a text field has focus; the runtime keyboard is OFF by default "
                                "(it steals input focus and stalls the game) — type on the keyboard, or on the "
                                "runtime's own if it synthesises scancodes");
            }
            if (s_kbShown) { s_overlay->HideKeyboard(); s_kbShown = false; }
            return;
        }
        // Raise / drop the runtime keyboard to follow the page's text focus.
        const ViewId want = s_kbView;
        if (want && !s_kbShown) {
            auto it = s_overlays.find(want);
            if (it != s_overlays.end() && it->second.handle != vr::k_ulOverlayHandleInvalid) {
                // MINIMAL MODE (true): the runtime sends one
                // VREvent_KeyboardCharInput per keystroke instead of
                // keeping its own text buffer and handing the whole string
                // over at Done. We are typing into a live web page, so we
                // want the keystrokes (field 2026-09-03: with minimal mode
                // off, SteamVR's keyboard opened and produced nothing).
                const auto err = s_overlay->ShowKeyboardForOverlay(
                    it->second.handle, vr::k_EGamepadTextInputModeNormal,
                    vr::k_EGamepadTextInputLineModeSingleLine, "Magelight", 256, "", true, 0);
                if (err == vr::VROverlayError_None) {
                    s_kbShown = true;
                    SKSE::log::info("Magelight VR: runtime keyboard shown for view {}", want);
                } else {
                    // OpenComposite has no overlay keyboard to raise; on that runtime the
                    // host's own on-panel keyboard (and OCU's WM_OC_CHAR window message) carry text.
                    static bool warned = false;
                    if (!warned) {
                        warned = true;
                        SKSE::log::info("Magelight VR: runtime keyboard unavailable ({}) — relying on the runtime's own (OCU) path",
                            static_cast<int>(err));
                    }
                    s_kbView = 0;
                }
            }
        } else if (!want && s_kbShown) {
            s_overlay->HideKeyboard();
            s_kbShown = false;
        }
        if (!s_kbShown) return;
        // Drain keyboard events off the focused view's overlay.
        auto it = s_overlays.find(s_kbView);
        if (it == s_overlays.end()) { s_overlay->HideKeyboard(); s_kbShown = false; s_kbView = 0; return; }
        // BOUNDED: this runs on the present thread, and raising the
        // keyboard produced a flood of focus events in the field. An
        // unbounded drain here is a frame stall.
        vr::VREvent_t ev{};
        int drained = 0;
        while (drained++ < 64 && s_overlay->PollNextOverlayEvent(it->second.handle, &ev, sizeof(ev))) {
            if (ev.eventType == vr::VREvent_KeyboardCharInput) {
                char buf[9]{};
                std::memcpy(buf, ev.data.keyboard.cNewInput, 8);
                wchar_t wide[16]{};
                const int n = MultiByteToWideChar(CP_UTF8, 0, buf, -1, wide, 15);
                for (int i = 0; i < n && wide[i]; ++i) {
                    if (wide[i] == L'\b') {   // the runtime keyboard's backspace
                        Magelight::QueueSyntheticInput(WM_KEYDOWN, VK_BACK, 0);
                        Magelight::QueueSyntheticInput(WM_KEYUP, VK_BACK, 0);
                    } else if (wide[i] >= 0x20 || wide[i] == L'\t' || wide[i] == L'\r') {
                        Magelight::QueueSyntheticInput(WM_CHAR, static_cast<std::uintptr_t>(wide[i]), 0);
                    }
                }
            } else if (ev.eventType == vr::VREvent_KeyboardDone) {
                // Full-mode fallback: the runtime kept the text itself.
                char text[256]{};
                const std::uint32_t n = s_overlay->GetKeyboardText(text, sizeof(text));
                if (n > 0) {
                    wchar_t wide[256]{};
                    const int wn = MultiByteToWideChar(CP_UTF8, 0, text, -1, wide, 255);
                    for (int i = 0; i < wn && wide[i]; ++i)
                        if (wide[i] >= 0x20 || wide[i] == L'\t' || wide[i] == L'\r')
                            Magelight::QueueSyntheticInput(WM_CHAR, static_cast<std::uintptr_t>(wide[i]), 0);
                    SKSE::log::info("Magelight VR: runtime keyboard done — {} byte(s) of text delivered", n);
                }
                s_kbShown = false; s_kbView = 0;
                break;
            } else if (ev.eventType == vr::VREvent_KeyboardClosed) {
                s_kbShown = false;
                s_kbView = 0;
                SKSE::log::info("Magelight VR: runtime keyboard closed");
                break;
            }   // every other overlay event (focus churn, process
                // connect, chaperone) is not ours to act on
        }
    }

    void ClearLaserState()
    {
        for (int h = 0; h < 2; ++h) {
            HideBeam(h); s_hover[h] = HoverState{}; s_lastTriggerDown[h] = false; s_scrollAccum[h] = 0.0f;
            s_gripWas[h] = false; s_gripSeeded[h] = false;
            s_dragRequest.store(0); s_dragView = 0; s_dragHand = -1;
            s_hands[h].stickY_mil.store(0); s_hands[h].gripEdge.store(false);
        }
        if (s_kbShown && s_overlay) s_overlay->HideKeyboard();
        s_kbShown = false; s_kbView = 0;
    }

    // Hover + click on the panel overlays (VR-3). One pose snapshot;
    // reconstruct each panel's world from the live HMD pose (B1); per hand,
    // intersect topmost-first and inject the in-view pixel through the host's
    // synthetic-input bridge. Present thread; touches no engine state (buttons
    // already came from the sink).
    void TickLaser(const PresentedFrame& frame)
    {
        if (!s_system || !s_compositor || !s_overlay) return;
        if (!s_frameHmdUsable) return;   // snapshot taken in SubmitFrame; no head pose -> nothing to hit
        const vr::TrackedDevicePose_t* render = s_frameRender;
        const vr::TrackedDevicePose_t& hmd = render[vr::k_unTrackedDeviceIndex_Hmd];

        // B1: HeadLocked panels (runtime-relative transform) get their world
        // reconstructed = HMD * relative; lazy/world panels already own theirs.
        for (const PresentedView& pv : frame) {
            if (pv.isInspector || pv.clickThrough || !pv.ul) continue;
            auto it = s_overlays.find(pv.id);
            if (it == s_overlays.end() || !it->second.shown) continue;
            if (it->second.placement.mode == Mode::HeadLocked)
                it->second.world = MatMul(hmd.mDeviceToAbsoluteTracking, HeadRelative(it->second.placement));
        }

        for (int hand = 0; hand < 2; ++hand) {
            const auto role = (hand == 1) ? vr::TrackedControllerRole_RightHand
                                          : vr::TrackedControllerRole_LeftHand;
            const vr::TrackedDeviceIndex_t idx = s_system->GetTrackedDeviceIndexForControllerRole(role);
            if (idx == vr::k_unTrackedDeviceIndexInvalid || idx >= vr::k_unMaxTrackedDeviceCount) {  // B4
                s_lastTriggerDown[hand] = false;   // controller gone: forget its click state
                s_hover[hand].hit = false;
                HideBeam(hand);
                continue;
            }
            const vr::TrackedDevicePose_t& ctrlPose = render[idx];
            if (!PoseUsable(ctrlPose)) { s_lastTriggerDown[hand] = false; s_hover[hand].hit = false; HideBeam(hand); continue; }

            // Controller state straight from the runtime (the primitive the
            // game's own BSOpenVRControllerDevice polls): legacy mapping
            // Axis0 = stick/pad, Axis1 = trigger (0..1), Axis2 = grip on
            // Touch/Index; k_EButton_Grip for wands. The engine's ButtonEvent
            // stream for these analog axes is not a usable level source
            // (0.17.7 field: still one click per frame), so the laser no
            // longer depends on it — the sink only swallows those events.
            vr::VRControllerState_t st{};
            bool polled = false, trig = false, gripNow = false; float stickY = 0.0f;
            if (s_system->GetControllerState(idx, &st, sizeof(st))) {
                polled = true;
                trig = (st.rAxis[1].x > 0.5f) ||
                       ((st.ulButtonPressed & vr::ButtonMaskFromId(vr::k_EButton_SteamVR_Trigger)) != 0);
                gripNow = ((st.ulButtonPressed & vr::ButtonMaskFromId(vr::k_EButton_Grip)) != 0) ||
                          (st.rAxis[2].x > 0.5f);
                stickY = st.rAxis[0].y;
            } else {
                static int s_pollWarned = 0;
                if (s_pollWarned++ < 2)
                    SKSE::log::warn("Magelight VR: GetControllerState failed for hand {} (device {}) — falling back to engine levels", hand, idx);
            }
            // Aim frame: the render-model tip where the controller has one,
            // the fixed pitch otherwise. Resolved once per device.
            ResolveAim(hand, idx, st);
            const vr::HmdMatrix34_t ctrl =
                s_aim[hand].haveTip
                    ? MatMul(ctrlPose.mDeviceToAbsoluteTracking, s_aim[hand].tip)
                    : ApplyLocalPitch(ctrlPose.mDeviceToAbsoluteTracking, s_settings.aimPitchDeg);

            // Grip rising edge: bring every non-HUD panel back in front of you.
            // A grip already held when UI mode opened is NOT an edge — it is
            // the modifier the user is holding for a controller binding
            // (field 2026-09-03: opening with grip+A recentred on every open).
            {
                if (!s_gripSeeded[hand]) { s_gripWas[hand] = gripNow; s_gripSeeded[hand] = true; }
                bool edge;
                if (polled) { edge = gripNow && !s_gripWas[hand]; s_gripWas[hand] = gripNow; }
                else edge = s_hands[hand].gripEdge.exchange(false);
                if (edge) {
                    for (auto& [id, r] : s_overlays)
                        if (r.shown && r.placement.mode != Mode::HeadLocked) { r.placed = false; r.following = false; }
                    SKSE::log::info("Magelight VR: grip (hand {}) — panels recentred", hand);
                }
            }

            // M5: topmost-first (reverse z-order), first ray hit wins —
            // matches the host hitTest so the pixel re-resolves to this view.
            bool hit = false; float bestU = 0, bestV = 0, bestDist = 0;
            const PresentedView* target = nullptr;
            for (auto rit = frame.rbegin(); rit != frame.rend(); ++rit) {
                const PresentedView& pv = *rit;
                if (pv.isInspector || pv.clickThrough || !pv.ul || pv.w <= 0 || pv.h <= 0) continue;
                auto it = s_overlays.find(pv.id);
                if (it == s_overlays.end() || !it->second.shown || it->second.texW <= 0) continue;
                OverlayRec& r = it->second;
                // Clamp to the SAME width the overlay is displayed at
                // (SetOverlayWidthInMeters(ClampWidthM(...))) — the raw
                // panelWidthM is unvalidated, so an out-of-range or NaN
                // value made the click rectangle diverge from the visible
                // panel (review 2026-09-05, finding 4).
                const float widthM  = ClampWidthM(r.placement.widthMeters);
                const float heightM = widthM * static_cast<float>(r.texH) / static_cast<float>(r.texW);  // M4
                float u, v, dist;
                if (IntersectQuad(r.world, widthM, heightM, ctrl, u, v, dist)) {
                    hit = true; bestU = u; bestV = v; bestDist = dist; target = &pv;
                    break;
                }
            }
            s_hover[hand].hit = hit;
            if (hit && target) { s_hover[hand].view = target->id; s_hover[hand].u = bestU; s_hover[hand].v = bestV; }

            // ── Grab-and-move ────────────────────────────────────────────
            // A page asked to be dragged: adopt the hand that is pointing
            // at it and remember how far away it was grabbed.
            if (const ViewId req = static_cast<ViewId>(s_dragRequest.load())) {
                if (!s_dragView && hit && target && target->id == req) {
                    s_dragView = req;
                    s_dragHand = hand;
                    s_dragDist = std::clamp(bestDist, 0.35f, 6.0f);
                    s_dragRequest.store(0);
                    SKSE::log::info("Magelight VR: view {} grabbed by hand {} at {:.2f}m", req, hand, s_dragDist);
                }
            }
            if (s_dragView && s_dragHand == hand) {
                auto dit = s_overlays.find(s_dragView);
                if (dit == s_overlays.end() || !dit->second.shown) {
                    s_dragView = 0; s_dragHand = -1;
                } else {
                    OverlayRec& dr = dit->second;
                    // Hang the panel on the ray at the grab distance, then
                    // face it back at the head so it stays readable.
                    const float ox = ctrl.m[0][3], oy = ctrl.m[1][3], oz = ctrl.m[2][3];
                    const float dx = -ctrl.m[0][2], dy = -ctrl.m[1][2], dz = -ctrl.m[2][2];
                    dr.pos[0] = ox + dx * s_dragDist;
                    dr.pos[1] = oy + dy * s_dragDist;
                    dr.pos[2] = oz + dz * s_dragDist;
                    float fx = dr.pos[0] - hmd.mDeviceToAbsoluteTracking.m[0][3];
                    float fz = dr.pos[2] - hmd.mDeviceToAbsoluteTracking.m[2][3];
                    const float fn = std::sqrt(fx * fx + fz * fz);
                    if (fn > 1e-3f) { dr.fwd[0] = fx / fn; dr.fwd[1] = fz / fn; }
                    dr.placement.mode = Mode::WorldLocked;   // stays where you put it
                    dr.placed = true;
                    dr.following = false;
                    dr.world = LevelMatrix(dr.fwd, dr.pos);
                    if (s_overlay && s_compositor && dr.handle != vr::k_ulOverlayHandleInvalid)
                        s_overlay->SetOverlayTransformAbsolute(dr.handle, s_compositor->GetTrackingSpace(), &dr.world);
                }
            }
            // Only draw the beam while it is ON one of our panels — the
            // runtime's own pointer behaves the same way (it appears near
            // its keyboard and nowhere else), and a beam sweeping the world
            // while you look around is just clutter.
            if (hit) UpdateBeam(hand, ctrl, hmd.mDeviceToAbsoluteTracking, bestDist);
            else HideBeam(hand);

            HandInput& hi = s_hands[hand];
            const bool nowDown = polled ? trig : hi.triggerDown.load();   // level (polled preferred)
            if (hit && target) {
                const int px = static_cast<int>(std::lround(target->x + bestU * target->w));
                const int py = static_cast<int>(std::lround(target->y + bestV * target->h));
                // Only when it actually moved: each queued move costs a
                // hitTest under the views mutex plus a WebKit hover
                // hit-test on the present thread, and a steady hand
                // otherwise pays that 90-180 times a second for nothing.
                const bool moved = (px != s_lastPx[hand] || py != s_lastPy[hand]);
                s_lastPx[hand] = px; s_lastPy[hand] = py;
                const auto pos = static_cast<std::intptr_t>(MAKELPARAM(static_cast<WORD>(px), static_cast<WORD>(py)));
                if (moved || (nowDown != s_lastTriggerDown[hand]))
                    Magelight::QueueSyntheticInput(WM_MOUSEMOVE, nowDown ? MK_LBUTTON : 0, pos);
                if (nowDown && !s_lastTriggerDown[hand])
                    Magelight::QueueSyntheticInput(WM_LBUTTONDOWN, MK_LBUTTON, pos);
            }
            // Release at the LAST HIT pixel of this hand (a web click needs
            // the press and the release on the same element — 0.17.9 sent
            // the up at (0,0), which the host re-hit-tested to a corner far
            // outside every control). Fires whether or not the ray still
            // hits, so a drag that left the panel still releases.
            if (!nowDown && s_lastTriggerDown[hand]) {
                const auto upPos = static_cast<std::intptr_t>(MAKELPARAM(static_cast<WORD>(s_lastPx[hand]), static_cast<WORD>(s_lastPy[hand])));
                Magelight::QueueSyntheticInput(WM_LBUTTONUP, 0, upPos);
            }
            s_lastTriggerDown[hand] = nowDown;

            // Thumbstick Y -> wheel notches at the hit pixel (only while
            // pointing at a panel). Deadzone, rate-limited via an
            // accumulator, and a staleness guard so a stick that stopped
            // reporting never scrolls forever.
            {
                float y;
                if (polled) y = std::clamp(stickY, -1.0f, 1.0f);
                else {
                    int mil = hi.stickY_mil.load();
                    if (GetTickCount64() - hi.stickTick.load() > kStickStaleMs) mil = 0;
                    y = mil / 1000.0f;
                }
                if (std::fabs(y) < kStickDeadzone) { y = 0.0f; s_scrollAccum[hand] = 0.0f; }
                else y = (y > 0 ? 1.0f : -1.0f) * (std::fabs(y) - kStickDeadzone) / (1.0f - kStickDeadzone);
                if (y != 0.0f && hit && target) {
                    s_scrollAccum[hand] += y * kScrollNotchesPerSec * std::clamp(s_frameDt, 0.0f, 0.1f);
                    const int px = static_cast<int>(std::lround(target->x + bestU * target->w));
                    const int py = static_cast<int>(std::lround(target->y + bestV * target->h));
                    const auto pos = static_cast<std::intptr_t>(MAKELPARAM(static_cast<WORD>(px), static_cast<WORD>(py)));
                    while (std::fabs(s_scrollAccum[hand]) >= 1.0f) {
                        const short delta = static_cast<short>(s_scrollAccum[hand] > 0 ? WHEEL_DELTA : -WHEEL_DELTA);   // stick up = wheel up
                        Magelight::QueueSyntheticInput(WM_MOUSEWHEEL,
                            static_cast<std::uintptr_t>(MAKEWPARAM(0, static_cast<WORD>(delta))), pos);
                        s_scrollAccum[hand] += (delta > 0) ? -1.0f : 1.0f;
                    }
                }
            }
        }
    }

}  // namespace Magelight::VR