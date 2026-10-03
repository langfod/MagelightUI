#pragma once
// Skyrim VR presenter — shared state, split across src/MagelightVR/*.cpp.
//
// The presenter's state was one anonymous namespace in a single TU (1735
// lines). Split naively, every file would get its own copy of s_overlays,
// s_settings and s_system — the code would still compile and would be silently
// broken. Everything declared here is state that more than one file reaches, or
// a helper with more than one caller; anything a single file uses alone stays a
// file-local `static` in an anonymous namespace.
//
// ── What the module is, and what its parts are ─────────────────────────────
//
//   Runtime.cpp    binding to openvr_api.dll, the state machine, settings,
//                  the per-frame pose snapshot. The only file that decides
//                  *whether* the presenter runs.
//   Input.cpp      engine input edges in (game thread) — buttons, thumbstick.
//                  Written here, read by Laser.cpp. Owns its state outright.
//   Bindings.cpp   controller hotkeys, chords, press-to-bind capture.
//                  Self-contained behind s_bindMutex.
//   Overlays.cpp   the overlay RECORD's lifecycle: create, place, follow,
//                  submit, release. Owns OverlayRec's invariants.
//   Laser.cpp      per-frame interaction with placed panels: ray hit-testing,
//                  beams, hover, the runtime keyboard, grab-and-move.
//   Lifecycle.cpp  Init / SubmitFrame / ReleaseView / Shutdown — the frame
//                  ORDER — plus the placement and query API.
//
// Two rules keep this correct, and both are load-bearing:
//
//  1. OverlayRec is declared once, here. Overlays.cpp owns creating,
//     placing, submitting and releasing it; Laser.cpp mutates individual
//     fields (grab-and-move writes pos/fwd/world) through s_overlays. That
//     split is deliberate — the record is shared, the *discipline* around it
//     lives with the code that owns each operation — but it means a change to
//     a field's meaning must be reflected in both files.
//  2. The pose snapshot (s_frameRender, s_frameHmdUsable, s_frameDt) is
//     written once per frame by SubmitFrame and read by the laser and the
//     follow glide. It is present-thread only; nothing else may touch it.
//
// See docs/VR_PRESENTER.md for the design this implements.

#include <d3d11.h>
#include <openvr.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>

#include "MagelightVR.h"


namespace Magelight::VR {

    // ── Runtime binding (Runtime.cpp) ───────────────────────────────────────

    using GetGenericInterfaceFn = void* (*)(const char*, vr::EVRInitError*);
    using IsInterfaceVersionValidFn = bool (*)(const char*);
    using GetInitTokenFn = std::uint32_t (*)();

    extern std::atomic<int> s_state;
    extern bool              s_isVR;          // REL::Module::IsVR(), latched in Init
    extern Settings          s_settings;      // Magelight.json "vr"
    extern DWORD             s_thread;        // the thread Init ran on (== Ultralight thread)
    extern vr::VROverlayHandle_t s_probe;     // VR-1: the hidden probe overlay

    extern vr::IVRSystem*       s_system;
    extern vr::IVRCompositor*   s_compositor;
    extern vr::IVROverlay*      s_overlay;     // the CLEAN interface (texture submission works)
    extern vr::IVRRenderModels* s_renderModels;  // optional: the controller "tip" aim frame
    extern std::uint32_t        s_initToken;
    extern GetGenericInterfaceFn     s_getInterface;
    extern IsInterfaceVersionValidFn s_isVersionValid;
    extern GetInitTokenFn            s_getInitToken;

    extern ID3D11Device*        s_device;      // not owned (the host's refs)
    extern ID3D11DeviceContext* s_context;

    extern const char* const kOverlayKeyPrefix;

    void SetState(State s);
    bool BindInterfaces();          // the rebind path in SubmitFrame calls it too
    void InitRuntimeState();        // the `enabled` gate, so Runtime.cpp owns the decision
    void CreateProbeOverlay();
    bool PoseUsable(const vr::TrackedDevicePose_t& p);   // OutOfRange still counts as usable

    // ── Per-frame pose snapshot (present thread only) ───────────────────────

    extern vr::TrackedDevicePose_t s_frameRender[vr::k_unMaxTrackedDeviceCount];
    extern bool  s_frameHmdUsable;
    extern float s_frameDt;
    extern std::chrono::steady_clock::time_point s_lastFrameTp;

    // ── Input edges (game thread writes, present thread reads) ──────────────
    // Input.cpp owns this; Laser.cpp reads it for the engine-event fallback.

