#pragma once
// Magelight UI — Skyrim VR presenter (docs/VR_PRESENTER.md).
//
// A second consumer of the textures the host already renders: one OpenVR
// overlay per visible view, laser-pointer input synthesised into the host's
// existing input queue. Runtime-agnostic by construction (SteamVR native and
// OpenComposite): poses and a quad are the only things asked of the runtime;
// intersection, click and scroll are ours.
//
// Threads: every function here that talks to OpenVR runs on the Ultralight/
// present thread from FrameWork (Init, SubmitFrame, Shutdown). Button edges
// come from the game thread (the input sink) through NoteButton — atomics,
// no lock. Nothing here touches engine state: control toggles, UI-mode
// changes and haptics are queued to the game thread by the callers.
//
// Failure posture: any refusal (no runtime, wrong interface version, overlay
// creation failed) leaves the presenter Dormant; the flat host is untouched
// and QueryCapability("vr") stays 0. s_renderDead is never set from here.

#include <cstdint>

#include "MagelightPresenter.h"

struct ID3D11Device;
struct ID3D11DeviceContext;

namespace Magelight::VR {

    enum class State : int {
        Dormant = 0,     // not a VR runtime, disabled, or init refused
        Pending = 1,     // VR runtime; interfaces not bound yet (first frames)
        Live = 2,        // overlays can be created; QueryCapability("vr") == 1
        Failed = 3,      // bound once, then a hard refusal (token change re-tries)
    };

    // Placement per view (host defaults + the v4 SetViewVRPlacement override).
    // HeadLocked: glued to the head (runtime-relative transform) — HUD widgets.
    // LazyFollow: placed level in front of the head; stays put until the gaze
    //   drifts past ~30 deg or the head moves ~0.5 m, then glides back (default
    //   for panels/popups). WorldLocked: placed once, never follows.
    enum class Mode : std::uint8_t { HeadLocked = 0, WorldLocked = 1, HandLocked = 2, LazyFollow = 3 };
    struct Placement {
        Mode  mode = Mode::LazyFollow;
        float distanceMeters = 1.6f;
        float widthMeters = 1.4f;
        float heightOffset = -0.15f;
        float curvature = 0.0f;         // unused on IVROverlay_016; kept for the API shape
        float autoCloseMeters = 3.0f;   // WorldLocked only; 0 = never
    };

