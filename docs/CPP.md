# Magelight UI — the C++ tier

The C++ tier is for an SKSE plugin that needs more than a manifest or Papyrus
can do: per-frame data into a page, texture images (render an actor into the
UI), custom JS listeners, owning UI mode, VR controller bindings. If a static
HUD or a hotkey-toggled config page is all you need, use the
[manifest tier](MANIFEST.md) — no DLL required.

There are two headers, both in `api/`, both header-only (no import library —
the host is reached by `GetModuleHandle`/`GetProcAddress`):

- **`MagelightUI_API.h`** — the raw C ABI. One struct of function pointers per
  API version; you acquire it and call the vtable. This is the whole contract.
- **`MagelightUI_Mod.h`** — an *optional* convenience wrapper over it. Owns
  `RegisterMod`/`UnregisterMod`, pre-fills the `size` fields, and turns the one
  `onEvent` callback into per-view `onClose`/`onError` slots. Use it to skip
  ~50 lines of boilerplate; ignore it if you want zero abstraction.

Vendor both headers into your plugin (copy them in, or add `api/` to your
include path). They depend only on the standard library and, for the raw
header, `<d3d11.h>` forward decls. Include CommonLib (or your precompiled
header) first: the raw header includes `<windows.h>`, and CommonLibVR stops
with "Windows API detected" when that comes before it.

## Minimum viable plugin

This is a complete SKSE plugin that puts a page on screen and logs when the
user closes it. It uses the wrapper; the raw-header equivalent is in the
comments.

```cpp
#include <SKSE/SKSE.h>               // CommonLib first (see above)
#include "MagelightUI_Mod.h"        // pulls in MagelightUI_API.h
#include <filesystem>

static MAGELIGHT_API::MagelightMod g_mod;
static MAGELIGHT_API::ViewId       g_view = 0;

// Your pages live under Data\Magelight\<modId>\...  — resolve that from the
// running exe, NOT a relative path (the host loads an absolute file:/// URL).
static std::string PagePath()
{
    wchar_t buf[MAX_PATH]{};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    auto p = std::filesystem::path(buf).parent_path()
           / L"Data" / L"Magelight" / L"MyMod" / L"views" / L"panel" / L"index.html";
    return p.string();
}

static void OnMessage(SKSE::MessagingInterface::Message* m)
{
    if (m->type != SKSE::MessagingInterface::kDataLoaded) return;

    // acquire: false = Magelight absent or older than 0.16.0 (it logs why).
    if (!g_mod.acquire("MyMod", "My Mod", 0, 16, 0)) {
        SKSE::log::warn("MyMod: Magelight UI 0.16.0+ not present — UI disabled");
        return;
    }
    const std::string page = PagePath();
    g_view = g_mod.createView("panel", page.c_str(),
                              MAGELIGHT_API::Layer::Panel,
                              { .fullscreen = true });     // a self-scaling page
    if (!g_view) {
        SKSE::log::error("MyMod: view refused — {}", g_mod.v4()->GetLastErrorMessage(g_mod.id()));
        return;
    }
    g_mod.onClose(g_view, [](MAGELIGHT_API::ViewId) {
        SKSE::log::info("MyMod: the user closed the panel");
    });

    // Open it with F13 (DirectInput scancode 0x64); a page in the Panel layer
    // takes UI mode (cursor + suspended game controls) when its key fires.
    g_mod.v4()->BindHotkey(g_view, 0x64, MAGELIGHT_API::kHotkeyActionToggleUIMode);
}

extern "C" __declspec(dllexport) bool SKSEPlugin_Load(const SKSE::LoadInterface* skse)
{
    SKSE::Init(skse);
    SKSE::GetMessagingInterface()->RegisterListener(OnMessage);
    return true;
}
```

The page (`Data\Magelight\MyMod\views\panel\index.html`) is any HTML. To talk
to the plugin, the page uses `window.magelight.send(name, arg)` and
`window.magelight.on(name, fn)`; on the C++ side you push with
`v4()->InvokeJS(view, "window.foo(...)")` or the typed `EvalJS`, and receive
with `v4()->RegisterJSListenerEx(view, "name", cb, user)`.

## The rules that bite