    struct HandInput {
        std::atomic<bool> triggerDown{ false };
        std::atomic<int>  triggerEdges{ 0 };   // +1 per down, -1 per up, consumed by the present thread
        std::atomic<bool> gripEdge{ false };
        std::atomic<bool> menuEdge{ false };   // B/Y
        bool gripWasDown = false;              // game thread only: rising-edge latch
        bool menuWasDown = false;
        std::atomic<int>  stickY_mil{ 0 };     // -1000..1000
        std::atomic<std::uint64_t> stickTick{ 0 };   // GetTickCount64 of the last stick event (staleness guard)
    };
    extern HandInput s_hands[2];   // 0 = left, 1 = right (physical), resolved via IsLeftHandedMode at use

    // ── Overlay records — see rule 1 above ─────────────────────────────────

    struct OverlayRec {
        vr::VROverlayHandle_t handle = vr::k_ulOverlayHandleInvalid;
        ID3D11Texture2D*          tex = nullptr;   // host-owned copy target (design §2)
        ID3D11RenderTargetView*   rtv = nullptr;
        ID3D11ShaderResourceView* srv = nullptr;
        int texW = 0, texH = 0;
        // Last frame's stamped cursor marks, so a still page whose laser
        // dot moved still gets redrawn. -1 = none last frame.
        float lastMarkU[2] = { -1.0f, -1.0f }, lastMarkV[2] = { -1.0f, -1.0f };
        int   lastMarkCount = -1;
        bool  everSubmitted = false;
        bool  shown = false;
        bool  placed = false;                        // transform issued at least once
        Placement placement;
        vr::HmdMatrix34_t world{};                  // last authored pose (compositor space) — the laser hits THIS
        // LazyFollow / WorldLocked state: level panel = horizontal forward + position.
        float fwd[2] = { 0.0f, -1.0f };             // XZ unit forward the panel faces AWAY from the head along
        float pos[3] = { 0.0f, 0.0f, 0.0f };
        bool  following = false;                    // mid catch-up glide
    };
    extern std::map<ViewId, OverlayRec> s_overlays;

    // Placement overrides from the API (any thread) — applied next frame.
    extern std::mutex s_placementMutex;
    extern std::map<ViewId, Placement> s_placementOverrides;
    extern std::map<ViewId, bool>      s_recenterRequests;
    // "reset to the layer default" requests (SetViewVRPlacement(view, nullptr)),
    // drained in SubmitFrame where the layer is known (finding 6).
    extern std::map<ViewId, bool>      s_placementResetRequests;
    // A thread-safe COPY of each view's effective placement, published by
    // the present thread each frame under s_placementMutex; GetPlacement
    // reads THIS, never s_overlays (which the present thread mutates
    // lock-free) — review 2026-09-05, finding 3.
    extern std::map<ViewId, Placement> s_placementSnapshot;

    // Bumped on every (re)bind of the OpenVR interfaces. Part of each
    // overlay key so a rebind can never collide with an overlay orphaned
    // inside the runtime by a restart mid-teardown.
    extern std::uint64_t s_bindGeneration;

    // ── Placement maths (Overlays.cpp) ─────────────────────────────────────

    // 3x4 [R|t] multiply with the implicit bottom row [0 0 0 1].
    vr::HmdMatrix34_t MatMul(const vr::HmdMatrix34_t& A, const vr::HmdMatrix34_t& B);
    // Head-relative placement matrix: overlay centre `distance` metres along the
    // HMD's -Z, `heightOffset` below eye height, facing the HMD.
    vr::HmdMatrix34_t HeadRelative(const Placement& p);
    // A LEVEL panel's matrix: X = right, Y up, Z = -forward (back toward the head).
    vr::HmdMatrix34_t LevelMatrix(const float fwd[2], const float pos[3]);
    // Every overlay width goes through here. Magelight.json supplies
    // panelWidthM unvalidated, and a zero or NaN width is not a thing the
    // runtime is obliged to survive.
    float ClampWidthM(float w);

    // ── Overlay record operations (Overlays.cpp) ───────────────────────────

    void ReleaseRecLocked(OverlayRec& r);
    void ReleaseAllOverlays();                      // Shutdown
    bool EnsureTexture(OverlayRec& r, int w, int h);
    bool EnsureOverlay(ViewId id, OverlayRec& r, int layer);
    Placement DefaultPlacementForLayer(int layer);
    void ApplyPlacement(OverlayRec& r, const Placement& p);
    void TickFollow(OverlayRec& r, const vr::HmdMatrix34_t& hmd, float dt);