    // Magelight.json "vr" object. Read at settings load (before Init).
    struct Settings {
        bool enabled = true;       // false = presenter never initialises
        bool submitViews = true;   // false = probe-only (the VR-1 behaviour) — a diagnostic opt-out
        bool mirror = true;        // keep drawing the flat overlay on the desktop mirror
        bool straightAlpha = true; // "alpha": "straight" (default) | "premultiplied" — the copy pass's PS
        bool beam = true;          // draw our own laser beam
        float beamAlpha = 0.55f;   // 0..1
        float cursorScale = 0.012f; // pointer height as a fraction of the view's height (0.26.12: a point,
                                    // not a sprite — 0.05 drew a 32px arrow at the laser's end, distracting in a headset)
        bool  cursorDot = true;     // true = a small disc at the laser's end (0.26.12); false = the arrow art
        // Fullscreen views normally track the game's backbuffer. In a headset
        // that backbuffer is the DESKTOP MIRROR window, whose aspect has
        // nothing to do with the panel the user is actually looking at —
        // Skyrim VR's is very nearly SQUARE (1261x1277 in the field), so a
        // page laid out for a wide desktop rendered into a square and every
        // column was crushed (field 2026-09-03, the SeverActions pages). VR
        // fullscreen views get this explicit panel resolution instead, and
        // the overlay quad takes its aspect from it.
        int panelWidth  = 1600;
        int panelHeight = 900;
        // The PANEL layer's placement (layer 1 — where every full page lives,
        // SeverActions' included). Tunable because comfortable distance and
        // size are matters of taste and headset FOV, and re-tuning them should
        // not need a rebuild. 1.6m away / 1.4m wide read as small and far in
        // the field; 1.2m / 1.6m subtends ~67 deg, which fills usefully more
        // of the view and puts targets within easier pointing reach.
        float panelDistanceM     = 1.2f;
        float panelWidthM        = 1.6f;
        float panelHeightOffsetM = -0.10f;
        // Lazy follow is OFF by default (field 2026-09-03: "just too
        // distracting"). A panel is placed once, in front of wherever you
        // were looking when it opened, and then STAYS there while you look
        // around — the grip moves it, deliberately and on request, which is
        // the only motion most people want. Set true to bring the glide
        // back; the follow* knobs below only matter then.
        bool  panelFollow = false;
        // Lazy follow: how far the gaze may drift before the panel glides
        // back. Measured from the panel's EDGE, not its centre — the panel's
        // own half-angle is added in, so a big page keeps a real deadzone
        // instead of chasing the head the moment you look at its far column.
        // A bare 30 deg from CENTRE was narrower than the panel itself, which
        // is why it felt twitchy; from the edge, 45 deg of margin plus a
        // ~34 deg half-angle lands near 80, and followMinDeg holds that as a
        // hard floor however the panel is resized. Turning your head should
        // be free; the grip recentres instantly when you do want it moved.
        float followAngleDeg = 45.0f;   // margin BEYOND the panel's own edge
        float followMinDeg   = 80.0f;   // absolute floor, whatever the geometry says
        float followDistM    = 0.50f;   // head travel that starts a glide
        // OpenComposite draws its OWN menu laser whenever its SKSE plugin sees
        // an engine menu open — including the focus menu UI mode pushes — so a
        // second beam appears over our panels. Its laser targets OCU's own menu
        // quad and knows nothing about our overlays, so it can never point at
        // our pages: deferring to it is not possible, but suppressing it is.
        // OCU reads a window property (OpenCompositeInput sets
        // SetPropW(hwnd, "OC_MENU_ACTIVE")); clearing it while our page is
        // focused makes OCU drop its beam. Our own control suspension already
        // covers what that flag otherwise gates for OCU.
        bool suppressRuntimeLaser = true;
        // Aim: prefer the controller's own render-model "tip" component, whose
        // local frame OpenVR defines with -Z out of the surface — the exact
        // ray convention we use, and per-controller correct (Touch, Index and
        // wands all differ). aimPitchDeg is the FALLBACK for a runtime or
        // controller with no tip component. "raw" = aimUseTip false + 0 deg.
        bool  aimUseTip = true;
        float aimPitchDeg = -35.0f;
        // The RUNTIME's virtual keyboard, off by default. Asking SteamVR for
        // one from a SCENE application (which Skyrim is) makes the compositor
        // move INPUT FOCUS away from the game: field 2026-09-03 logged
        // VREvent_InputFocusReleased + InputFocusChanged the instant it was
        // raised, the frame rate collapsed to single digits for as long as it
        // was up, and no keyboard was ever visible. Typing already works
        // without it — the input sink reads the engine keyboard device, which
        // also catches runtime keyboards that synthesise scancodes
        // (OpenComposite's PC mode). Set true only to experiment.
        bool runtimeKeyboard = false;
        // OUR OWN keyboard: a Magelight page the laser clicks, shown when a
        // text field takes focus. Runtime-independent by construction, which
        // is the whole point — SteamVR's steals input focus and OpenComposite's
        // VR mode delivers to a module it resolves by name.
        bool ownKeyboard = true;
    };
    void Configure(const Settings& s);
    bool MirrorEnabled();          // false only when live AND mirror == false
    bool SuppressRuntimeLaser();   // live && the setting is on
    bool OwnKeyboardEnabled();     // live && the setting is on
    float CursorScale();
    bool  CursorDot();
    /// The placement mode a non-HUD surface gets by default — WorldLocked
    /// unless vr.panelFollow asked for the glide back.
    Mode DefaultPanelMode();
    // True when a FULLSCREEN view should take the VR panel resolution instead
    // of the game's backbuffer (live and submitting). Fills w/h when true.
    bool PanelSizeForFullscreen(int& w, int& h);

    // ── Lifecycle (Ultralight thread) ───────────────────────────────────────
    // Called once from EnsureRenderInit's success path. Cheap when not VR.
    // VR-1: binds the interfaces and creates ONE hidden probe overlay so the
    // log proves create/destroy works on this runtime before any view is
    // ever submitted.
    void Init(ID3D11Device* device, ID3D11DeviceContext* context);
    // Called from FrameWork after the flat composite each frame. Creates /
    // updates / hides overlays for the frame's views; runs the laser and
    // queues input. Never called when the host is render-dead.
    void SubmitFrame(const PresentedFrame& frame, bool uiModeOn, ViewId uiModeView);
    // Called from ApplyPendingLifecycle BEFORE a View RefPtr drops and from
    // HibernateIdleViews BEFORE a texture is released: destroys the overlay
    // and the host copy texture for that view. Idempotent.
    // hibernating=true (HibernateIdleViews): the overlay and the placement
    // snapshot go, the view RECORD stays — so its controller binding stays
    // too. 0.28.4: the hibernate path shared the destroy path and erased the
    // binding with everything else, so a popup bound to a chord (SA's quick
    // wheel, hibernate budget 30 s) went deaf 30 s after it was last hidden
    // and only came back on a rebind (field 2026-09-08, a tester's log: every
    // failed press sat after a "hibernated" line, every working press after
    // a fresh bind). Only the destroy path may unbind.
    void ReleaseView(ViewId view, bool hibernating = false);
    // Tear everything down (render death, plugin unload).
    void Shutdown();

    // ── Queries (any thread) ────────────────────────────────────────────────
    State GetState();
    inline bool IsLive() { return GetState() == State::Live; }
    bool IsVRRuntime();   // REL::Module::IsVR() — decided at load, never changes

