// VR presenter — runtime binding: interface resolution, state, settings, the
// thread latch, and the per-frame pose snapshot.
//
// The OpenVR interfaces are resolved at runtime from the openvr_api.dll the
// game already loaded — no import library, no /DELAYLOAD, and no version
// negotiation beyond asking for the exact strings our header's vtables
// describe (design §7: never ask for a newer one).
//
// This file and Overlays.cpp are the two that own cross-file state; everything
// else in the module is a consumer. See State.h for the rules.

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include "MagelightVR.h"
#include "State.h"

#include <algorithm>
#include <cmath>

#include <windows.h>

namespace Magelight::VR {

    std::atomic<int>  s_state{ static_cast<int>(State::Dormant) };
    bool              s_isVR = false;
    Settings          s_settings;
    DWORD             s_thread = 0;
    vr::VROverlayHandle_t s_probe = vr::k_ulOverlayHandleInvalid;

    vr::IVRSystem*     s_system = nullptr;
    vr::IVRCompositor* s_compositor = nullptr;
    vr::IVROverlay*    s_overlay = nullptr;
    vr::IVRRenderModels* s_renderModels = nullptr;
    std::uint32_t      s_initToken = 0;
    GetGenericInterfaceFn     s_getInterface = nullptr;
    IsInterfaceVersionValidFn s_isVersionValid = nullptr;
    GetInitTokenFn            s_getInitToken = nullptr;

    ID3D11Device*        s_device = nullptr;
    ID3D11DeviceContext* s_context = nullptr;

    const char* const kOverlayKeyPrefix = "severause.magelight.view.";

    // Per-frame pose snapshot, present thread only.
    vr::TrackedDevicePose_t s_frameRender[vr::k_unMaxTrackedDeviceCount];
    bool  s_frameHmdUsable = false;
    float s_frameDt = 0.0f;
    std::chrono::steady_clock::time_point s_lastFrameTp{};

    void SetState(State s) { s_state.store(static_cast<int>(s)); }

    // Resolve the three interfaces against the EXACT version strings our
    // header's vtables describe (design §7: never ask for a newer one).
    bool BindInterfaces()
    {
        const HMODULE mod = GetModuleHandleA("openvr_api.dll");
        if (!mod) {
            SKSE::log::warn("Magelight VR: openvr_api.dll is not loaded in this process — presenter dormant");
            return false;
        }
        s_getInterface   = reinterpret_cast<GetGenericInterfaceFn>(GetProcAddress(mod, "VR_GetGenericInterface"));
        s_isVersionValid = reinterpret_cast<IsInterfaceVersionValidFn>(GetProcAddress(mod, "VR_IsInterfaceVersionValid"));
        s_getInitToken   = reinterpret_cast<GetInitTokenFn>(GetProcAddress(mod, "VR_GetInitToken"));
        if (!s_getInterface) {
            SKSE::log::error("Magelight VR: VR_GetGenericInterface not exported by openvr_api.dll — presenter dormant");
            return false;
        }
        auto bind = [&](const char* version, void*& out) -> bool {
            if (s_isVersionValid && !s_isVersionValid(version)) {
                SKSE::log::error("Magelight VR: runtime does not serve {} — presenter dormant", version);
                return false;
            }
            vr::EVRInitError err = vr::VRInitError_None;
            out = s_getInterface(version, &err);
            if (!out || err != vr::VRInitError_None) {
                SKSE::log::error("Magelight VR: {} unavailable (init error {}) — presenter dormant",
                    version, static_cast<int>(err));
                return false;
            }
            return true;
        };
        void* sys = nullptr; void* comp = nullptr; void* ovl = nullptr;
        if (!bind(vr::IVRSystem_Version, sys)) return false;
        if (!bind(vr::IVRCompositor_Version, comp)) return false;
        if (!bind(vr::IVROverlay_Version, ovl)) return false;
        s_system = static_cast<vr::IVRSystem*>(sys);
        s_compositor = static_cast<vr::IVRCompositor*>(comp);
        s_overlay = static_cast<vr::IVROverlay*>(ovl);
        // Render models are OPTIONAL — only the aim transform uses them, and
        // the fixed-pitch fallback covers a runtime that does not serve them.
        //
        // OpenComposite (OpenVR re-implemented over OpenXR; OCU on Skyrim VR)
        // SERVES IVRRenderModels but stubs RenderModelHasComponent: the
        // call pops its "Hit stubbed file" dialog every time (field
        // 2026-09-05). It is recognisable by the factory exports stock
        // openvr_api.dll never has (HmdSystemFactory, VRSystem, ...), so
        // don't touch render models there at all.
        const bool openComposite = GetProcAddress(mod, "HmdSystemFactory") != nullptr;
        if (openComposite) {
            SKSE::log::info("Magelight VR: OpenComposite detected (HmdSystemFactory export) — "
                            "render-model tip aim skipped (stubbed there); aim uses the fixed pitch");
        } else {
            vr::EVRInitError rmErr = vr::VRInitError_None;
            s_renderModels = static_cast<vr::IVRRenderModels*>(
                s_getInterface(vr::IVRRenderModels_Version, &rmErr));
            if (!s_renderModels)
                SKSE::log::info("Magelight VR: {} unavailable ({}) — aim falls back to the fixed pitch",
                    vr::IVRRenderModels_Version, static_cast<int>(rmErr));
        }
        s_initToken = s_getInitToken ? s_getInitToken() : 0;
        SKSE::log::info("Magelight VR: bound {} / {} / {} (init token {}); tracking space {}",
            vr::IVRSystem_Version, vr::IVRCompositor_Version, vr::IVROverlay_Version, s_initToken,
            static_cast<int>(s_compositor->GetTrackingSpace()));
        return true;
    }