    bool SubmitOverlay(OverlayRec& r, const PresentedView& pv, bool uiModeOn);
    void ReportSubmitRate();                 // periodic submit-rate telemetry

    // ── Beams + hover + keyboard (Laser.cpp) ───────────────────────────────
    // Beams are session-global like the probe: a static gradient texture set
    // ONCE, never recreated while the runtime references it (recreating one it
    // still holds is a known teardown fault). Released only by Shutdown.

    void ForgetBeams();       // runtime restart: every handle is dead
    void ReleaseBeams();      // Shutdown
    void ForgetAim();         // runtime restart: drop the cached render-model tips

    // Last hit per hand (present thread): the copy pass stamps the host cursor
    // sprite into that view's texture at (u,v) next frame.
    struct HoverState { ViewId view = 0; float u = 0.0f, v = 0.0f; bool hit = false; };
    extern HoverState s_hover[2];

    // View whose text field has focus (0 = none). Set by NoteTextFocus on the
    // render thread (the JS dispatcher); TickKeyboard acts on it next frame.
    extern ViewId s_kbView;
    extern bool   s_kbShown;                        // the runtime's keyboard is up for it

    void TickLaser(const PresentedFrame& frame);
    void TickKeyboard();
    void ClearLaserState();                         // UI mode off
    void BeginViewDrag(ViewId view);
    void EndViewDrag();

    // ── Controller bindings (Bindings.cpp) ─────────────────────────────────

    struct VRBinding {
        std::uint32_t button = 0, modifier = 0, action = 0;
        std::uint8_t  hand = 0;
        HotkeyFn      cb = nullptr;   // set = call the mod instead of running `action`
        void*         user = nullptr;
    };
    extern std::mutex s_bindMutex;
    extern std::map<ViewId, VRBinding> s_bindings;
    // Global bind epoch: bumped under s_bindMutex on ANY (re)bind, unbind,
    // or capture arm/disarm. The dispatch loop re-latches EVERY view when
    // the epoch it last saw changed, so a chord still physically held when
    // capture ends — or when another view is rebound — can never fire a
    // bystander view's action (review 2026-09-05, finding 1).
    extern std::uint32_t               s_bindEpoch;   // guarded by s_bindMutex
    // Present-thread only: seed the capture masks on the first tick after
    // arming. The 0xFFFFFFFF "suppress everything currently held" seed is
    // written by the OWNER of these arrays (the tick), never by
    // SetButtonListener on the game thread — that cross-thread write raced
    // the tick's own writes (review 2026-09-05, finding 7).
    extern bool                        s_edgeSeedPending;  // guarded by s_bindMutex
    extern std::map<ViewId, bool>      s_bindWas;       // present thread: rising-edge latch per view
    extern std::map<ViewId, std::uint32_t> s_bindWasEpoch;  // present thread: bind epoch the latch last reset for
    extern std::map<ViewId, std::uint64_t> s_bindLastTick;  // present thread: PER-VIEW debounce (finding 5)
    extern std::uint64_t               s_captureLastTick;  // present thread: the capture branch's own debounce

    // Press-to-bind listener (guarded by s_bindMutex). While set, the
    // hotkey tick reports raw button edges here INSTEAD of dispatching
    // bindings — the press being captured must not also open a menu.
    extern ButtonEdgeFn  s_edgeCb;
    extern void*         s_edgeUser;
    extern std::uint32_t s_edgeWasMask[2];   // per hand: known buttons held last frame
    // A modifier-class button (Grip, Trigger) pressed ALONE is not
    // reported at once: it may be the HOLD half of a chord. It sits here
    // until a second button on the same hand fires (→ reported as that
    // button with this one held) or it is released with nothing else
    // pressed (→ reported bare). Field 2026-09-05: without this, pressing
    // Grip to start "Grip + A" captured a bare Grip immediately.
    extern std::uint32_t s_edgePending[2];
    // The chord vocabulary, in the order a chord's MODIFIER is reported
    // when more than one other button is held (Grip and Trigger are what
    // people naturally hold while pressing a face button).
    inline constexpr std::uint32_t kEdgeCodes[] = { 2, 33, 1, 7, 32, 35 };

    void TickVRHotkeys();                           // present thread; polled with UI mode OFF too

}  // namespace Magelight::VR