    // ── VR controller hotkeys (VR-4) ────────────────────────────────────────
    // Bind a view to a controller button, optionally requiring a second button
    // held as a modifier. Polled from the runtime every frame REGARDLESS of UI
    // mode (that is the point: it opens a view while the game owns input), then
    // marshalled to the game thread, where the same gates the keyboard hotkeys
    // use apply (no engine menu, no console, no text entry; while a page holds
    // focus only ITS OWN binding fires, to close). A bare button with no
    // modifier fires during gameplay too — mods should prefer a modifier.
    // button/modifier are OpenVR EVRButtonId values (see kVRButton* below);
    // hand: 0 = either, 1 = left, 2 = right. action = kHotkeyAction* from the
    // public API. button == 0 clears the view's binding.
    inline constexpr std::uint32_t kVRButtonNone       = 0;
    inline constexpr std::uint32_t kVRButtonMenu       = 1;    // B / Y (ApplicationMenu)
    inline constexpr std::uint32_t kVRButtonGrip       = 2;
    inline constexpr std::uint32_t kVRButtonA          = 7;    // A / X
    inline constexpr std::uint32_t kVRButtonStickClick = 32;   // Axis0
    inline constexpr std::uint32_t kVRButtonTrigger    = 33;   // Axis1
    inline constexpr std::uint32_t kVRButtonTouchpad   = 35;   // Axis3
    using HotkeyFn = void (*)(ViewId view, void* user);
    // Raw button-edge listener for press-to-bind UIs; suppresses bindings
    // while set. nullptr clears.
    using ButtonEdgeFn = void (*)(std::uint32_t button, std::uint32_t heldOther,
                                  std::uint8_t hand, void* user);
    void SetButtonListener(ButtonEdgeFn cb, void* user);
    void BindHotkeyCallback(ViewId view, std::uint32_t button, std::uint32_t modifier,
                            std::uint8_t hand, HotkeyFn cb, void* user);
    void BindHotkey(ViewId view, std::uint32_t button, std::uint32_t modifier,
                    std::uint8_t hand, std::uint32_t action);
    void UnbindView(ViewId view);   // drop bindings + any laser/keyboard state for a destroyed view
    // Does this view's mod bind `openvrButton` (vr::EVRButtonId) as its own
    // VR hotkey, with or without a modifier? The input sink asks before running
    // the built-in B/Y exit: a mod that bound B/Y owns open/close on that
    // button, and the built-in exit racing its callback made SA's menu
    // un-dismissable (close, then the chord re-opened it 12 ms later — VR
    // field log 2026-09-12 14:20).
    bool ViewBindsButton(ViewId view, std::uint32_t openvrButton);
    inline constexpr std::uint32_t kButtonApplicationMenu = 1;   // vr::k_EButton_ApplicationMenu — B (right) / Y (left)

    // ── Virtual keyboard (VR-4) ─────────────────────────────────────────────
    // The page bridge reports text-field focus (the reserved '__textfocus'
    // channel); on the SteamVR runtime that raises the runtime's own keyboard
    // over the focused view's overlay and its characters are queued into the
    // host's ordinary key path. OpenComposite has no keyboard of its own that
    // we can raise — there the host's own on-panel keyboard (and OCU's
    // WM_OC_CHAR window message) carry text instead, so this is a no-op there.
    void NoteTextFocus(ViewId view, bool focused);

    // ── Placement (any thread; applied next frame) ──────────────────────────
    void SetPlacement(ViewId view, const Placement& p);
    void ClearPlacement(ViewId view);   // SetViewVRPlacement(view, nullptr): restore the layer default
    bool GetPlacement(ViewId view, Placement& out);
    void Recenter(ViewId view);   // re-place in front of the HMD now (WorldLocked re-anchors)

    // Grab-and-move: while a drag is active the view rides the ray of whichever
    // hand is pointing at it, at the distance it was grabbed from, facing the
    // player. On release it STAYS there (WorldLocked) — grip recentres it.
    void BeginViewDrag(ViewId view);
    void EndViewDrag();

    // ── Button LEVELS from the game's input sink (game thread) ──────────────
    // device: RE::INPUT_DEVICE value (kVivePrimary..kWMRSecondary); code:
    // BSOpenVRControllerDevice::Key; value: ButtonEvent::Value() — the ANALOG
    // level (trigger/grip are axes on Touch/Index). The engine emits a fresh
    // "just pressed" event every frame an axis value changes, so IsDown() is
    // useless as a click edge (field 2026-09-03: each trigger pull became a
    // burst of down/up pairs — toggles flipped back to where they started,
    // sliders could not drag, grip recentred every frame). Levels are
    // thresholded here and edges derived once per transition.
    void NoteButton(int device, std::uint32_t code, float value);
    // Thumbstick deflection (game thread), -1..1 each axis, per hand.
    void NoteThumbstick(int device, float x, float y);

    // Diagnostics for the log / status page.
    const char* StateName(State s);

}  // namespace Magelight::VR