    // VR-1 probe: one overlay, created hidden, never shown, destroyed in
    // Shutdown. Its two log lines are the milestone: the runtime accepts our
    // overlay creation on this thread and releases it cleanly.
    void CreateProbeOverlay()
    {
        if (!s_overlay) return;
        const auto err = s_overlay->CreateOverlay("severause.magelight.probe", "Magelight probe", &s_probe);
        if (err != vr::VROverlayError_None) {
            SKSE::log::error("Magelight VR: probe CreateOverlay failed ({}) — view overlays would fail the same way",
                static_cast<int>(err));
            s_probe = vr::k_ulOverlayHandleInvalid;
            return;
        }
        s_overlay->SetOverlayWidthInMeters(s_probe, 0.5f);
        SKSE::log::info("Magelight VR: probe overlay created (handle {}) — hidden by design", s_probe);
    }

    // Diagnostic: which tracked devices the runtime reports right now.
    void LogTrackedDevices()
    {
        if (!s_system) return;
        int devices = 0;
        for (vr::TrackedDeviceIndex_t i = 0; i < vr::k_unMaxTrackedDeviceCount; ++i)
            if (s_system->IsTrackedDeviceConnected(i)) ++devices;
        SKSE::log::info("Magelight VR: {} tracked devices connected; hmd present={}", devices,
            s_system->IsTrackedDeviceConnected(vr::k_unTrackedDeviceIndex_Hmd));
    }

    // The device/context are set by Init (Lifecycle.cpp) before this runs.
    void InitRuntimeState()
    {
        s_isVR = REL::Module::IsVR();
        s_thread = GetCurrentThreadId();
        if (!s_isVR) { SetState(State::Dormant); return; }
        if (!s_settings.enabled) {
            SKSE::log::info("Magelight VR: disabled by Magelight.json — presenter dormant");
            SetState(State::Dormant);
            return;
        }
        SKSE::log::info("Magelight VR: Skyrim VR runtime detected (init on thread {}; submitViews={}, mirror={}, alpha={})",
            s_thread, s_settings.submitViews, s_settings.mirror, s_settings.straightAlpha ? "straight" : "premultiplied");
        if (!BindInterfaces()) { SetState(State::Failed); return; }
        SetState(State::Live);
        CreateProbeOverlay();
        LogTrackedDevices();
    }

    // PoseUsable lives here because both the snapshot (SubmitFrame) and the
    // laser consult it: Running_OutOfRange is a NORMAL, recoverable state (you
    // stepped outside the play space, a base station lost you for a moment).
    // The pose it carries is still valid — bPoseIsValid says so — and demanding
    // Running_OK meant a panel never got placed and the laser died outright for
    // the duration. Everything else (Uninitialised, calibrating,
    // fallback-rotation-only) stays out.
    bool PoseUsable(const vr::TrackedDevicePose_t& p)
    {
        if (!p.bPoseIsValid || !p.bDeviceIsConnected) return false;
        return p.eTrackingResult == vr::TrackingResult_Running_OK ||
               p.eTrackingResult == vr::TrackingResult_Running_OutOfRange;
    }

    // ── Small accessors the other files use ─────────────────────────────────

    bool MirrorEnabled() { return s_settings.mirror || GetState() != State::Live; }
    bool SuppressRuntimeLaser() { return s_settings.suppressRuntimeLaser && GetState() == State::Live; }
    bool OwnKeyboardEnabled() { return s_settings.ownKeyboard && GetState() == State::Live; }
    float CursorScale() { return std::clamp(s_settings.cursorScale, 0.004f, 0.5f); }
    bool  CursorDot()   { return s_settings.cursorDot; }

    Mode DefaultPanelMode()
    {
        return s_settings.panelFollow ? Mode::LazyFollow : Mode::WorldLocked;
    }

    bool PanelSizeForFullscreen(int& w, int& h)
    {
        if (GetState() != State::Live || !s_settings.submitViews) return false;
        if (s_settings.panelWidth <= 0 || s_settings.panelHeight <= 0) return false;
        w = s_settings.panelWidth;
        h = s_settings.panelHeight;
        return true;
    }

    void Configure(const Settings& s) { s_settings = s; }

    State GetState() { return static_cast<State>(s_state.load()); }
    bool  IsVRRuntime() { return s_isVR; }

}  // namespace Magelight::VR