- **Threads.** Only your `onEvent` (and the wrapper's `onClose`/`onError`)
  and `EvalJS` results arrive on the callback thread you pass to `acquire` —
  default `GameThread`, the only thread where `RE::` engine state is safe.
  JS listeners (`RegisterJSListenerEx`, and v1 `RegisterJSListener`) are called
  inline on the **render thread**, mid-frame, whatever thread you chose: copy
  the argument (it is valid only during the call) and post the work with
  `v4()->PostGameTask(fn, user)` (host 0.30.1+) before you touch anything
  `RE::`, never SKSE's `AddTask`: these callbacks run inside the game's Present,
  where on Skyrim VR an `AddTask` can hang the game (the threading contract in
  `MagelightUI_API.h` says why). Magelight's own functions are safe to call from
  there; every one is callable from any thread. VR button callbacks
  (`BindVRHotkeyCallback`, `SetVRButtonListener`) run on the **present thread**;
  post from them the same way. `fn` is a plain function pointer, so box the copy:

  ```cpp
  void OnClick(MAGELIGHT_API::ViewId, const char* arg, void*)
  {
      auto* copy = new std::string(arg ? arg : "");
      auto run = [](void* user) {
          std::unique_ptr<std::string> text(static_cast<std::string*>(user));
          // RE:: is safe here
      };
      if (v4()->hostVersionNumber >= 3001) {
          v4()->PostGameTask(run, copy);
      } else {   // an older host: post from a thread of your own
          std::thread([run, copy]() { SKSE::GetTaskInterface()->AddTask([run, copy]() { run(copy); }); }).detach();
      }
  }
  ```
- **Versioning.** Anything added after your `minHost` must be gated:
  `if (v4()->hostVersionNumber >= 2100) v4()->BindVRHotkeyCallback(...)`, or
  `if (v4()->QueryCapability("vr")) ...`. The packed number is
  `MAJOR*10000 + MINOR*100 + PATCH`. See [CHANGELOG.md](../CHANGELOG.md) for
  which version added which call.
- **Absolute page paths.** The host loads a `file:///` URL and pins reads to
  your `Data\Magelight\<modId>\` folder, or, for a page kept elsewhere (a page
  laid out under another UI mod's own folder, e.g. `Data\<OtherUI>\views\...`),
  to the page's own directory (0.26.4+; pages outside `Data\Magelight` load as
  of 0.28.1).
  Resolve the path from `GetModuleFileNameW`, not the working directory.
- **Network.** Since 0.30.0 every mod's pages are file-only by default
  (`NetworkPolicy::FileOnly`): they read their own files (see the previous
  point) and the host's runtime folder, and nothing over the network. Opt in with
  `SetNetworkPolicy(mod, policy)` once after `RegisterMod`:
  `NetworkPolicy::LoopbackOnly` adds `http(s)` to this machine (`localhost`,
  `127.x`, `[::1]`), for your own local server or a companion app;
  `NetworkPolicy::Any` reaches any host with any scheme (the internet:
  catalogs, indexes, a user-set endpoint). It applies to all of that mod's
  views and each change is logged. (`FileOnly` as a value is new in 0.30.0;
  an older host reads it as `LoopbackOnly`.) Before 0.30.0 the default was
  `LoopbackOnly`, so a mod that relied on reaching a local server must now
  call `SetNetworkPolicy(mod, NetworkPolicy::LoopbackOnly)`; the call exists
  since 0.28.2 (`hostVersionNumber >= 2802`), and on 0.28.2 to 0.29.x it only
  restates the old default. Views created through the v1-v3 `CreateView`
  belong to no mod: they are file-only and cannot opt in. A page that needs
  `LoopbackOnly` or `Any` must be created with `CreateViewEx` under a
  registered mod.
- **Register early.** Call `RegisterMod` at `kDataLoaded` at the latest, before
  any script runs. A script that registers your slug first, or touches the
  manifest mod you ship under it, owns that mod for the session, and your
  `RegisterMod` then returns `InvalidMod`.
- **One UI-mode owner.** Only one view holds UI mode (cursor + input) at a
  time; `RequestUIMode` returns `Busy` (or queues with `kUIModeFlagQueue`) if
  someone else holds it. A HUD/click-through view never takes UI mode.

## Trust model: views are addressable, not owned

The C ABI carries no caller identity. Any plugin that knows a view id can
call `ShowView`, `Navigate`, `DestroyView`, `EvalJS` and the
listener-registration functions on it — including another mod's ids. That
is deliberate: the Skyrim modding ecosystem is cooperative and cross-mod UI
coordination is a feature (see `FindView`). But it is **not** isolation.
What the host does isolate per mod: file reads (a page reads only its own
`Data\Magelight\<modId>\` folder or its own directory — the log names the
first 32 refused reads), storage (per-mod sessions), network (file-only unless
the mod opts in with `SetNetworkPolicy`), hotkeys (first registrant wins), and
page calls (since 0.30.0 a page's `window.magelight.send` reaches only the
listeners of the view it is in; it cannot call another view's). What it does
not isolate between plugins: view control and JS execution inside an
already-created view. The etiquette follows: drive only your own views, and
coordinate with other mods through the channels they document — never by
reaching into their pages.

Storage jars are shared by name. The default `sessionName` (isolated) gives
your mod a jar named after its `modId`, which only your mod uses. A custom
`sessionName` is shared with every mod that names it and with any mod whose
`modId` is that name; `"default"` is the jar all v1-v3 views use. Scripts
cannot drive a mod whose jar a plugin uses, but another mod's pages can still
run in it. If your storage must stay out of other mods' reach, keep the
isolated default.

Scripts get less reach (0.30.0). A mod registered through this API
(`RegisterMod`/`RegisterModEx`, including a manifest mod your DLL adopted) is
**plugin-owned**: Papyrus `RegisterMod`, `CreateView`, `ReleaseUIMode` and
`SetNetworkPolicy` refuse its slug, and every Papyrus call that acts on its
views (`ShowView`, `RequestUIMode`, `Call`, `Eval`, `EvalAsync`,
`RegisterListener`, `BindHotkey`, …) does nothing and returns false. Scripts
can still read: `FindView`, `IsVisible`, `IsUIModeActive`, `GetUIModeView`,
`GetLastError`. The same refusals cover a script-tier mod whose storage jar
is the default one or a plugin's session name. If scripts should open your
panel, register a Papyrus native
of your own that does it (`examples/Magelight.InspectTarget`). See
[PAPYRUS.md](PAPYRUS.md#which-mods-a-script-can-drive).

## Adopting a manifest mod from C++

If you also ship a `manifest.json` under the same `modId`, your DLL adopts it:
call `RegisterMod` with that id and you can drive its declared views. Resolve
each view's id from its name with `v4()->FindView(id(), "viewName", &out)`
(0.26.8+). This lets a designer lay out views in the manifest and the DLL fill
them with live data. Adopting makes the mod plugin-owned, so scripts can no
longer drive its views (see the trust model above).

## IME (Chinese, Japanese, Korean input)

Nothing to do. When a text field in your page takes focus, the host attaches
the window's IME context; the OS candidate list opens at the caret; the
composition text shows inline in the field; the committed text arrives as
ordinary keystrokes. While a composition is open, Enter, Space and Escape
belong to the IME and your key handlers do not see them. For an "IME active"
indicator, listen for the
`magelight:ime` window event (`detail.state` is `start`, `compose`, `commit`
or `end`). Feature-detect with `QueryCapability("ime")`. Not available in VR,
where the runtime keyboard owns text entry.

## Escape, loopback, and the other 0.28.0 hooks

- **Escape.** By default Escape leaves UI mode. When a modal or an editing
  control should handle it instead, call `SetEscapeCapture(view, true)` (or
  from the page: `magelight.send('__escapecapture','1')`); Escape then
  arrives as an ordinary keydown/keyup and UI mode stays. Clear it when the
  control closes. Feature-detect with `QueryCapability("escapecapture")`.
  The host's toggle key (PageUp unless `Magelight.json` sets `toggleKey`)
  always leaves UI mode, captured or not.
- **Local servers.** Once the mod has called
  `SetNetworkPolicy(mod, NetworkPolicy::LoopbackOnly)` (required since
  0.30.0, see Network above), a page may `fetch()`
  `http://127.0.0.1:<port>` (or `localhost` / `[::1]`) — your own server on
  this machine. Anything else is refused. `QueryCapability("loopback")`.
- **Inspector, z-order, scroll step.** `ShowInspector`, `SetViewOrder`,
  `SetScrollStep` — see the header (`QueryCapability("inspector")`,
  `"vieworder"`, `"scrollstep"`). Page console output reaches you as the
  `ConsoleMessage` host event; there is no separate console callback. The
  event carries every message in full; `Magelight.log` records only what the
  player's `consoleLog` allows (0.31.0: warnings and errors by default, cut at
  2 KB, 20 lines a second per view). `QueryCapability("consolelog")` answers
  that level (0 none, 1 errors, 2 warnings, 3 all); forward your own lines
  from the event when you need them and it is below 3. Keep secrets out of a
  page's console output either way.

