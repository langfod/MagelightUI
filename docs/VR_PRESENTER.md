# Magelight UI — Skyrim VR presenter (design)

Status: IMPLEMENTED. The VR presenter shipped in 0.18.0 and has field-passed
on both SteamVR-native and OpenComposite runtimes (see the summary at the end
of this document). This is the design and the reasoning behind it; where the
text reads in the future tense it predates the implementation, and the
**(verify)** markers were the open questions the first field pass resolved.

The flat host is unchanged by this work until milestone VR-2 lands; the VR
presenter is an additional CONSUMER of the textures the host already renders.

---

## 1. Goal and posture

- **Full Skyrim VR (1.4.15) compatibility, SteamVR-native AND OpenComposite.**
  The prior in-game UI host's VR build (Alpha 8, June 2026 — the copy on the
  VR test instance) logs `VR laser input requires OpenComposite Unleashed` and
  drives its keyboard through OCU's `WM_OC_CHAR`; SteamVR-native users get
  panels with no pointer. That gap is the reason this exists. Our laser, click and scroll
  must depend on NOTHING the runtime does for us beyond poses and a quad.
- **First target: an in-world quad per visible view** (one OpenVR overlay per
  view, laser pointer, trigger = click, stick = scroll). No 3D UI, no curved
  HUD wrap, no VRIK-hand attachment in v1.
- **Flat fallback is total**: if the VR runtime, the overlay interface or the
  present-thread rule fails, the host behaves exactly as on flatscreen (the
  desktop mirror shows the overlay; the headset shows nothing of ours) and
  `QueryCapability("vr")` stays 0. A VR failure must never disable the
  overlay for the session (`s_renderDead` is for render faults only).
- **Universal DLL.** One `Magelight.dll` for SE/AE/VR: the NG port already
  builds with `ENABLE_SKYRIM_VR=1` (exported by `CommonLibSSE-targets.cmake`,
  verified in `C:\b\mgl`), and SA ships the same way. No openvr import
  library, no `/DELAYLOAD`: every OpenVR call goes through a vtable obtained
  from `VR_GetGenericInterface` resolved with `GetProcAddress` on the
  `openvr_api.dll` the GAME already loaded (the `RE::BSOpenVR::GetCleanIVROverlay`
  pattern). Today's `Magelight.dll` has no openvr dependency at all
  (`dumpbin /DEPENDENTS`, 2026-09-03) and that stays true.

## 2. Where the pixels come from

The GPU driver already renders every view into its own D3D11 texture
(`MgGpu_GetTextureSRV(rt.texture_id)`, padded, addressed by `rt.uv_coords`).
The flat compositor samples that SRV onto the backbuffer (`DrawOverlay`); the
VR presenter is a second consumer of the same SRV. It does NOT hand the
driver's texture to the runtime:

- **Per-view host-owned overlay texture** (`ID3D11Texture2D` + RTV + SRV,
  BGRA8 UNORM, no mips, exactly `view.w × view.h`). Each frame the presenter
  DRAWS the view's SRV into it with the existing quad pipeline (a second
  `DrawOverlay`-class pass targeting our RTV instead of the backbuffer):
  the pass resolves the uv padding, applies the cutout (same PS constant),
  and converts premultiplied to straight alpha (a one-line PS variant —
  `VROverlayFlags_IsPremultiplied` is not in the header we compile against
  (IVROverlay_016) and OpenComposite's handling of it is unknown, so we do
  not rely on the runtime for it). **(verify)** which alpha convention each
  runtime actually composites overlays with; the PS variant is a switch.
- **Lifetime rule (guards the nvwgf2umx CTD):** the runtime is only
  ever handed a texture WE own, and that texture outlives its overlay: the
  overlay is destroyed (`DestroyOverlay`, then the host texture released) in
  `ApplyPendingLifecycle` BEFORE the View RefPtr drops, and in
  `HibernateIdleViews` before the texture is released. `SetOverlayTexture`
  is called only from `FrameWork` on the Ultralight thread for a view whose
  `ul` is live THIS frame (snapshot the RefPtr for the duration of the
  submit). A view that is not visible gets `HideOverlay`, never a stale
  submit.
- **Mirror stays on.** The flat compositor keeps drawing on the desktop
  swapchain in VR (spectator/streaming view; also the only thing a SteamVR
  user sees if overlays fail). `Magelight.json` `vr.mirror: false` turns it
  off.
- CPU-surface path: `v.tex` is already host-owned; submit it directly (no
  copy pass needed, alpha is premultiplied there too — same PS switch).

## 3. Presenter seam

`src/MagelightPresenter.h` names the seam the flat compositor and the VR
submitter share. `FrameWork` builds one `PresentedView` list per frame
(id, SRV, uv rect, size, cutout, layer, visibility, `ul` snapshot) and
hands it to:

1. the **flat compositor** (today's inline loop, unchanged behaviour), and
2. the **VR submitter** (`Magelight::VR::SubmitFrame`) when
   `VR::IsLive()`.

The seam is internal (not ABI); it exists so the two consumers cannot drift
in how they read the registry, and so a later `IPresentSource`/`ICompositor`
split has a name to grow from.

## 4. Threads

- **All OpenVR calls run on the Ultralight/present thread from `FrameWork`**
  (overlay create/destroy/texture/transform/show/hide, pose reads). Same
  thread every frame, after `Render()` and the driver's command list, inside
  the existing `StateBackup` bracket (our copy pass rebinds RTs).
  Invariant 1 holds: nothing VR touches Ultralight from another thread.
  An earlier design note claimed "SteamVR wants overlay calls on the input
  thread, OpenComposite on the render thread" — not from Valve's docs (which say overlay calls are
  thread-agnostic, apart from `SetOverlayTexture` needing the creating
  process). We start on the present thread for both runtimes and move only
  if a runtime demonstrably objects.
