// VR presenter — lifecycle (Init / SubmitFrame / ReleaseView / Shutdown) and
// the placement/query API.
//
// SubmitFrame is the presenter's whole per-frame contract in one place, and it
// is deliberately the only place that decides WHEN the other subsystems run:
// rebind, snapshot, place, submit, then tick the hotkeys and (when UI mode is
// on) the laser and the keyboard. That ordering is why this file is cohesive;
// the work it calls lives in the others.
//
// The placement/query API at the bottom is here rather than in its own file
// because every one of those functions is a two-line accessor over state this
// file's SubmitFrame publishes — separating them would mean a file whose whole
// content is "take a lock, read a map".

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include "MagelightVR.h"
#include "MagelightPresenter.h" // PresentedFrame, PresentedView
#include "State.h"

#include <algorithm>
#include <cmath>

#include <windows.h>

namespace Magelight::VR {

    void Init(ID3D11Device* device, ID3D11DeviceContext* context)
    {
        s_device = device;
        s_context = context;
        InitRuntimeState();
    }

    void SubmitFrame(const PresentedFrame& frame, bool uiModeOn, ViewId /*uiModeView*/)
    {
        // Frame time for the follow glide + scroll rate (present thread).
        {
            const auto now = std::chrono::steady_clock::now();
            s_frameDt = (s_lastFrameTp.time_since_epoch().count() == 0) ? 0.0f
                      : std::chrono::duration<float>(now - s_lastFrameTp).count();
            s_lastFrameTp = now;
        }
        // The runtime-restart check has to run BEFORE the Live gate, or a
        // single transient bind failure is terminal for the session: the only
        // exit from Failed is the rebind below, and VR::Init runs once per
        // process. Checked first, a restart recovers us either way.
        if (s_getInitToken && s_getInitToken() != s_initToken && GetState() != State::Live) {
            SKSE::log::warn("Magelight VR: OpenVR init token changed while not live — rebinding");
            {
                std::lock_guard<std::mutex> lk(s_placementMutex);
                for (auto& [id, r] : s_overlays) { ReleaseRecLocked(r); r.handle = vr::k_ulOverlayHandleInvalid; }
            }
            ForgetBeams();
            ForgetAim();
            ++s_bindGeneration;
            SetState(BindInterfaces() ? State::Live : State::Failed);
            return;
        }
        if (GetState() != State::Live) return;
        if (GetCurrentThreadId() != s_thread) {   // invariant 1, mirrored — and a VR-1 telemetry line
            static bool warned = false;
            if (!warned) {
                warned = true;
                SKSE::log::warn("Magelight VR: SubmitFrame on thread {} (init thread {}) — skipping VR work on this thread",
                    GetCurrentThreadId(), s_thread);
            }
            return;
        }
        if (!s_settings.submitViews) return;     // VR-1: probe only; VR-2 flips this on

        // Runtime restart: every handle is dead. Rebind, next frame recreates.
        if (s_getInitToken && s_getInitToken() != s_initToken) {
            SKSE::log::warn("Magelight VR: OpenVR init token changed — rebinding");
            // ReleaseRecLocked guards every runtime teardown call on
            // `handle != invalid`, so nulling FIRST skipped HideOverlay /
            // ClearOverlayTexture / DestroyOverlay entirely and orphaned the
            // overlay inside the runtime under its stable key. The next
            // CreateOverlay for that view then failed KeyInUse forever.
            for (auto& [id, r] : s_overlays) { ReleaseRecLocked(r); r.handle = vr::k_ulOverlayHandleInvalid; }
            ForgetBeams();
            ForgetAim();
            ++s_bindGeneration;
            SetState(BindInterfaces() ? State::Live : State::Failed);
            return;
        }

        // One pose snapshot per frame (M1): the compositor's RENDER poses in
        // the space the overlays are composited in. Shared by the follow glide
        // and the laser.
        s_frameHmdUsable = false;
        {
            vr::TrackedDevicePose_t game[vr::k_unMaxTrackedDeviceCount];
            if (s_compositor && s_compositor->GetLastPoses(s_frameRender, vr::k_unMaxTrackedDeviceCount,
                                                           game, vr::k_unMaxTrackedDeviceCount) == vr::VRCompositorError_None)
                s_frameHmdUsable = PoseUsable(s_frameRender[vr::k_unTrackedDeviceIndex_Hmd]);
        }

        // Recenter requests (placement overrides are merged per view below,
        // AFTER the layer default is seeded — applying them here would create
        // the registry entry early and lose that default).
        {
            std::lock_guard<std::mutex> lk(s_placementMutex);
            for (auto& [id, _] : s_recenterRequests) {
                auto it = s_overlays.find(id);
                if (it != s_overlays.end()) { it->second.placed = false; it->second.following = false; }
            }
            s_recenterRequests.clear();
        }

        // Present every visible, non-inspector view; hide the rest.
        for (const PresentedView& pv : frame) {
            if (pv.isInspector || !pv.ul || !pv.srv || pv.w <= 0 || pv.h <= 0) continue;
            const bool fresh = s_overlays.find(pv.id) == s_overlays.end();
            OverlayRec& r = s_overlays[pv.id];
            if (fresh) r.placement = DefaultPlacementForLayer(pv.layer);
            // A queued override MERGES onto that default: 0 means "keep the
            // host's value for this layer" (a manifest that only names a mode
            // must not drag a HUD badge to the panel distance — field
            // 2026-09-03: the badge came out 1.40m at 1.60m instead of
            // 0.45m at 1.50m).
            {
                std::lock_guard<std::mutex> lk(s_placementMutex);
                if (s_placementResetRequests.erase(pv.id) > 0) {
                    // SetViewVRPlacement(view, nullptr): restore the LAYER
                    // default rather than storing a concrete default that
                    // clobbers it (review 2026-09-05, finding 6).
                    r.placement = DefaultPlacementForLayer(pv.layer);
                    r.placed = false; r.following = false;
                    s_placementOverrides.erase(pv.id);
                    if (r.handle != vr::k_ulOverlayHandleInvalid)
                        s_overlay->SetOverlayWidthInMeters(r.handle, ClampWidthM(r.placement.widthMeters));
                }
                else if (auto ov = s_placementOverrides.find(pv.id); ov != s_placementOverrides.end()) {
                    const Placement& p = ov->second;
                    r.placement.mode = p.mode;
                    if (p.distanceMeters > 0.0f) r.placement.distanceMeters = p.distanceMeters;
                    if (p.widthMeters > 0.0f)    r.placement.widthMeters    = p.widthMeters;
                    r.placement.heightOffset = p.heightOffset;
                    r.placed = false; r.following = false;   // re-place with the new values
                    s_placementOverrides.erase(ov);
                    if (r.handle != vr::k_ulOverlayHandleInvalid)
                        s_overlay->SetOverlayWidthInMeters(r.handle, ClampWidthM(r.placement.widthMeters));
                }
                s_placementSnapshot[pv.id] = r.placement;   // finding 3: publish under the lock
            }
            if (!EnsureOverlay(pv.id, r, pv.layer)) continue;
            if (r.placement.mode == Mode::HeadLocked) {
                // Glued: world = HMD * relative, pushed as an ABSOLUTE transform
                // every frame the head pose is usable. OpenComposite put the
                // TrackedDeviceRelative badge on the floor (field 2026-09-03);
                // the absolute path is what both runtimes composite correctly,
                // and it is the same matrix the laser hits. One frame of lag on
                // a HUD widget is accepted. Runtime-relative remains the
                // first-placement fallback while no head pose is available.
                if (s_frameHmdUsable && s_overlay && s_compositor) {
                    r.world = MatMul(s_frameRender[vr::k_unTrackedDeviceIndex_Hmd].mDeviceToAbsoluteTracking,
                                     HeadRelative(r.placement));
                    s_overlay->SetOverlayTransformAbsolute(r.handle, s_compositor->GetTrackingSpace(), &r.world);
                    r.placed = true;
                } else if (!r.placed) ApplyPlacement(r, r.placement);
            } else if (s_frameHmdUsable) {
                TickFollow(r, s_frameRender[vr::k_unTrackedDeviceIndex_Hmd].mDeviceToAbsoluteTracking, s_frameDt);
            }
            // A panel whose transform was never authored sits at the
            // tracking-space ORIGIN — on the floor, usually behind the player,
            // while the desktop mirror renders it perfectly. That is the
            // "open on the monitor, invisible in the headset" report. It
            // happens whenever the head pose was unusable on the frame the
            // overlay was created, because TickFollow is the only thing that
            // writes r.world for a non-HeadLocked view. Wait for a pose.
            if (r.placement.mode != Mode::HeadLocked && !r.placed) continue;
            if (!EnsureTexture(r, pv.w, pv.h)) continue;
            SubmitOverlay(r, pv, uiModeOn);
        }
        ReportSubmitRate();

        for (auto& [id, r] : s_overlays) {
            bool present = false;
            for (const PresentedView& pv : frame) if (pv.id == id && !pv.isInspector && pv.ul) { present = true; break; }
            if (!present && r.shown && r.handle != vr::k_ulOverlayHandleInvalid) {
                s_overlay->HideOverlay(r.handle);
                r.shown = false;
                // Forget the anchor so the next SHOW re-places the panel in
                // front of wherever the head is looking THEN. Without this a
                // WorldLocked panel keeps the transform it was first given
                // for the whole session: field 2026-09-03 opened the UI once
                // on load, played on for three minutes, and every later open
                // was invisible in the headset while the desktop mirror
                // showed it perfectly — the quad was still sitting where the
                // player had been standing when they first opened it. Lazy
                // follow used to hide this by continuously re-aiming, so it
                // only surfaced once follow became opt-in (0.22.0). Placed
                // and STAYS placed is the contract, but the placement is per
                // opening, not per session.
                r.placed = false;
                r.following = false;
                SKSE::log::info("Magelight VR: overlay hidden for view {}", id);
            }
        }

        TickVRHotkeys();          // polled with UI mode OFF too — that is how a view opens
        if (uiModeOn) { TickLaser(frame); TickKeyboard(); }
        else ClearLaserState();   // beams off, hover forgotten, click levels reset
    }