## UI sounds (0.29.0)

Ultralight has no media stack — `<audio>` and Web Audio do nothing — so the
host plays through the game's own audio, which also respects the player's UI
volume. Three ways in, all the same names:

- **Markup, no code.** `data-ml-sound="click"` on any element plays on click
  (keyboard activation too); `data-ml-sound-hover="focus"` plays on entering
  the element. The injected bridge wires both up; hover is throttled host-side
  (one per ~60 ms per view). Page-driven sounds play only while the view is
  visible.
- **Page script.** `magelight.sound('cancel')` (SDK: `host.sound`, React:
  `useSound()`).
- **C++ / Papyrus.** `PlayUISound(view, "cancel")` on a refused action, no page
  round-trip; `Magelight.PlaySound(view, name)`. Gate on `hostVersionNumber
  >= 2900` or `QueryCapability("sound")`.

Names: `ok`/`click`, `cancel`, `prevnext`, `focus`/`hover`, `open`, `close`,
`inactive` — the vanilla `UIMenu*` descriptors, resolved by FormID (EditorIDs
are unreliable at runtime on VR); **any vanilla `UI*` / `ITM*` descriptor by its
EditorID** (`UIJournalOpen`, `UISelectOn`, `ITMGoldUpSD`, `ITMBookOpenSD` — all
134, case-insensitive, from a generated FormID table); `none` for explicit
silence; or `Plugin.esp|0xFormID` for any SNDR your mod ships. An unknown name logs once and stays silent. `SetViewSounds(view,
"open", "close")` (manifest `"sounds"`) makes a menu sound like a menu on
UI-mode enter/exit with zero page code; it is off unless you ask. There is
deliberately no file playback: a loose `.wav` path would be a new attack
surface (SECURITY.md) and would bypass the audio settings.

