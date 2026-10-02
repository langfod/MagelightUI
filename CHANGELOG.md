# Magelight UI — changelog

Versions are `MAJOR.MINOR.PATCH`. The C++ ABI is **append-only**: a field is
only ever added to the end of a struct, never reordered or retyped, so a
plugin built against an older header keeps working against a newer host. Gate
any call added after your minimum on `hostVersionNumber >= PACKED` (packed =
`MAJOR*10000 + MINOR*100 + PATCH`), or ask `QueryCapability("name")`.

The **"added in"** column is the version to put in `minHost` (manifest) /
`minHostVersion` (C++ `RegisterMod`) if your mod needs that feature.

## API feature history

| Added | Gate | Feature |
|---|---|---|
| 0.30.1 | 3001 | `PostGameTask(fn, user)` — post game-thread work from any thread; from a callback inside a frame it never waits on SKSE's task lock |
| 0.30.0 | 3000 | `NetworkPolicy::FileOnly` (the new default), manifest `"network": "file" \| "loopback"`, Papyrus `SetNetworkPolicy(modId, policy)`; the Papyrus tier acts only on script-owned mods |
| 0.29.0 | 2900 | `PlayUISound(view, name)`, `SetViewSounds(view, open, close)`; page `magelight.sound()` / `__sound`, `data-ml-sound` markup, manifest `sounds`, Papyrus `PlaySound`; `QueryCapability("sound")` |
| 0.28.2 | 2802 | `SetNetworkPolicy(mod, policy)` — per-mod escape from the file + loopback sandbox; `QueryCapability("networkpolicy")` |
| 0.28.0 | 2800 | `SetEscapeCapture`, `ShowInspector` / `IsInspectorVisible`, `SetViewOrder` / `GetViewOrder`, `SetScrollStep`; loopback `http(s)` allowed; capabilities `escapecapture` `loopback` `vieworder` `scrollstep` |
| 0.27.0 | — | Native IME (CJK composition) on flat; `QueryCapability("ime")` |
| 0.26.11 | 2611 | `RequestUIMode` on the holding view retargets `kUIModeFlagPause` in place |
| 0.26.9 | 2609 | `SetViewScale(view, scale)`; `ViewDesc::uiScale` is the real Ultralight device scale and round-trips through `GetViewInfo` |
| 0.26.8 | 2608 | `FindView(mod, name, &id)` — resolve a view name to its id from C++ (the Papyrus tier already had it) |
| 0.26.8 | — | Manifest robustness (a mistyped scalar warns instead of throwing); capability list single-sourced |
| 0.26.6 | 2606 | `kUIModeFlagNoTextEntry` (a page with no text field asks the host not to raise the engine text-entry gate — on Skyrim VR that gate summons OpenComposite's keyboard) |
| 0.26.4 | — | Host pinning: a page reads only its own `Data/Magelight/<Mod>/` folder and the runtime dir |
| 0.26.3 | — | OpenComposite (OCU) detected and render-model tip-aim skipped there (its stub pops a dialog) |
| 0.26.0 | 2600 | `SetVRButtonListener` — raw controller button edges for press-to-bind chord UIs |
| 0.21.0 | 2100 | `BindVRHotkeyCallback` — a VR controller chord that calls YOU instead of running a built-in action |
| 0.18.0 | 1800 | VR presenter: `SetViewVRPlacement` / `GetViewVRPlacement` / `RecenterVRView` / `BindVRHotkey`, `VRPlacementDesc`, manifest `vr`/`vrHotkey` |
| 0.16.0 | 1600 | `SetViewCutout` (compositor cutout), `SetViewHibernate` (release a hidden view's texture); Papyrus `SetCutout`, `SetHibernate` |
| 0.15.0 | — | `window.magelight` page bridge installed on every page; SDK (`@magelight/sdk`, `@magelight/react`, vite plugin, `create-magelight-view`) |
| 0.14.0 | 1400 | `EvalJS` — run script and read the result / catch the exception back; Papyrus `EvalAsync` |
| 0.13.0 | 1300 | Papyrus tier (`Magelight.psc`: `RegisterMod`, `CreateView`, `FindView`, `ShowView`, `RequestUIMode`, `Call`, `Eval`, `BindHotkey`, `RegisterListener` and the rest); the version gate page |
| 0.12.0 | — | `BindHotkey` registry; manifest `hotkey` |
| 0.11.0 | — | Sessions / manifest docs |
| 0.10.0 | — | API v4 (`MagelightApi4`): per-mod registration, texture images, UI mode, events |

## 0.30.3

Three fixes for 0.30.2.

- **Crash at startup with Community Shaders (fixed).** 0.30.2 asked the game's
  swapchain for its device on every frame, to skip swapchains on other devices,
  and released it. Community Shaders' swapchain wrapper hands the device out
  without adding a reference, so each release took one of the game's own, and
  the device was freed within a second (an access violation in `d3d11.dll`, with
  or without `Magelight.json`). The vtable hooks now compare the swapchain with
  the game's own; only the dxgi detours, which see dxgi's own swapchain, ask for
  its device.

- **Crash at startup with ENB (fixed).** Behind a swapchain proxy (ENB's or
  ReShade's `d3d11.dll`, Skyrim Upscaler), 0.30.2 created a throwaway Direct3D
  device to find dxgi's own Present, and with ENB that killed the game with no
  crash log. `presentHook` `auto` now draws in the vtable hook behind a proxy, as
  0.30.1 did; only `"presentHook": "late"` still looks past one.
- **Pages cut off at low resolutions (fixed).** Below a device scale of 1,
  Ultralight drew only the top-left part of a page (scale squared of it: 64% at
  0.8), which hit screens under about 1067 pixels tall. `ViewDesc::uiScale` and
  `SetViewScale` now clamp to 1.0..3.0 (was 0.5..3.0) and log the raise; a page
  that wants to be smaller scales itself with a CSS transform.
- No API additions; the npm packages stay at 0.30.0.

## 0.30.2

Compatibility with mods that wrap the game's window or its frame output: a crash fix, and the
pages drawing behind upscalers and frame generation.

- **Crash with mods that subclass the game window (fixed).** Skyrim's window is
  ANSI, and many SKSE plugins subclass it with `SetWindowLongPtrA` and call the
  previous procedure directly. Magelight's input subclass is a Unicode one, so
  such a plugin got a handle it could only call through `CallWindowProc`, and a
  direct call crashed inside USER32 (an address like `0xFFFF...`). An ANSI shim
  now sits on top of the input subclass and hands every later subclass a real
  function.
- **Pages behind another Present hook.** Magelight now also hooks `Present1`,
  and when another plugin hooked Present first (an upscaler, a camera mod), it
  draws inside dxgi's own Present instead, after that plugin, so nothing draws
  over the pages. Behind a swapchain proxy it finds dxgi's own Present through a
  throwaway swapchain. It steps back to the plain hook when that Present is never
  reached. `Magelight.json` `"presentHook"`: `auto` (default), `late`, `vtable`.
- **Pages with Skyrim Upscaler and frame generation.** Skyrim Upscaler's HUD Fix
  keeps the game's UI apart from the scene and drops anything drawn at Present,
  so the pages opened (sound, paused game) but stayed invisible. Magelight can now
  draw the pages in the game's own UI pass: an invisible engine menu, open only
  while a page the player can click is visible, draws them into the UI's render
  target. When that pass does not run (loading screens, the console's `tm`), the
  pages are drawn at Present again within two frames. Click-through HUD pages
  still draw at Present, so they stay hidden behind the HUD Fix: the menu would
  keep Escape from opening the Journal for as long as one is up. `Magelight.json` `"composite"`: `auto`
  (default: the UI pass when Skyrim Upscaler is installed, never on VR),
  `present`, `ui`.
- **Diagnostics.** The log names the plugin whose Present hook Magelight found,
  the game swapchain (size, format, swap effect, flags), each swapchain presented
  and the plugin that presented it, and the UI-pass render target; every ten
  seconds it counts presents to swapchains other than the game's, while there are
  any.
- Third-party: MinHook (BSD-2-Clause), in `NOTICES.txt` and
  `THIRD_PARTY_LICENSES.txt`.
- No API changes; the npm packages stay at 0.30.0.

## 0.30.1

A fix for a hang on Skyrim VR, and the API call that lets a mod avoid the same one.

- **No more SKSE task posts from inside Present.** On Skyrim VR, SKSE also
  drains its task queue on game job threads and holds the queue's lock until
  the queue is empty. Magelight posted game-thread work straight to SKSE from
  inside the game's Present (GameThread events, EvalJS results, the Papyrus
  tier's events, sounds and cursor work) and from the main thread's input,
  window-message, engine-menu, SKSE-message and Papyrus handling. Such a post waited for that lock, and when the job
  being drained waited for the frame to finish, the game froze for good. It
  showed on the first load of a large save, when every view loads in the same
  frames as the save's post-load work. Those entry points now hold a scope,
  and every post made inside one goes to a queue that a host thread hands to
  SKSE, in order. Posts made elsewhere (inside SKSE tasks, from other threads)
  go straight to SKSE as before, so work a mod sequences
  after a UI-mode call keeps its order.
- **`PostGameTask(fn, user)`** (v4, gate 3001). The same handoff for mods: call
  it instead of SKSE's `AddTask` from a JS listener, a VR callback or, for a
  `RenderThread` mod, an event or EvalJS result.   Hang-safe from the host's
  callbacks; anywhere else it posts straight to SKSE, so a mod's own
  main-thread entry points still need their own handoff. A fault in `fn` is logged with its module
  and skipped. On an older host, post from a thread of your own. The threading
  contract in `MagelightUI_API.h`, [docs/CPP.md](docs/CPP.md) (with an example)
  and the C++ examples now say so.
- The cursor re-assert that follows a UI-mode entry no longer hides the
  cursor after an exit that landed first.
- **Stall watchdog.** The first stall sample of a session now logs every other
  thread's stack too (up to 64 threads, 24 frames each), so a hang names the
  thread holding things up, not only the one waiting. Each walk stays inside
  that thread's own stack.
- The npm packages stay at 0.30.0: the page contract did not change.

## 0.30.0

Three breaking changes to the sandbox, made before the first public release
because tightening them later would break mods that had come to rely on
them. No fields reordered; `NetworkPolicy` gains a value and `Magelight.psc`
one native.

- **Changed: the release is three plain zips, not a FOMOD.** The main
  download is the runtime alone, in the game's Data layout, so a mod
  manager installs it with no options to get wrong; the Web Inspector
  and the host's dev pages ship as `MagelightUI-<ver>-DevTools.zip` and
  the example mods as `MagelightUI-<ver>-Examples.zip`, both for mod
  authors. `tools/package.ps1` replaces `package_fomod.ps1`, and the
  `fomod/` folder is gone.
- **Breaking: network access is file-only by default.** A page reads files
  under its own mod folder and the host's runtime dir and reaches nothing
  over the network. Before 0.30.0 every page could fetch `http(s)` on
  loopback (`localhost`, `127.x.x.x`, `[::1]`) with no opt-in, so a
  data-only manifest mod could talk to any service running on the machine.
  A mod that relied on its own local server now opts in, per mod:
  - C++: `SetNetworkPolicy(mod, NetworkPolicy::LoopbackOnly)` for loopback,
    `NetworkPolicy::Any` for any host and scheme (unchanged), and the new
    `NetworkPolicy::FileOnly` for the default. The enum is now
    `{ LoopbackOnly = 0, Any = 1, FileOnly = 2 }`; the existing values keep
    their numbers. Gate `FileOnly` on `hostVersionNumber >= 3000`.
  - Manifest: a top-level `"network": "file"` (the default) or
    `"loopback"`. `"any"` is refused with a log line: internet reach stays a
    plugin's call.
  - Papyrus: `bool Magelight.SetNetworkPolicy(string modId, string policy)`
    takes `"file"` or `"loopback"` (any case) for a script-owned mod;
    `"any"`, an unknown word or a plugin-owned mod returns false.

  Each change is logged once per mod. Views created through the v1-v3
  `CreateView` belong to no mod: they are file-only now and cannot opt in.
  A page that needs loopback or `Any` must be created with `CreateViewEx`
  under a mod registered through v4 `RegisterMod`.
- **Breaking: the Papyrus tier acts only on script-owned mods.** A mod
  registered through the C++ API (`RegisterMod`/`RegisterModEx`, including
  a manifest mod a DLL adopted) is plugin-owned; a mod registered by
  Papyrus `RegisterMod`/`CreateView`, or declared by a manifest no DLL
  adopted, is script-owned. Papyrus `RegisterMod`, `CreateView`,
  `ReleaseUIMode` and `SetNetworkPolicy` refuse a plugin-owned slug (false
  or 0, one log line), and `DestroyView`, `ShowView`, `RequestUIMode`,
  `ReloadView`, `Call`, `Eval`, `EvalAsync`, `BindHotkey`, `SetCutout`,
  `SetHibernate`, `RegisterListener` and `PlaySound` do nothing (false or 0)
  on a plugin-owned mod's view. `FindView`, `IsVisible`, `IsUIModeActive`,
  `GetUIModeView`, `GetLastError`, `GetVersion`/`GetVersionString` and
  `IsReady` still answer for any mod. Before 0.30.0 a script could reuse a
  plugin's slug and create views under it (inheriting its network reach and
  storage), and could drive, `Eval` into and read the storage of any
  plugin's views. Among script-owned mods nothing changes: Papyrus cannot
  tell scripts apart, so any script still drives any script-owned mod's
  views, and their page storage is only as private as the script tier.

  Storage jars are shared by name, so the same refusals cover a
  script-owned mod whose jar a plugin uses: a manifest mod with
  `"session": "default"` (the jar of every v1-v3 view), a mod whose
  modId equals a plugin's custom `sessionName`, or a mod whose jar a plugin
  used in an earlier session: the host records every jar a plugin uses (its
  modId or its custom session name) in `Magelight-cache\plugin jars.txt`
  and keeps scripts out of those jars in later sessions, even with that
  plugin uninstalled, since the jar's data stays on disk. A manifest whose
  modId equals a plugin's custom session name is refused at load, and a plugin
  registering a session a script-owned mod already uses is warned in the
  log. A relative `CreateView` path must now exist under
  `Data/Magelight/<modId>/` (it used to fall back to the host's runtime
  folder). Each refusal is logged once per function and target, not once
  per function.
- **Breaking: a manifest's `modId` must equal its folder name** (letter
  case aside). A manifest naming another `modId` is not loaded, and the log
  names its path; leave `modId` out or give the folder's name. A folder
  whose name is not a valid slug (`[A-Za-z0-9_.-]`, 1..32 characters;
  `My HUD`, say) can no longer be rescued with an explicit `modId`: rename
  the folder. Before 0.30.0 a manifest in any folder could claim any slug,
  and a plugin registering that slug later adopted its views. A manifest
  now claims only its own folder's slug; the folder is the identity (see
  docs/MANIFEST.md).
- **Fixed (security): a page acts only as its own view.** The native
  bridge took the view id from the page's own argument, so a page could
  call `__mlNative('<another view id>', ...)` and fire that view's JS
  listeners (which then ran with that view's id), play sounds or change
  its Escape capture as it, and send keys through the VR keyboard's
  channel into whichever page held UI mode. A script could reach any
  plugin listener this way by evaluating that call in a page of its own.
  The bridge function now carries its page's view id and refuses a call
  made as any other view; only the host's keyboard page may send keys.
- **Fixed (security): the inspector is sandboxed like a page.** The
  DevTools view `ShowInspector` opens had no request filter, so it could
  read every file the host serves (its runtime folder, every mod folder
  under Data\Magelight and every registered page folder) and reach any
  host. It now reads only the host's runtime
  folder (where its own files are) and the inspected page's folder, and
  reaches nothing over the network.
- **Fixed: a view no longer keeps a stale network level** when
  `SetNetworkPolicy` runs while another thread creates a view of the same
  mod (Papyrus calls now run on parallel VM threads).
- **Fixed: a listener registered after the page loaded no longer replaces
  `window.magelight`.** A late `RegisterJSListener`/`RegisterJSListenerEx`
  or Papyrus `RegisterListener` (from a DOM-ready handler, or on every game
  load) re-ran the whole page bridge, which built a new `window.magelight`.
  `@magelight/sdk` and every `magelight.on` subscription stayed on the old
  object, so `useChannel`, `useUIMode` and host events stopped arriving from
  then on. The bridge core and its document listeners now install once per
  page; a later registration re-creates `__mlNative` and every listener's
  `window.<name>` shim, and keeps `window.magelight` and its subscriptions.
- **Fixed: `data-ml-sound` click sounds no longer repeat.** The same re-run
  added the click delegation again, so each late registration made a click
  play once more.
- **Changed: `magelight` and listener names starting `__` are the
  host's.** A listener under such a name replaced part of the host's page
  script (`window.magelight`, `__mlNative`, the reserved channels) and broke
  the page. `RegisterJSListenerEx` now returns `InvalidArgument` for them
  (`GetLastErrorMessage` says why), the v1-v3 `RegisterJSListener` drops
  them, and Papyrus `RegisterListener` returns false; the log names each
  refused name once.
- **Changed: example and doc hotkeys moved off the game's own keys and off
  each other.** F5 is Skyrim's Quicksave, F9 its Quickload and F12 Steam's
  screenshot key, and a HUD toggle leaves the key press to the game too.
  `Magelight.Badge`: badge F6, panel F7. `Magelight.ReactConfig` stays F8
  and the CppPanel example F13. The README Quick Start uses F11, the
  `create-magelight-view` template F10 and the docs/PAPYRUS.md minimal
  example Home (DIK 199). PageUp (DIK 201) is the host's default toggle key
  and is refused as a mod hotkey.
- **Fixed: a `<label>` wrapping its control plays its `data-ml-sound`
  once.** The label re-dispatches the click on the control, which played
  the sound a second time.
- **npm packages (0.30.0).** `@magelight/sdk`'s ESM import specifiers carry
  `.js`, so it loads under native Node ESM and `moduleResolution: nodenext`.
  `host.sound` no longer falls back to `send('__sound')` on hosts before
  0.29.0 (they have no such channel and logged a warning per call).
  `@magelight/react`'s `HostGate` installs the browser mock before its first
  check, so a page in a plain browser renders instead of the fallback. The
  `create-magelight-view` template dedupes React, so a `--local` scaffold
  no longer loads two copies and renders blank.
- **Packaging.** The standalone zip writes
  `SKSE/Plugins/Magelight/VERSION.txt`, the stamp SeverActions writes into
  its bundled copy. The host's test pages (`views/app`, `views/probe`,
  `views/*.html`, used only by the `demoViews` and `imageProbe` settings)
  moved from the required core to the Developer Tools group.
  `THIRD_PARTY_LICENSES.txt` is also at the root of the zip. `build.ps1`
  refuses an Ultralight SDK other than the build of record
  (`1.4.0b.081c48b`).
- **Example: `Magelight.InspectTarget`.** Its native is now
  `bool Open(Actor)`, which fills, shows and focuses the plugin-owned view
  (it was `Fill`, with the script showing the view, which 0.30.0 refuses).
- **Docs.** Papyrus ModEvent handlers take the standard four arguments
  (`string eventName, string strArg, float numArg, Form sender`); the
  two-argument form documented before does not match what the host sends.
  The host keeps mods, views, listeners and hotkeys in memory only, so a
  Papyrus mod registers again on every game load and finds its views with
  `FindView` rather than a saved id. JS listener callbacks
  (`RegisterJSListener`/`RegisterJSListenerEx`) run on the render thread,
  not on the mod's callback thread: copy the argument and `AddTask` before
  touching game state.

## 0.29.0

Two functions appended to the v4 table; one new capability.

- **Runs on Skyrim 1.7.99 / 1.7.104.** The CommonLib pin moved to MinLL/CommonLibVR
  4.39.5, which reads the format-5 address library the 1.7.99+ runtimes ship; earlier
  builds could not load on them. No API change; the VR build is unaffected.
- **UI sounds.** Ultralight has no media stack — `<audio>` and Web
  Audio do nothing — so the host now plays through the game's
  own audio (`BSAudioManager`, game thread), which also respects the player's
  UI volume. Names are the vanilla `UIMenu*` descriptors — `ok`/`click`,
  `cancel`, `prevnext`, `focus`/`hover`, `open`, `close`, `inactive`, resolved
  by FormID (VR has no reliable EditorIDs) — or `Plugin.esp|0xFormID` for any
  SNDR a mod ships — and every vanilla `UI*`/`ITM*` descriptor by EditorID
  (`UIJournalOpen`, `UISelectOn`, `ITMGoldUpSD`: 134 names from a generated
  table, so a page can pick the exact sound the vanilla menu uses there);
  `none` opts an element out. An unknown name logs once and stays silent. Four ways in:
  the markup convention `data-ml-sound="click"` / `data-ml-sound-hover="focus"`
  (the injected bridge delegates both; hover throttled to one per ~60 ms per
  view, page-driven sounds only while the view is visible); the page verb
  `magelight.sound(name)` (SDK `host.sound`, React `useSound()`); the C++
  `PlayUISound(view, name)` and Papyrus `Magelight.PlaySound(view, name)`; and
  per-view open/close sounds on UI-mode enter/exit — `SetViewSounds(view, open,
  close)` or manifest `"sounds": { "open": "open", "close": "close" }`, off
  unless asked for. `QueryCapability("sound")`. No file playback by design
  (SECURITY.md).
- **Flat cursor a little smaller.** Default `cursorHeight` 44 → 36 px at 1080p (still scales
  with resolution; `Magelight.json` overrides as before). VR's `vr.cursorScale` is unchanged.
- **Only visible views are rendered.** The present loop calls `Renderer::RenderOnly`
  with the visible views instead of `Render()`, which painted every dirty page —
  a hidden popup with a spinner or a closed dashboard with a blinking caret was
  repainted every frame whenever any other view kept the loop rendering. Hidden
  pages keep their dirty flag and paint on their next Show.
- **VR: the built-in B/Y exit yields to a mod that bound B/Y.** While a view is
  in UI mode, B/Y on either controller leaves UI mode (0.17.3). When the view's
  own mod has bound B/Y as a VR hotkey (bare or chorded, `BindVRHotkey` /
  `BindVRHotkeyCallback` / manifest `vrHotkey`) the built-in exit now stands
  down and the binding alone decides — before, one Grip+B/Y press ran both, and
  whichever game-thread task landed first decided whether the menu closed or
  closed-and-reopened 12 ms later (SeverActions, Skyrim VR field log
  2026-09-12: the menu could not be dismissed). Views whose mod did not bind B/Y
  keep the built-in way out. New internal `VR::ViewBindsButton`; no ABI change.
- **Removed: the PrismaUI compatibility surface.** The `PrismaVR_DeliverChar`
  / `PrismaVR_DeliverVKey` DLL exports are gone (OpenComposite's keyboard
  reaches a focused page through the window message instead);
  `Magelight_DeliverChar` / `Magelight_DeliverVKey` remain. The
  `window.__prismaNativeImeFocusChanged` page shim is gone too: the bridge
  detects text-field focus by itself, and a page that still calls the
  global unguarded now throws.

## 0.28.2

One function appended to the v4 table.

- **Per-mod network policy.** `SetNetworkPolicy(mod, NetworkPolicy::Any)` lets a
  mod's pages reach any host over any scheme — the reach a page has in a browser —
  for that mod only; every other mod keeps file + loopback. Set
  it once after `RegisterMod`; it covers views created before and after, and the
  log names the mod that opted in. `QueryCapability("networkpolicy")`. Added for
  SkyrimNet's dashboard (model catalogs on openrouter.ai, presets and plugin
  indexes on raw.githubusercontent.com, whatever the user set as provider).