- **Every engine-state toggle the laser triggers goes through the game
  thread** (`GameTask::Post`, `MagelightGameTask.h`): UI-mode entry/exit,
  `ToggleControls`, text-entry flag, haptics. This forecloses the
  panel-freeze bug (render-thread `ToggleControls` racing the game thread's
  input processing) and our own invariant 2. The VR submitter only ever
  QUEUES input (`QueueInput`, the existing `InputMsg` queue) and the
  existing paths do the rest.
- **Button edges arrive on the game thread** (the input sink, section 6) and
  are recorded into atomics the present thread reads. No new mutex.
- Present-thread identity under `SkyrimVR.exe` is **(verify)** item #1 —
  invariant 1 was proven on SE; VR has a different frame loop (WaitGetPoses
  in the game's own `BSOpenVR` before the mirror Present). If the mirror
  swapchain's `Present` does not fire from the main thread every frame, the
  alternative hook point is `BSOpenVR::PostPresentHandoff` /
  `Submit` (NG exposes both as vfuncs 02/03) — same thread question, and
  the design above does not change, only where `FrameWork` is called from.

## 5. Placement model

Per view, a `VRPlacement` (host default + per-view API override):

| Field | Default | Meaning |
|---|---|---|
| `mode` | `HeadLocked` | `HeadLocked`: transform relative to the HMD each frame with a lazy follow (re-centres only when the panel leaves a yaw/pitch window — no nausea, no "glued to the face"); `WorldLocked`: placed once at open in the standing universe and left there; `HandLocked` (later): relative to a controller |
| `distanceMeters` | 1.6 (panel) / 1.2 (popup) | along the HMD's -Z at placement time |
| `widthMeters` | 1.4 (panel) / 0.9 (popup) | overlay width; height follows the view's aspect |
| `heightOffset` | -0.15 | metres below eye height, so the panel's centre sits slightly low |
| `curvature` | 0 | `SetOverlayCurvature` where the runtime has it (IVROverlay_016 does not: leave 0 until the interface is bumped; OpenComposite has no curved overlays) |
| `sortOrder` | by layer | Hud < Panel < Popup < System, mirroring the flat z-order |
| `autoCloseMeters` | 3.0 | a world-locked panel the player walked away from closes itself; a head-locked one re-centres on cell transition |

Placement is applied through `SetOverlayTransformTrackedDeviceRelative(hmd,
…)` for HeadLocked (the runtime does the per-frame follow; we only re-issue
the matrix when the lazy-follow window is exceeded) and
`SetOverlayTransformAbsolute(TrackingUniverseStanding, …)` for WorldLocked
(Skyrim VR runs Standing — `BSOpenVR::SetTrackingSpaceAsStanding`).
Fullscreen views (SA's pages) are quads sized by `widthMeters` and the
backbuffer aspect: the page keeps rendering at backbuffer resolution and
self-scales, exactly as on flat. A dedicated VR resolution per view
(`ViewDesc.w/h` override for the VR quad) is a follow-up once text
legibility is measured in-headset.

**API surface (append-only, v4 tail, gate on `hostVersionNumber >= 1800`):**

```
struct VRPlacementDesc { uint32_t size; uint8_t mode; float distanceMeters,
                         widthMeters, heightOffset, curvature; float autoCloseMeters; };
Result (*SetViewVRPlacement)(ViewId view, const VRPlacementDesc* desc);   // nullptr = host defaults
Result (*GetViewVRPlacement)(ViewId view, VRPlacementDesc* out);
Result (*RecenterVRView)(ViewId view);                                    // re-place in front of the HMD now
```

`QueryCapability("vr")` returns 1 only while the VR presenter is live (runtime
found, interfaces bound, first overlay created). Manifest mods get the same
knobs as an optional `"vr": { … }` block per view (MANIFEST.md). Papyrus has
no placement call; a script's views take the manifest's `vr` block or the
host defaults.

## 6. Input

**Pointing — runtime-independent by construction.** Both runtimes serve
poses (`IVRCompositor::GetLastPoses` in the game's tracking universe — the
same poses the game rendered with; `IVRSystem::GetDeviceToAbsoluteTrackingPose`
as the fallback). The presenter keeps each visible overlay's world transform
(it authored it), so the laser is OUR ray/quad intersection: controller pose
→ aim ray (pose forward, -Z, with the runtime's aim offset ignored in v1)
→ intersect the quad plane → inside the rectangle → uv → view pixels.
`ComputeOverlayIntersection` and `PollNextOverlayEvent` are deliberately not
used: OpenComposite implements overlays as OpenXR quad layers and does not
emulate overlay input events **(verify — it is why a design leaning on those
events ends up OCU-only)**; owning the math removes the question.

**Hand selection:** the hand whose ray last hit any overlay is the active
hand; the other hand's laser is hidden. Trigger on a non-hitting hand does
nothing.

**Buttons — from the GAME, not the runtime.** Skyrim VR turns controller
state into `RE::ButtonEvent`s on VR devices (`INPUT_DEVICE::kVivePrimary`…
`kWMRSecondary`, `BSOpenVRControllerDevice::Key`: `kTrigger` 33, `kGrip`
2/34, `kXA` 7, `kBY` 1, `kJoystickTrigger` 32). Our `InputSink` already sees
them (they pass through untouched today). While the VR presenter is live and
UI mode is on:

| Control | Synthesised as | Notes |
|---|---|---|
| ray moves over a panel | `WM_MOUSEMOVE` (view pixels) | every frame the hit changes; hover works |
| trigger down / up | `WM_LBUTTONDOWN` / `WM_LBUTTONUP` | at the current hit; a down with no hit is ignored |
| stick Y (`thumbstick` events, `kThumbstick` input type) | `WM_MOUSEWHEEL` ±`WHEEL_DELTA` per ~0.35 deflection with repeat | scrolls what is under the ray |
| B/Y (`kBY`) on the active hand | leaves UI mode | the VR way out; a page that has taken Escape does not stop it; skipped when B/Y is bound for the UI-mode view |
| grip | `RecenterVRView` on the UI-mode view | cheap "bring it to me" |

`hitTest` in `DrainInputQueue` works in swapchain pixels; the VR path
converts the overlay uv to the VIEW's swapchain rect (`EffectivePos` + `w/h`)
so the existing hit-test resolves the same view. Multiple visible overlays
therefore behave like the flat stack: the ray hits one quad, and that quad's
pixel maps onto exactly that view.

**Keeping the game from acting on the same trigger** is the same
suppression UI mode already applies (`ToggleControls` off for
movement/looking/activate/fighting…); a finer approach masks combat
controls only while the trigger is over a panel ("masking combat controls
(trigger on panel)"). We start with the existing whole-UI-mode suspension
and refine only if VR users need to keep moving while a HUD-class view is
up. Whatever we do goes through `GameTask::Post` (section 4).

**Keyboard/text.** UI mode raises the engine text-entry flag; on VR that is
precisely the signal that pops OCU's auto keyboard and SteamVR's overlay
keyboard (the SA `VRImmersiveMode.h` comment records users hitting this with
the prior host). Decision: in VR the flag is raised only while a page text field
has focus (a page-bridge `__textfocus` signal from the `focusin/focusout`
handler the host already installs for `window.magelight`), and the host
calls `ShowKeyboardForOverlay` on SteamVR (chars arrive as
`VREvent_KeyboardCharInput` → the existing `WM_CHAR` queue path).
OpenComposite has no keyboard **(verify)**: v1 ships without one there
(OCU's `WM_OC_CHAR` would be an additional source later).

**Laser visuals.** Two thin quad overlays (one per hand, a 4×64 gradient
texture, premultiplied) transformed along the ray, shortened to the hit
distance; hidden when no hit. The page's CSS cursor dot is the hit marker
(the flat cursor sprite is not drawn in VR — `s_focused` still hides the
vanilla cursor, the own-cursor draw is skipped when `VR::IsLive()`).

## 7. Runtime detection and interface versions

```
REL::Module::IsVR()                      -> not VR: presenter never initialises
GetModuleHandleA("openvr_api.dll")       -> game loaded it (SteamVR or OpenComposite's replacement DLL)
VR_GetGenericInterface / VR_IsInterfaceVersionValid via GetProcAddress
IVRSystem_Version     "IVRSystem_017"    -> the vtables our vendored openvr.h describes
IVRCompositor_Version "IVRCompositor_021"   (openvr ebdea152, the commit CommonLibVR pins)
IVROverlay_Version    "IVROverlay_016"
```

**Never request a version string newer than the header's vtable.** The
strings and the vtable layout are one contract; another binary asks for
016/025/026 and 017/021/022 because it compiles against a newer header and
probes downward. Ours asks for exactly the three above (both runtimes still
serve them **(verify for OpenComposite)**); a bump means re-vendoring the
header through the NG port, not editing a string.

**Two IVROverlay pointers, on purpose.** The game's proxied context
interface (`BSOpenVR::GetIVROverlayFromContext`) is fine for create/
transform/show but NG documents `SetOverlayTexture` through it failing with
`VROverlayError_PermissionDenied`; the "clean" interface from
`VR_GetGenericInterface` submits textures. We use the clean interface for
everything (one pointer, one code path) and keep the proxied one as the
diagnostic fallback if creation fails **(verify)**.

`VR_GetInitToken` is checked once per frame; a token change (runtime
restart) drops every overlay and re-initialises next frame.

## 8. Build and packaging

- `src/MagelightVR/` joins the host target as six translation units
  (Runtime, Input, Bindings, Overlays, Laser, Lifecycle) sharing `State.h`; no
  new link libraries (`vr::` types come from `openvr.h`, already propagated by
  the NG port into `vcpkg_installed/.../include`). `openvr.h` is included by
  `State.h` only, which every one of them includes.
- Two rules govern the split, both stated in `State.h`: `OverlayRec` is
  declared once and `Overlays.cpp` owns its lifecycle while `Laser.cpp` mutates
  individual fields through `s_overlays`; and the per-frame pose snapshot is
  present-thread only.
- `ENABLE_SKYRIM_VR=1` is already on for every consumer of the NG target;
  the VR code is guarded by runtime checks, not `#ifdef`, so the flat build
  compiles and links it too (as SA's `VRImmersiveMode` does).
- Version bump to **0.17.0** when VR-2 (first overlay in-headset) lands;
  the design and skeleton do not change the deployable.
- `Magelight.json`: a `vr` object; the keys and defaults are in the
  `Magelight.json` table of [TROUBLESHOOTING.md](TROUBLESHOOTING.md).
- Address-library exposure in VR: everything the host touches is
  NG-wrapped (`MenuCursor`, `ControlMap`, `UI`/`UIMessageQueue`,
  `BSInputDeviceManager`, `BSWin32KeyboardDevice` vtable for the poll mute,
  `BSGraphics::Renderer` data). Each of those has a VR id in NG or the
  build would not have compiled with `ENABLE_SKYRIM_VR` — but NG marks some
  offsets "VR untested". **(verify)** the keyboard-poll vtable write and the
  `CursorMenu`/`MenuCursor` drive under VR; the CursorMenu is irrelevant to
  pointing (the laser writes the queue directly) but UI mode still opens it
  today. A VR-specific `SetUIModeImpl` branch may skip the CursorMenu and
  the vanilla-cursor hide.

## 9. SeverActions follow-ups

Nothing in SA is required for VR-1..3. When the presenter is live:

- `VRImmersiveMode` (`auto` = active on VR) exists because the prior host's
  VR cards had no working pointer. With a laser that works, `auto` should
  read the host: active only when `QueryCapability("vr") == 0` on a VR
  runtime (i.e. the earlier behaviour stays for a user without the VR
  presenter). Three-way setting keeps its meaning.
- The live actor mirror / item stage (`RegisterTextureImageEx`) refuses on
  VR today (`MannequinRenderer: LiveStart refused — VR runtime`). Unrelated
  to the presenter; the texture-image path works in VR like on flat because
  the page samples it — re-enable once the renderer's VR render-target
  assumptions are checked.
- SA's `GamepadUIBridge` semantics are the model for the host gamepad item
; the VR button→pointer synthesis in section 6 is the same
  machinery and should land in the host once, not twice.

## 10. Field test plan

- VR instance: Skyrim VR 1.4.15.0 through SKSEVR, with VR Address Library,
  in a mod-manager profile. Test SteamVR-native first, then OpenComposite on
  the same profile.
- Deploy the stage as its own mod. Logs: `My Games\Skyrim VR\SKSE\Magelight.log`.
- Disable any other Ultralight-based UI host for the runs (both are namespaced,
  but the test should isolate). The host test needs no consumer mod: use the
  manifest examples (`build.ps1 -Examples`; `Magelight.Badge` F6/F7,
  `Magelight.ReactConfig` F8).
- Pass 1 (VR-1) is log-only: present-thread identity, `openvr_api.dll`
  found, the three interface versions valid, first `CreateOverlay` result.

## 10a. VR-1 result (2026-09-03, MO2 VR instance, OpenComposite Unleashed 1.9 / OpenXR)

The first field pass ran on a MO2 VR instance whose SkyrimVR runs
**OpenComposite Unleashed 1.9 with the OpenXR backend** (`opencomposite.ini`
in the game folder; `OCUnleashedSKSE.log`) — the harder runtime first. Log:
`My Games\Skyrim VR\SKSE\Magelight.log`, host 0.17.0.

| Claim (section) | Result |
|---|---|
| Present-thread rule holds under SkyrimVR.exe (§4) | **Yes.** Plugin-load thread 50272; the present thread alternated 26036 (menus/loading) ↔ 50272 (in-game) exactly as on SE; Ultralight and the VR presenter initialised on 50272 on the first in-game frame; no `SubmitFrame on thread` warning |
| `openvr_api.dll` in-process + `VR_GetGenericInterface` (§7) | **Yes** — OpenComposite's replacement DLL exports it |
| OC serves IVRSystem_017 / IVRCompositor_021 / IVROverlay_016 (§7, was (verify)) | **Yes**, all three bound; init token 1; tracking space 1 (Standing) |
| `CreateOverlay` through the clean interface (§7) | **Yes**, handle returned; 6 tracked devices, HMD present |
| Flat host unaffected | Yes — GPU path, three example views loaded to DOM ready |

**SteamVR-native pass (same day, 12:04, OCU removed from the game folder):**
identical outcome — same thread pattern (main 33552, loading-screen thread
20288), the three interfaces bound from Valve's own `openvr_api.dll` (init
token 1, tracking space 1), probe overlay created, 3 tracked devices, HMD
present, all example views to DOM ready. Both runtimes clear VR-1.

**VR-2 first pass (0.17.1, SteamVR native, 12:25): CTD on the first F10.**
Both overlays were created and shown (`overlay shown for view 1 (216x44, 0)`,
`... view 2 (420x240, 0)` — the runtime accepted our host textures on the
first try), then the engine crashed inside its menu-open processing with
our `MagelightFocus` on the stack (R8 = the menu name). Root cause is an
OBJECT LAYOUT gap, not the overlay: `RE::IMenu` is 0x30 bytes on SE/AE and
**0x40 on VR** (`STATIC_ASSERT_SIZE(IMenu, 0x30, 0x30, 0x40, 0x30)`) — the
VR engine keeps `unk30`, `unk34` and a `BSFixedString menuName` at 0x38 on
every menu, and NG's cross-runtime build reaches them by relocation instead
of declaring them, so a menu allocated at the SE size hands VR garbage past
its end (the crash reads through that string pointer). Fixed in 0.17.2 by
giving `MagelightFocusMenu` the VR tail as real members
(`static_assert(sizeof == 0x40)`). **Every `RE::IMenu` subclass in a
universal SE/VR plugin needs the same** — SeverActions' live-view carrier
menu (`ItemRenderer.h`) has the identical exposure and must get the same
tail before its VR live views are re-enabled. One way to dodge the crash is
to never register a focus menu on VR; we keep ours (menu
context, Cancel, pause-per-open) with the layout fixed.

**VR-2 pass (0.17.3, SteamVR native, 12:49): overlays open, close and toggle
in the headset.** Panel (F10), config page (F8) and the badge (F9; the
example keys of that build, now F7, F8 and F6) each
show/hide as overlays with the runtime returning 0 on every ShowOverlay;
UI mode leaves via the view's own hotkey, via **B/Y on the controller**
(`controller B/Y (device 5)` = kOculusPrimary, so Quest/Touch controllers
report through the Oculus device ids under SteamVR too) and the view's
overlay hides on exit. The 0.17.1 "no way to close" was the keyboard mute:
on VR the window proc needs OS focus the mirror window rarely has, so 0.17.3
skips the mute on VR, closes on Escape through the sink, and routes VR
controller buttons through the sink (B/Y = close). Owner reports "all seems
to be working" — legibility/alpha judgement still to be collected.

**VR-3 first cut (0.17.4): controller laser — hover + click.** Design
verified by a 5-lens grounded panel before implementation (poses, transform
space, ray math, injection, OC compat). Decisions locked: pose source is
`IVRCompositor::GetLastPoses` render array (no universe/prediction arg,
inherits the compositor space the overlays were composited in), gated on
`bPoseIsValid && bDeviceIsConnected && Running_OK`; panel world reconstructed
each frame `= HMD_pose * HeadRelative` into `OverlayRec.world` (overlays STAY
native head-locked — not switched to per-frame absolute); own-math
`IntersectQuad` on BOTH runtimes (never `ComputeOverlayIntersection` — an
unimplemented call on OC returns false = a silent miss); controller index
re-resolved per frame via `GetTrackedDeviceIndexForControllerRole` and
BOUNDS-CHECKED against `k_unTrackedDeviceIndexInvalid`/max (an off controller
returns 0xFFFFFFFF → OOB pose read → CTD, the most likely first-cut crash);
`heightM = widthM * texH/texW`; topmost-first hit (reverse frame order, matches
the host hitTest); pixel injected through a new `Magelight::QueueSyntheticInput`
bridge (the file-static `QueueInput` is invisible to the VR TU); trigger drives
clicks off the LEVEL not the edge counter; the sink swallows the VR trigger +
thumbstick while a page is focused so they can't also fire the weapon / turn
the player (B/Y stays linked for the exit). A fixed `kAimPitchDeg = -35°`
grip→aim tilt (tunable, 0 = raw) prevents a "laser points above my hand" false
report; the render-model `tip` transform is the VR-3.1 replacement. Deferred to
VR-3.1: thumbstick→wheel scroll (needs deadzone + rate-limit + staleness
reset), the visible beam overlays (session-global handles, static texture,
released in Shutdown only — strict teardown discipline), and the tip
transform. Accepted first-cut gaps: injected input drains one frame late
(~11ms, harmless on static panels); a drag whose release leaves the panel
routes the up through the UI-mode-view fallback.

**VR-3 visual feedback (0.17.5).** The 0.17.4 plan assumed the page draws
its own cursor — it does not (the OS draws the pointer on flat; Ultralight
renders none), so a laser with no beam and no host sprite had ZERO visual
feedback. Fixed two ways: the copy pass now stamps the host's cursor sprite
INTO the panel texture at each hand's last hit uv (straight-alpha 'over'
blend, 36px@1080p scaled to the view, never under 24px), and two
session-global BEAM overlays (one per hand; a static 256x2 straight-alpha
gradient set once; per frame only width + an absolute transform billboarded
toward the HMD so the strip is never edge-on; shortened to the hit distance,
2m idle; hidden when the controller is unusable or UI mode is off; released
in Shutdown only, never per view; handles forgotten on an init-token change).

**13:40 crash (0.17.4 session, before any UI mode)**: execute-address fault
(jumped to a heap address) inside the game's own scene render pass — BSBatch
Renderer / BSShaderAccumulator with Community Shaders on the stack 23 times
and Engine Fixes' alpha-test hook; no Magelight frame. Registers held float
geometry data (a return-address smash from below). Nothing 0.17.4 added runs
outside UI mode, and the identical per-frame path ran clean through 0.17.1–
0.17.3. The same session had controllers repeatedly disconnecting at launch
(a SteamVR/USB flake day; the 11:54 launch crash was DXGI during compositor
init too). Not attributed to Magelight; watch for recurrence — if it repeats
at ~3s after world-ready, bisect with `"vr": { "enabled": false }`.

**VR-3 field pass (0.17.5, SteamVR native, 13:53): beams and panels live in
the headset.** Owner: "the lasers just come right from your hands in game
which I thought was nice. the badge and open UI does follow the headset as
well." Log: both beam overlays created on the first UI-mode entry, panel +
badge shown, no faults. The 0.17.4-session crash + controller drops did NOT
recur on 0.17.5 in the same conditions. Still to confirm from the owner:
click landing, dot-vs-beam agreement, and whether the -35° aim tilt is right.

**VR-3.1 (0.17.6): lazy follow (default), grip recenter, thumbstick scroll.**
Placement modes are now `HeadLocked` (glued; runtime-relative transform —
HUD widgets keep this), `LazyFollow` (the new default for panels/popups/
system: a LEVEL panel placed at head + horizontal-forward*distance +
heightOffset, `SetOverlayTransformAbsolute` in the compositor space; it stays
put until the gaze drifts past 30° or the head moves 0.5 m, then glides back
(~150 ms time constant) and snaps within 1.5°/2 cm; the SAME matrix is what
the laser hits, so lazy panels no longer need the HMD*relative reconstruction
— only HeadLocked ones do) and `WorldLocked` (placed once, never follows).
The per-frame pose snapshot (`GetLastPoses` render array) moved from
TickLaser into SubmitFrame so the follow glide and the laser share one read.
Grip on either hand recentres every non-HUD panel (placed=false → next
frame re-places in front of the head). Thumbstick Y scrolls the panel under
the ray: 0.30 deadzone, 6 notches/s at full deflection through an
accumulator, a 250 ms staleness guard on the last stick event (a stick that
stops reporting can never scroll forever), stick up = wheel up (`+WHEEL_DELTA`,
mirrors the flat wheel path — flip in-field if reversed). The earlier design's
"overlays stay native head-locked" now applies to HeadLocked only.

**VR-3.1 field pass + the analog-axis lesson (0.17.6 → 0.17.7).** Owner:
lazy follow "brings it to center so that's great", grip recentres. But
"none of it was clickable ... not the toggle and not the slider" while the
log showed the React page's `save` firing in BURSTS (three inside 12 ms)
and `grip — panels recentred` every 22 ms for two seconds while held. Root
cause: **on VR the trigger and grip are ANALOG axes, and the engine emits a
fresh ButtonEvent with `heldDownSecs == 0` every frame the value changes —
so `IsDown()` is true every frame, not once per press.** Each pull became a
burst of down/up pairs: the checkbox toggled back to where it started, the
slider had no held state to drag with, the grip re-placed every frame.
0.17.7 feeds `ButtonEvent::Value()` (the analog level) into `NoteButton`,
thresholds at half travel, keeps the trigger as a LEVEL for TickLaser's edge
derivation and latches grip/B-Y rising edges on the game thread. B/Y kept
working under `IsDown()` because it is a digital button. Rule: **never use
IsDown()/IsUp() as a click edge for a VR axis input.**

**SKSEVR does not deliver kNewGame (0.17.9).** An OCU session started as a
NEW game (Alternate Perspective, autosave `APStartCell`): `sksevr.log` shows
SKSE dispatching messages 4 and 5 only — no 3 (PostLoadGame, it was not a
load) and no 7 (NewGame) — so `NotifyWorldReady` never ran, Ultralight never
initialised, and every hotkey suspended the controls over an empty screen
("none of the keys worked"). The earlier OCU pass was a LOADED save, which
does deliver 3. Fix: a world-ready FALLBACK in FrameWork — on a main-thread
frame, ~twice a second, if the player has a parent cell, is 3D-loaded and
neither MainMenu nor LoadingMenu is open, world-ready is declared (the
thread-identity gate still decides when Ultralight is created); and UI
mode is REFUSED with `UIModeRefused "renderer not ready"` while the
renderer is down, so a hotkey can never strand the player with suspended
controls again. Also on record from the engine-event diagnostic: the grip
AXIS (code 34) reports `held=0.000 down=true` EVERY frame while squeezed;
the grip BUTTON (code 2) counts held time normally — the analog-axis IsDown
lesson, now proven in the log.

**0.17.10–0.17.11 field findings (OCU).** (1) Clicks DO land: each laser
press/release pair on the checkbox is followed by exactly one page `save`
~10 ms after the release; the React example page simply could not SHOW it —
it is a controlled form redrawn only from a host-echoed `state` channel, and
a manifest-only mod has no host. The example now keeps local state. (2) The
release used to be queued at (0,0), which the host re-hit-tested to a corner
outside every control — fixed in 0.17.10 (release at the hand's last hit
pixel). (3) **OpenComposite does not honour `SetOverlayTransformTracked
DeviceRelative`** — the glued HUD badge sat on the floor; HeadLocked
overlays now push `world = HMD * relative` as an ABSOLUTE transform every
frame (same matrix the laser hits). (4) **OCU's own menu laser** appears
under ours whenever an engine menu is up (its `OC_MENU_ACTIVE` bridge) and
drives the vanilla MenuCursor; on VR the sink now ignores the flat mouse
path entirely so that second pointer cannot compete with the laser. (5)
**OCU's virtual keyboard** pops on our text-entry flag and posts each
character to the game window as a private message (`WM_OC_CHAR`), which the
host already subclasses and handles directly — no change on OCU's side (see
0.18.1 below). On the SteamVR-native runtime the host raises the overlay
keyboard (`ShowKeyboardForOverlay`, VR-4). Either way the host also has its
own on-panel keyboard, raised on text focus, that needs no runtime keyboard
at all.

**VR-3 / VR-3.1 FIELD-PASSED (0.17.11, OpenComposite Unleashed, 16:46):**
owner "okay now the clicks worked properly". Log: 22 press/release pairs a
few pixels apart on both hands, checkbox and slider responding, grip once
per squeeze, B/Y exit, lazy follow and recentre confirmed earlier on SteamVR
native. The laser stack (poses → own intersection → queued input → cursor
dot + beam) is now proven on both runtimes.

**VR-4 (0.18.0): the modder-facing surface.**
- **API v4 tail** (append-only, gate on `hostVersionNumber >= 1800`; export
  table order matches the struct, invariant 10): `SetViewVRPlacement`,
  `GetViewVRPlacement`, `RecenterVRView`, `BindVRHotkey`, with
  `VRPlacementDesc` (mode / distance / width / heightOffset) and
  `VRHotkeyDesc` (button / modifier / hand / action).
- **VR controller bindings** — the answer to "I cannot open the menu without
  a keyboard". Polled from `GetControllerState` every frame REGARDLESS of UI
  mode (that is how a view opens while the game owns input), rising-edge with
  a 250 ms debounce, then marshalled to the game thread through the new
  `Api4::DispatchViewAction` — the same gates and the same
  only-this-view's-own-binding-closes rule the keyboard hotkeys use.
  `DispatchHotkey` was refactored onto it so the two paths cannot drift.
  Buttons are OpenVR EVRButtonId values with analog fallbacks for trigger and
  grip (the axis lesson). Manifest: `"vrHotkey": {button, modifier, hand}`.
- **SteamVR virtual keyboard**: the page bridge now reports text-field focus
  on a reserved `__textfocus` channel (focusin/focusout, intercepted in the JS
  dispatcher before any mod listener); the presenter raises
  `ShowKeyboardForOverlay` on the focused view's overlay and drains
  `VREvent_KeyboardCharInput` into the ordinary queued key path (backspace as
  VK_BACK). If the runtime has no keyboard to raise (OpenComposite) it logs
  once and stands down — OCU types through the `WM_OC_CHAR` window message
  instead, and the host's own on-panel keyboard is always available.
- Manifest `"vr": {mode, distance, width, heightOffset}` per view; the Badge
  example binds its panel to **A + grip** and its badge to head-locked.
- Cleanup: the click-edge and engine-VR-button diagnostics are gone.
- Still owed: the Papyrus surface for placement/bindings, and the
  render-model `tip` aim transform replacing `kAimPitchDeg`.

**0.18.1 — OpenComposite's keyboard needs NOTHING from OCU.** Their public
source (`OpenOVR/Misc/Keyboard/VRKeyboard.cpp`) shows the export path is only
half the story: `PostCharToGame` posts every character to the GAME WINDOW as
a private message, `WM_OC_CHAR = WM_APP + 0x4F45`, wParam = wchar_t, lParam
0 = printable / 1 = a virtual key (Backspace, Enter, Tab, Escape). Magelight
already subclasses that window, so 0.18.1 handles the message directly and
OCU's keyboard types into a focused page with no change on their side. The
window message is the road in; the host also exports a generic
`Magelight_DeliverChar/VKey` hook, though nothing needs it. Consumed only
while a page holds focus;
otherwise it passes through to the console/Scaleform untouched. Field note:
`ShowKeyboardForOverlay` SUCCEEDS on OCU — it raises OCU's own keyboard, not
SteamVR's, which is exactly what we want.
Also 0.18.1: a manifest `"vr"` block with no distance/width no longer drags a
HUD badge to the panel default (overrides now MERGE onto the layer default,
0 = inherit, applied after it is seeded); a grip held as a binding MODIFIER
no longer counts as a recenter edge (the latch seeds from the live level on
UI-mode entry); the Badge example ships hidden (its hotkey, now F6, shows it).

**0.18.2 — one pointer, and it looks like the runtime's.** Deferring to
OpenComposite's laser is NOT possible: their beam targets OCU's own menu quad
and knows nothing about our OpenVR overlays, so it can never hit-test a
Magelight page (their public source also shows the MCM menu-laser system
`#if 0`-disabled; what appears over our panels comes from the local build's
menu laser + the keyboard laser). Suppressing it IS possible and is what we
do: `OpenCompositeInput/src/Main.cpp` sets a window property,
`SetPropW(hwnd, "OC_MENU_ACTIVE", anyMenuVisible)`, on every
MenuOpenCloseEvent — and UI mode's focus menu counts as a menu. While a page
holds focus we clear that property (on entry and on the CursorMenu watchdog
tick, since OCU re-asserts it), and OCU drops its beam; their own watcher
puts it back the moment a real menu opens or ours closes, so the flag is
never left wrong. What OCU gates on it (WASD blocking) our own
`ToggleControls` suspension already covers. Setting:
`vr.suppressRuntimeLaser` (default true).
Beam look: 512x2 instead of 256x2 (the overlay's height is
width * texH/texW, so a wider texture is a THINNER beam — ~0.8 cm at 2 m,
half of what it was), white instead of light cyan, alpha 0.55
(`vr.beam` off entirely, `vr.beamAlpha` to taste).
VR pointer: a small white disc with a dark rim at the laser's end
(`vr.cursorDot`, default true, 0.26.12), sized as a fraction of the panel
height (`vr.cursorScale`, default 0.012, floor 0.004) instead of the flat
cursor's 1080p-relative size with a 24 px floor — that floor was ~10% of a
240 px panel and read as a huge arrow stuck on the end of the beam.
`"cursorDot": false` brings the arrow (or a custom cursor PNG) back.

**Why the export road was abandoned (0.18.3).** OpenComposite can also hand
text to a UI host through a named DLL export, but it resolves that export by
MODULE NAME on a specific host's DLL, not ours — so it could never reach
Magelight, and chasing it was a dead end. The `WM_OC_CHAR` window message
above is the road in; on the SteamVR-native runtime the host's own
`ShowKeyboardForOverlay`/`VREvent_KeyboardCharInput` path owes nothing to OCU
and is the cleaner test of our side.

**THE REAL KEYBOARD BUG (0.18.4): in VR the game window receives no keyboard
messages at all.** With both copies of that DLL disabled the diagnostics were
STILL silent — no `WM_OC_CHAR`, no `WM_CHAR`, no `WM_KEYDOWN` — and the owner
found that a PHYSICAL keyboard would not type into a page either. That is the
tell: the window-proc typing path the flat host uses is dead in a headset,
because the game window almost never holds OS focus there (SteamVR/OCU owns
it). Invariant 6 says the mouse never gets WM messages in Skyrim; the keyboard
does not either once the window is unfocused, which is the normal VR state. So
the prior-host hand-off was only ever the SECOND problem, and asking OCU to
widen its module probe is a nice-to-have, not the fix.
Fix: on VR, type from the SKSE input sink — the engine keyboard device, the
same source Escape and the hotkeys already use. `QueueScancodeAsText`
translates the DirectInput scancode into exactly the `WM_KEYDOWN` /
`WM_CHAR` / `WM_KEYUP` the window would have produced (`MapVirtualKeyW` +
`ToUnicode` with live modifier state from `GetAsyncKeyState`, which needs no
focus) and queues it; `DrainInputQueue` is untouched. Gated on
`VR::IsLive()` so flat never double-types, and skipped for the toggle key,
Escape and bound hotkeys so a close key is never also a character. This should
also pick up OCU's keyboard, which synthesises scancodes at the driver level
alongside its own delivery paths.

**VR-4 FIELD-PASSED (0.18.4, OpenComposite, 2026-09-03).** Owner: "yup that
worked perfectly". Controller bindings open and close a panel with no
keyboard; the beam is thin, white and only drawn on a panel; the pointer
sprite scales with the panel; OCU's second beam stands down; and typing works
from a physical keyboard and from **OCU's keyboard in PC mode** (it
synthesises scancodes at the driver level, which the engine device sees).
OCU's keyboard in **VR mode** still does not reach us — that road is the
name-keyed export road, and getting OCU to probe our module by name is now a
polish request rather than a blocker. The keyboard-hunt diagnostics were
removed in 0.18.5; the rate-limited `VR typing — scancode` line stays.

**The VR presenter is complete**: detection, overlays,
placement, laser, controller bindings and typing all field-passed on SteamVR
native AND OpenComposite. Remaining VR odds and ends, none blocking: the
Papyrus surface for placement/bindings, the render-model `tip` aim transform
replacing `kAimPitchDeg`, and a SteamVR-native pass on the final build.

**0.19.0 — aim from the controller's own render model.** `kAimPitchDeg` is
gone as the primary: the laser now takes its frame from the controller's
render-model **tip** component, whose local space OpenVR defines with **-Z out
of the surface** — exactly the ray convention `IntersectQuad` already uses, so
it drops in as `aim = devicePose * mTrackingToComponentLocal`. It is
per-controller correct, which one hard-coded tilt can never be: a Touch, an
Index and a Vive wand are each held differently. Resolved once per device
(`Prop_RenderModelName_String` → `RenderModelHasComponent` →
`GetComponentState`; the component is static, so the live controller state is
passed but the result is cached) and re-resolved only when the model name
changes or the runtime restarts. `IVRRenderModels` is bound OPTIONALLY — a
runtime that does not serve it, or a model with no tip, falls back to the
fixed local-X pitch, now `vr.aimPitchDeg` (default -35). `vr.aimUseTip: false`
forces the fallback. The log names which source each hand got.

**0.19.3 — the runtime keyboard is OFF by default, and here is why.**
Raising SteamVR's keyboard from a SCENE application (Skyrim is one; keyboard-
for-overlay is really an overlay-app facility) makes the compositor move
INPUT FOCUS away from the game. Field 2026-09-03: the instant
`ShowKeyboardForOverlay` succeeded the overlay queue produced
`VREvent_InputFocusReleased` (401) and `VREvent_InputFocusChanged` (406) in a
burst, the frame rate collapsed to single digits for the whole time it was
up, and NO keyboard was ever visible; clicking away restored both. So the
call is not merely useless on a scene app, it is harmful. `vr.runtimeKeyboard`
now defaults false (the text-focus signal still fires, for our own keyboard
later); the event drain is also BOUNDED at 64 per frame, because an unbounded
drain on the present thread is a frame stall by construction.
Typing loses nothing: the input sink reads the engine keyboard device, which
covers a physical keyboard and any runtime keyboard that synthesises
scancodes (OpenComposite's PC mode).
Also confirmed this run: **the render-model tip aim works** — `hand 0 aim =
render-model tip of 'oculus_quest2_controller_left'` and the right likewise.

**0.20.0 — Magelight's OWN virtual keyboard.** Every runtime keyboard turned
out to be a dead end (SteamVR's steals input focus and stalls the game;
OpenComposite's VR mode delivers through a module resolved by NAME), so the
host now ships one: `views/keyboard/index.html`, a page in the same theme as
the rest of the UI, raised automatically when a text field in another view
reports focus on the reserved `__textfocus` channel and dismissed on blur, on
Close, or with UI mode.
The design point that makes it work: **the keyboard never takes UI mode.** It
is a System-layer, non-click-through view, so the laser hit-tests and clicks
it like any page while the view being typed into keeps key focus — and
`DrainInputQueue` already routes keys to the UI-mode view, so an injected
character lands in the text field, not in the keyboard. Keys arrive on a
second reserved channel, `__key`: `{"c":"a"}` a character, `{"vk":8}` a
virtual key, `{"close":true}` dismiss; the host queues them exactly as a typed
key. Shift is one-shot, Caps latches, both relabel the keys live. In VR it is
placed LazyFollow at 1.1 m, half a metre below eye line. `vr.keyboard: false`
turns it off. It is runtime-independent by construction, which no other option
was.

**0.20.1 VERIFIED in the headset (19:41).** Keyboard raised only after the
text field was clicked (view 4 created 2 s AFTER UI mode opened, not at load);
`view 4 grabbed by hand 1 at 0.88m` then `released`; backspace repeated after
a 400 ms delay then every ~67 ms; both controllers took their aim from the
render-model tip. The VR presenter is feature-complete for 1.0.

Not yet observed: `probe overlay destroyed` — `Shutdown` runs only on render
death, and a normal quit has no plugin-unload hook (harmless; the process
ends).
Bonus datum from the OCU log: `BaseOverlay::_BuildLayers — Menu laser:
destroyed (menu-active flag went false)` — OCU runs its own menu laser keyed
on the engine's menu-active state, which is what the prior host leans on; ours
stays independent of it (§6).

## 10.1 0.21.0 — the first real page in a headset

Field round with SeverActions' own pages (VR instance, Quest 2, SteamVR). The doll
rendered and the menu worked, which surfaced three things nothing smaller had:

**A fullscreen view was tracking the DESKTOP MIRROR's aspect.** Skyrim VR's
mirror window is very nearly SQUARE (1024x1024 here; the user's desktop grab
was 1261x1277), and `fullscreen` means "track the backbuffer" — so a page laid
out for a wide desktop was rendered into a square and every column was crushed.
The backbuffer is the right answer on flat and the wrong one in a headset,
where nobody is looking at that window at all. VR fullscreen views now take an
explicit panel resolution (`vr.panelWidth`/`panelHeight`, default 1600x900),
and the desktop mirror LETTERBOXES the panel instead of drawing it 1:1 and
cropping the right edge.

**The lazy-follow deadzone was narrower than the panel.** 30 deg from centre
sounds generous until you notice the panel's own half-angle is 24 deg at
1.4 m / 1.6 m — so looking at its far column was already most of the way to
triggering a glide, and a bigger page would have been worse. The threshold is
now measured from the panel's EDGE (`followAngleDeg` + the panel's half-angle),
so the deadzone grows with the panel automatically. `widthMeters` default went
1.4 -> 1.6 in the same pass, because a 16:9 panel is much shorter than the
square one it replaced.

**A mod whose menu does real work cannot use a built-in hotkey action.**
`kHotkeyActionToggleUIMode` shows a view; SeverActions' open also gathers page
data, snapshots the pause setting and raises a text-entry gate, so binding the
built-in action would have shown the page with its bridge's bookkeeping
desynced. Hence the 0.21.0 API tail: `BindVRHotkeyCallback` hands the button
edge to the mod and lets it run its own path. The mod cannot do this itself —
VR triggers and grips are ANALOG axes that re-emit a press every frame through
the SKSE input sink (the 0.17.x click bug), which is exactly why this layer
polls `GetControllerState` and why it has to own the edge for everyone.

SA binds Grip + B/Y: a bare button fires during gameplay, and Grip + A/X is
what the shipped example view already uses.

## 11. Milestones

| # | Deliverable | Proves |
|---|---|---|
| VR-1 ✅ (OCU + SteamVR 2026-09-03) | Detection + thread telemetry + one hidden overlay created/destroyed (log only) | present-thread rule under SkyrimVR.exe; interface versions; clean-vs-proxied overlay pointer |
| VR-2 ✅ (SteamVR 2026-09-03) | One visible view submitted as a head-locked quad (copy pass, alpha switch, hide/destroy on lifecycle) | texture ownership + lifetime rule; alpha convention per runtime; mirror coexistence |
| VR-3 ✅ (0.17.11, SteamVR + OCU 2026-09-03) | Laser hover + trigger click + stick scroll + beam + grip recentre + lazy follow | runtime-independent pointing; multi-view hit mapping; the pose/transform conventions |
| VR-4 | Placement API (v4 tail + manifest + Papyrus), `QueryCapability("vr")`, `Magelight.json` knobs, SteamVR keyboard | the modder-facing surface a prior host never exposed |
| VR-5 | OpenComposite pass on the same profile; docs (`TROUBLESHOOTING.md` VR section, `NEXUS_PAGE.md` "VR: yes") | posture claim from section 1 |