## Device scale and the pause retarget

- **Real DPI (0.26.9).** `ViewDesc::uiScale` is Ultralight's device scale:
  the page's `devicePixelRatio`, rasterised at that scale (0 = the host
  default). `SetViewScale(view, scale)` changes it on a live view — the
  render thread applies it on its next frame. Clamped 1.0..3.0 since 0.30.3
  (below 1 Ultralight clipped the page to scale squared of the view; shrink a
  page with a CSS transform instead). Cutout and
  image rects are in VIEW pixels = CSS px x scale. `GetViewInfo` reads it
  back. Gate on `hostVersionNumber >= 2609`.
- **Pause retarget (0.26.11).** Re-requesting UI mode on the view that
  already holds it re-targets `kUIModeFlagPause` in place: the live focus
  menu's pause flag and the engine's pause counter move on the game thread,
  nothing is hidden or re-shown, so your close detection never fires. Gate
  on `hostVersionNumber >= 2611`.

## Freezing the world behind a fullscreen page (0.31.0)

`SetViewFreezeWorld(view, true)` asks the game to skip its 3D world render
while `view` holds UI mode with `kUIModeFlagPause`: the engine shows a frozen
frame instead (expected from the Journal's freeze flag; the flag alone is
untested in game as of 0.31.0, and the saving is still to be measured).
It is a per-view preference, callable from any thread and kept until you
change it or destroy the view; the host applies it on the game thread.

- **Pause is required.** A freeze over a running game hangs it, so the host
  sets the freeze only while its own menu holds the pause, and drops it
  before the pause whenever the view is unpaused in place, another view takes
  UI mode, UI mode closes or a load starts. If another mod lets the game run
  under it, the host drops it within a fraction of a second.
- **Opaque pages only.** The HUD is expected to keep drawing under the page,
  and anything translucent shows the frozen frame, not the live world.
- **Flat only.** Untested on VR, so excluded as a precaution (the frozen frame
  may never be drawn in the headset): `QueryCapability("freezeworld")` answers 0 and the call returns
  `Unsupported`. The player can turn it off for every mod with
  `"freezeWorld": false` in `Magelight.json`; then it answers 0 too.