    void ReleaseView(ViewId view, bool hibernating)
    {
        if (!hibernating) UnbindView(view);   // a hibernated view keeps its chord (see the header)
        {
            std::lock_guard<std::mutex> lk(s_placementMutex);
            s_placementSnapshot.erase(view);
            s_placementResetRequests.erase(view);
        }
        auto it = s_overlays.find(view);
        if (it == s_overlays.end()) return;
        ReleaseRecLocked(it->second);
        s_overlays.erase(it);
        SKSE::log::info("Magelight VR: overlay released for view {}", view);
    }

    void Shutdown()
    {
        ReleaseAllOverlays();
        ReleaseBeams();
        if (s_overlay && s_probe != vr::k_ulOverlayHandleInvalid) {
            const auto err = s_overlay->DestroyOverlay(s_probe);
            SKSE::log::info("Magelight VR: probe overlay destroyed ({})", static_cast<int>(err));
            s_probe = vr::k_ulOverlayHandleInvalid;
        }
        s_system = nullptr; s_compositor = nullptr; s_overlay = nullptr;
        if (GetState() == State::Live) SetState(State::Dormant);
    }

    // ── Placement / query API (any thread unless noted) ─────────────────────

    void SetPlacement(ViewId view, const Placement& p)
    {
        std::lock_guard<std::mutex> lk(s_placementMutex);
        s_placementOverrides[view] = p;
    }

    bool GetPlacement(ViewId view, Placement& out)
    {
        // Read the snapshot the present thread publishes each frame under this
        // lock — never a lock-free s_overlays.find() from the game thread while
        // the present thread inserts/erases that map (review 2026-09-05, finding 3).
        std::lock_guard<std::mutex> lk(s_placementMutex);
        auto it = s_placementSnapshot.find(view);
        if (it == s_placementSnapshot.end()) return false;
        out = it->second;
        return true;
    }

    void ClearPlacement(ViewId view)
    {
        std::lock_guard<std::mutex> lk(s_placementMutex);
        s_placementOverrides.erase(view);
        s_placementResetRequests[view] = true;
    }

    void Recenter(ViewId view)
    {
        std::lock_guard<std::mutex> lk(s_placementMutex);
        s_recenterRequests[view] = true;
    }

    const char* StateName(State s)
    {
        switch (s) {
        case State::Dormant: return "dormant";
        case State::Pending: return "pending";
        case State::Live:    return "live";
        case State::Failed:  return "failed";
        }
        return "?";
    }

}  // namespace Magelight::VR