## 0.28.0

The host-side prerequisites for running SkyrimNet's in-game dashboard on
Magelight. No fields reordered; six functions appended to the v4 table.

- **Loopback allowed.** Pages may fetch `http(s)://127.0.0.1`, `localhost`
  and `[::1]` (a mod's own local server); every other network request stays
  refused. `QueryCapability("loopback")`.
- **Escape ownership.** `SetEscapeCapture(view, true)` (or, from a page,
  `magelight.send('__escapecapture','1')`) delivers Escape to the page as a
  key instead of leaving UI mode — a modal or an editing control closes
  itself. Honoured on both Escape paths (window proc on flat, the input sink
  on VR).
- **Inspector API.** `ShowInspector(view, show)` / `IsInspectorVisible`.
- **View order.** `SetViewOrder` / `GetViewOrder` (explicit z within the
  layer; `RaiseView` still means "above everything").
  `QueryCapability("vieworder")`.
- **Scroll step.** `SetScrollStep(view, px)` — pixels per wheel notch.
  `QueryCapability("scrollstep")`.
- **Keyboard mute drains DirectInput.** While a page has focus on flat, the
  keyboard mute now drains the device's buffered queue each poll instead of
  skipping the poll — keys typed into a page no longer replay into the game
  (journal, map) the moment the menu closes.
- Console messages need no new API: they already arrive as the
  `ConsoleMessage` host event (level, line, text).


## 0.27.0

- **Native IME (CJK input).** The window's
  IME context attaches only while a text field in the UI-mode view has focus
  (the bridge detects focus, so pages need no code) and detaches otherwise,
  so gameplay keys never raise a composition. Composition text is drawn
  inline in the field (it rides as the selection; the commit arrives as
  ordinary characters and replaces it, a cancel deletes it). The OS candidate
  list opens at the caret the page reports. Keys the IME consumed
  (VK_PROCESSKEY) are swallowed before the page sees them, so Enter, Space
  and Escape mid-composition go to the IME, not to your handlers. Characters
  outside the BMP arrive as one event instead of two broken halves.
  `magelight:ime` window event `{state, text}` for
  indicators; `QueryCapability("ime")`. Flat only.


## 0.26.12

- **VR: the laser-end pointer is a point.** A small white disc with a dark
  rim replaces the 32px arrow sprite at the end of the laser, drawn at
  `vr.cursorScale` x the view height (default now 0.012, was 0.05; floor
  0.004) with a centred hotspot. `"cursorDot": false` in the `vr` block
  brings the arrow (or your custom cursor PNG) back. The desktop cursor is
  unchanged.


## 0.26.11

- **RequestUIMode retargets the pause flag on a same-view re-request, in
  place.** A mod that already holds UI mode on a view can re-request it with
  or without `kUIModeFlagPause` to unpause / pause the game without leaving
  the mode: the live focus menu's `kPausesGame` and the engine's pause
  counter are adjusted directly. Cursor, controls and text entry are
  untouched, and no menu is hidden or re-shown — 0.26.10 did it by hide +
  show, and a holder's close detection read that as the engine taking its
  menu away (SeverActions auto-closed on the spot); withdrawn. Before 0.26.10
  the re-request returned Ok and silently kept the old pause. Internal
  `SetUIModePause`.

## 0.26.9

- **Real device scale.** `ViewDesc::uiScale` is Ultralight's device scale
  (the page's `devicePixelRatio`), and `SetViewScale(view, scale)` changes
  it on a live view; the render thread applies it. Clamped 0.5..3.0; cutout
  and image rects are in view pixels (CSS px x scale). Gate on
  `hostVersionNumber >= 2609`.

## 0.26.8

- **`FindView(mod, name, &id)`** appended to the v4 table — the C++ twin of
  the Papyrus FindView, for driving a manifest-declared view from a DLL.
  Gate on `hostVersionNumber >= 2608`.
- Manifest robustness (a mistyped scalar warns and falls back instead of
  aborting the file; unknown keys are logged with a did-you-mean) and the
  `MagelightUI_Mod.h` header-only wrapper.

## Runtime / packaging (no ABI change)

- **0.28.7** — vcpkg baseline moved from 2022-12-20 to 2025-12-12 (`ee12231b`, the commit upstream CommonLibSSE-NG 4.17.0 builds against): fmt 9.1.0 → 12.1.0, spdlog 1.11 → 1.16, DirectXTK/DirectXTex 2022 → 2025-10-27, xbyak 6.60 → 7.28, nlohmann-json 3.11 → 3.12. fmt 9.1.0 no longer compiles on MSVC 14.51 (VS 18's STL removed `stdext::checked_array_iterator`), which broke CI and would have broken the next local Visual Studio update; the overlay port now carries upstream's dependency floors so the library can't be paired with 2022 dependencies again. No source changes were needed; CI builds on the VS 18 image again.
- **0.28.6** — trust-boundary hardening (no ABI change). Slug validation refuses dot-only values, a trailing dot and the Windows device basenames (CON, PRN, AUX, NUL, COM1-9, LPT1-9) — a manifest-only mod could point its session storage outside the cache root. Papyrus `CreateView`: an absolute `htmlPath` must resolve inside the game root; a non-absolute one must be genuinely relative (no root name or root directory — a root-relative or UNC path replaces the base on join) and free of `..`, and the probed file must still sit inside `Data\Magelight\<modId>\`; the C++ tier keeps unrestricted absolute paths. Mod-supplied view and listener names are JSON-quoted before landing in generated page script. The trust model is documented (`docs/CPP.md`, `api/MagelightUI_API.h`): views are addressable, not owned.
- **0.28.5** — review fixes: the stall watchdog's heartbeat is stamped at the present hook's entry, so loading screens and menus (which present from a foreign thread) no longer read as stalls and burn the sample budget; the watchdog captures the thread context under suspension and walks the stack only after `ResumeThread` (the walk takes ntdll function-table locks a suspended thread may hold); `SetViewScale` is applied by the render thread instead of re-laying out WebKit on the caller's thread; the loopback allow-list accepts `127.x.x.x` as a dotted quad only (a `127.` hostname prefix passed); the 0.26.11 pause retarget does its `RE::UI` work on the game thread; the engine `Cancel` path honours `SetEscapeCapture`; the 0.28.0 view setters share the v4-view gate and record a `GetLastErrorMessage` on refusal; `GetViewInfo` returns the live `uiScale`; `host.caps` gains `networkPolicy` and the header/mock lists are synced; manifest `vr`/`vrHotkey` blocks use the non-throwing reads; `MagelightUI_Mod.h` compiles (`DomReadyFn`) and its snippet names a real key (F13, 0x64).

- **0.28.4** — a hibernated view keeps its controller binding. Hibernation (a view hidden past its `SetViewHibernate` budget) released the overlay through the same path a destroy takes, and that path also dropped the view's VR hotkey — so a popup bound to a chord went deaf 30 s after it was last hidden and only answered again after a rebind (SeverActions' quick wheel: worked once per session or per rebind, then never). Only a destroy unbinds now; the chord fires on the dormant view and wakes it.
- **0.28.3** — stall watchdog: when no frame has been presented for 1.5 s while the world is live, the thread that presented the last frame (and the main thread, if different) is suspended, its stack walked with the OS unwinder, resumed, and the frames logged as `module+offset` — a process dump's headline, captured automatically, for the freezes and soft stalls testers cannot otherwise document. One sample per 5 s, 12 per session; `Magelight.json` `"stallWatchdog": false` disables, `"stallThresholdMs"` tunes.
- **0.28.1** — a page living OUTSIDE `Data\Magelight` (an absolute path, e.g. a dashboard shipped under another mod's own folder) now actually loads: the per-view pin already granted its own folder, but the view-blind file-system layer beneath it only served `Data\Magelight` and refused every read, so the view came up empty. That layer now serves the union of registered page roots.
- **0.26.7** — review fixes: VR chord latch/debounce/seed correctness, laser hit-test clamp, thread-safe `GetPlacement`, `SetViewVRPlacement(nullptr)` restores the layer default, `EvalJS` context no longer leaks on view destroy.
- **0.26.5** — `Magelight.json` `fontHinting` / `fontGamma` (text rasterization tuning).
- **0.26.x** — `tools/package_fomod.ps1 -Tester` (runtime-only zip); the probe page moved behind `-Examples`.

For the full commit history see the git log; the API contract itself lives in `api/MagelightUI_API.h`.