- **Never set menu flags yourself.** `kFreezeFrameBackground` and
  `kTopmostRenderedMenu` on `MagelightFocus` (the PrismaUI recipe) would not
  outlive a pause retarget safely, and `kTopmostRenderedMenu` stops the UI
  pass that draws your page behind an upscaler.

```cpp
if (v4()->hostVersionNumber >= 3100 && v4()->QueryCapability("freezeworld") == 1)
    v4()->SetViewFreezeWorld(dashboard, true);   // once; takes effect while it holds UI mode paused
```

## When pages load (0.31.0)

A view's page loads some time after `CreateViewEx`, never inside it; DOM ready
(your `onDomReady`, the `ViewDomReady` event) follows the load. Since 0.31.0
the host spreads first loads over frames instead of starting every registered
view in the frame the world becomes ready (each page's parse and first script
run otherwise land in the same few frames after a save loads):

- A view loads at once when it is visible at creation (`startVisible`) or is
  shown (`ShowView(true)` or UI-mode entry) before its page has loaded, so an
  open never waits behind other views.
- The others wait in a queue in the order they were created. One starts per
  frame, and the next only once the previous page reached DOM ready or failed
  (or 100 ms passed) and the frame's Ultralight update ran under the player's
  `loadBudgetMs` (default 8 ms). A batch lasts at most about a second;
  whatever is still waiting then starts together. The log line
  `staggered load - N views over F frames in T ms` sums each batch.
- Calls made before DOM ready (`RegisterJSListenerEx`, `InteropCall`,
  `InvokeJS`, `EvalJS`) stay queued on the view and are delivered after it, as
  for any view created before the world loaded. A consumer that waits for DOM
  ready before it shows a view (or refuses to show it until then) waits for
  the queue as well, up to about a second after a load plus its page's own
  load. Show the view instead: showing it loads it at once.
- `QueryCapability("loadstagger")` is 1 while staggering is on; the player
  turns it off with `"loadStagger": false` in `Magelight.json` (every waiting
  view then loads in one frame, as before 0.31.0).

`SetViewLoadOnShow(view, true)` goes further for a panel that is rarely
opened: its page is not loaded at all until the view is first shown. Call it
right after `CreateViewEx`, before the world loads (manifest views take the
`loadOnShow` key); it decides only a load that has not started, and a loaded
page stays loaded. Such a view's DOM ready comes after its first show, so
never wait for it before showing the view.

```cpp
if (v4()->hostVersionNumber >= 3100 && v4()->QueryCapability("loadonshow") == 1)
    v4()->SetViewLoadOnShow(settingsPanel, true);   // right after CreateViewEx
```

## Your own cursor (0.31.0)

The flat cursor is the host's by default: a drawn arrow that glows over a
clickable element and turns into an I-beam over text. `SetViewCursor` gives a
view its own images, one per page state, chosen from the page's CSS cursor:

| State | Shown over | Without an image |
|---|---|---|
| `kCursorArrow` | everything not below, `cursor: url(...)` included | the host arrow |
| `kCursorPointer` | `cursor: pointer`, `grab`, `grabbing` | the view's arrow image, else the host arrow |
| `kCursorText` | text (`cursor: text`) | the host I-beam (the caret hint matters more than the look) |

```cpp
if (v4()->hostVersionNumber >= 3100 && v4()->QueryCapability("cursor") == 1) {
    MAGELIGHT_API::CursorDesc d{ sizeof(d) };
    d.state = MAGELIGHT_API::kCursorArrow;
    d.imagePath = "views/cursor/arrow.png";   // relative to the mod folder, or absolute
    d.hotspotX = 3; d.hotspotY = 1;           // image pixels, like CSS cursor: url(x) 3 1
    d.height = 28;                            // px at 1080p, scaled with the resolution; 0 = the image's own
    d.pressShrink = true;                     // shrink about the hotspot on a click, like the host arrow
    v4()->SetViewCursor(view, &d);
    d.state = MAGELIGHT_API::kCursorPointer;
    d.imagePath = "views/cursor/hand.png";
    v4()->SetViewCursor(view, &d);
}
// later: v4()->SetViewCursor(view, nullptr) clears every state
```

- One call per state, kept until changed or the view is destroyed; an empty
  `imagePath` clears that state, `nullptr` clears them all. Any thread.
- Images are PNG or DDS (anything Windows' image decoder reads), at most
  256x256. A relative path resolves against the mod folder (`Data/Magelight/
  <Mod>/` for a page there, else the page's own folder) and may not contain
  `..`; a plugin may pass an absolute path. A missing file is
  `InvalidArgument`. The image decodes on a worker thread the first time it is
  drawn, never inside a frame, and is shared by every view that names the
  same file with the same hotspot, height and press; until it is ready, and
  if it fails (one log line), the host cursor shows.
- `imagePath = "none"` (state `kCursorArrow`) says the page draws its own
  pointer: the host draws nothing over the view. A page can say the same with
  CSS `cursor: none`, on any element. Either only takes effect while the
  pointer is inside that view, and every page's cursor is forgotten when UI
  mode closes, so a page cannot leave the player without a pointer.
- A view with no cursor of its own uses its mod's default (the manifest's
  top-level `"cursor"`), then the host cursor (the player's `cursorFile`, else
  the drawn art).
- The player can override all of it: `Magelight.json` `"cursorForce": true`
  shows the host cursor everywhere, even over a page that hides the pointer;
  `"modCursors": false` ignores mod images (a page that hides the pointer
  still hides it). Design for the host cursor too.
- VR keeps its laser dot. With `vr.cursorDot` false the laser end shows the
  view's image for the page state (text falls back to the arrow image; the
  size comes from the panel, `vr.cursorScale`); `"none"` and `cursor: none`
  are ignored there.
- The hover glow belongs to the drawn art and does not carry over: give the
  pointer state its own image if you want hover feedback.

The manifest equivalent is the `cursor` key ([MANIFEST.md](MANIFEST.md)), and
Papyrus has `SetCursor` / `ClearCursor` ([PAPYRUS.md](PAPYRUS.md)).

## Tinting the host cursor (0.31.1)

To match a page's colours without image files, recolour the host's own drawn
cursor over the view. It keeps its shape, hover glow, press-shrink, I-beam and
the player's `cursorHeight`, and stays sharp at every resolution:

```cpp
if (v4()->hostVersionNumber >= 3101 && v4()->QueryCapability("cursortint") == 1) {
    MAGELIGHT_API::CursorTint t{ sizeof(t) };
    t.lit   = 0xFFDCE6F0;   // 0xAARRGGBB; alpha 0 keeps the host's colour, any other is drawn opaque
    t.shade = 0xFF6E8296;
    t.ink   = 0xFF0B1118;
    t.glow  = 0xFF7CB2CE;   // the hover glow over a clickable element
    t.ibeam = 0xFFDCE6F0;   // the I-beam over text
    v4()->SetViewCursorTint(view, &t);
}
// later: v4()->SetViewCursorTint(view, nullptr) gives the host's colours back
```

- Kept until changed or the view is destroyed. Any thread; the cursor is
  rebuilt on the render thread the next time it is drawn over the view. Set it
  when the colours change, not every frame: each new tint builds its textures,
  and the host keeps four.
- The view's own images (`SetViewCursor`, the manifest's `"cursor"`) win over
  the tint. On flat the player's `cursorFile` replaces the drawn cursor, and
  `"cursorForce": true` or `"modCursors": false` drop the tint, as they drop
  images.
- VR: the laser dot takes `lit` for its core and `ink` for its rim, at the
  panel's next redraw (`"cursorForce": true` and `"modCursors": false` drop it
  there too). With `vr.cursorDot` false the laser end shows the view's image,
  the player's `cursorFile` or the baked arrow, none of them tinted.
- C++ only: there is no manifest key or Papyrus call for the tint.

## When something doesn't work

Every failure is logged to `My Games\Skyrim Special Edition\SKSE\Magelight.log`
(`Skyrim VR` on VR, `Skyrim Special Edition GOG` on GOG) with your mod id and
the view name. `GetLastErrorMessage(mod)` returns the last
per-mod failure string. Page console warnings and errors go to the same log
(every console line with `"consoleLog": "all"` or `"devMode": true` in
`Magelight.json`, 0.31.0); devMode also adds hot reload and an on-page banner
for script errors. See [TROUBLESHOOTING.md](TROUBLESHOOTING.md).
