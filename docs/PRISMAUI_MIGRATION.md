# Moving a mod from PrismaUI to Magelight UI

For mod authors with an existing PrismaUI dependency. End users don't need this document —
and, because the two hosts coexist, they never have to choose (see
[Running both](#running-both)).

A note on framing before anything technical: PrismaUI proved that web-stack UI belongs in
Skyrim modding and built the market this project serves. Magelight exists because
SeverActions needed things PrismaUI doesn't do — GPU rendering, lifecycle control, VR — not
because the model was wrong. If your mod works on PrismaUI today and needs nothing listed
below, staying is a legitimate choice; this page is for the day it isn't.

## Why mods move (and why some shouldn't)

**You might move for:** GPU rendering through a D3D11 driver on the game's own device, view
lifecycle beyond create/destroy (`ReloadView`, `Navigate`, hibernation of hidden views), view
bounds/anchors/layers (PrismaUI views are implicit fullscreen), a Papyrus API, hot reload in devMode, per-mod storage isolation, VR support
(SteamVR-native *and* OpenComposite), and a `minHost` version gate that tells users what to
install instead of rendering a blank page.

**You might stay for:** PrismaUI is installed by every one of your current users and its
CPU path is fine for a light page; your page does nothing PrismaUI can't do; you don't want
to touch a working integration. (Project-state facts — Nexus build version, game-version
compatibility — change over time; check them fresh rather than trusting
any summary, including this one.)

## Running both

Magelight ships its Ultralight runtime under private file names precisely so it cannot
collide with PrismaUI's copy in the same process. A user can install both hosts and run
PrismaUI mods and Magelight mods side by side. One caveat: if your C++ polls PrismaUI's
`HasAnyActiveFocus` to avoid overlapping UI, query Magelight's `IsUIModeActive()` too when
both hosts are present — neither host sees the other's focused view.

## What carries over

Both hosts embed Ultralight (WebKit), so **your pages carry over almost unchanged** — this
is measured, not aspirational: SeverActions ported 13 long-lived views across 10 HTML
entries with the frontend essentially untouched.

- **Page → host:** PrismaUI registers C++ listeners as page globals (`window.castSpell(...)`).
  Magelight's `RegisterJSListener(view, name, cb)` installs the same `window.<name>(arg)`
  global — your page calls it identically. New code can use the typed
  `window.magelight.send(channel, payload)` bridge instead; both work.
- **Host → page:** PrismaUI's `InteropCall(view, fn, arg)` invokes `window.fn(arg)`.
  Same name, same behavior on Magelight. New code can use
  `window.magelight.on(channel, fn)` with pre-mount replay instead of hand-rolled
  ready-handshakes (Magelight buffers sends that arrive before your app mounts).
- **Marshalling:** pass raw JSON as one string arg, no extra escaping — same rule on both.
- **Text focus:** Magelight detects text-field focus automatically (`focusin`/`focusout`
  on real inputs), so PrismaUI's `__prismaNativeImeFocusChanged` signal is not needed —
  delete any calls to it.
- **UI sounds (Magelight-only gain, 0.29.0):** neither host's pages can play audio (Ultralight has no
  media stack — the recurring "can I add a click sound to PrismaUI buttons?" question). On Magelight
  put `data-ml-sound="click"` on a button, or call `magelight.sound('click')`, and the host plays the
  vanilla menu sound through the game's audio. PrismaUI ignores the attribute, so markup needs no
  branch. A script call does: PrismaUI has no `window.magelight`, so an unguarded
  `magelight.sound(...)` throws there. Guard it (`window.magelight?.sound?.('click')`) or use the
  SDK's `host.sound`, which is a no-op off Magelight.
- **Network:** Magelight pages are file-only by default (0.30.0): they read their own files and
  nothing over the network. If your page fetches a local server, your DLL calls
  `SetNetworkPolicy(mod, NetworkPolicy::LoopbackOnly)`; for the internet, `NetworkPolicy::Any`
  ([CPP.md](CPP.md)).
- **localStorage:** persists on both hosts. Note: Magelight's cache is a different folder,
  and sessions are isolated per mod by default (`sessionName: "default"` opts back into a
  shared jar). User data stored under PrismaUI does **not** move — if that matters for your
  mod, migrate it through your own save data.
- **Frontend build:** strip `crossorigin` from module tags and set `base: './'` — required
  over `file:///` on Magelight, tolerated-but-pointless on PrismaUI. If you targeted es2019
  for an older engine, es2022 works on Magelight's Ultralight 1.4 (WebKit 615). Both are what
  `@magelight/vite-plugin` sets for you.

## The C++ mapping

Magelight's header is the same acquisition pattern (`Magelight_RequestApi(n)`, versioned
function-pointer struct, null = host absent). The calls SA used map one-to-one:

| PrismaUI | Magelight | Note |
|---|---|---|
| `CreateView(path, onDomReady)` | `CreateViewEx(mod, &desc, &out)` | fullscreen via `ViewDesc::fullscreen`, or give real bounds/anchor |
| `Show` / `Hide` | `ShowView(view, bool)` | identical |
| `Focus(view, pauseGame)` | `RequestUIMode(view, flags)` | returns a `Result` — someone else may own UI mode; queue or refuse |
| `Unfocus(view)` | `ReleaseUIMode(mod)` | never hides the view — same as Unfocus |
| `IsValid(view)` | `IsViewValid(view)` | |
| `HasAnyActiveFocus()` | `IsUIModeActive()` | per-host — see [Running both](#running-both) |
| `Invoke(view, js)` | `InvokeJS(view, js)` / `EvalJS` | EvalJS also returns result/exception |
| `InteropCall` / `RegisterJSListener` | same names | `RegisterJSListenerEx` adds a `void* user` |
| `kFreezeFrameBackground` / `kTopmostRenderedMenu` on `PrismaUI_FocusMenu` | `SetViewFreezeWorld(view, true)` (0.31.0) | once per view; the host applies it while the view holds UI mode paused and drops it before the pause. Never set flags on `MagelightFocus` yourself |
| FocusMenu close polling | **delete it** | `onEvent` gets `UIModeExited` on *every* exit path — no Escape poller, no orphaned FocusMenu |
| Destroy (unused by most) | `DestroyView(view)` | async; views can also `ReloadView`/`Navigate` |

Three behavioral differences to design around:

1. **Threading contract is explicit.** `onEvent` and `EvalJS` results arrive on the callback
   thread you pick at `RegisterMod` (default: game thread — the only thread where `RE::` is
   safe). JS listeners (`RegisterJSListener`/`RegisterJSListenerEx`) do not: they run inline on
   the render thread, as PrismaUI's callbacks effectively do. Marshal from them with
   `PostGameTask` (0.30.1+), not SKSE's `AddTask`: copy the argument, then post the work before
   touching `RE::`. On Skyrim VR an `AddTask` made inside the frame can hang the game
   ([CPP.md](CPP.md), "The rules that bite").
2. **UI mode is owned, not global.** One view holds UI mode at a time, across all mods;
   `Busy` is a first-class result with the owner's name in the log, and `FocusDenied` is an
   event the owner receives. HUD views never take UI mode.
3. **Failures name themselves.** `GetLastErrorMessage(mod)`, load-failure events with the
   resolved path, page console in `Magelight.log` — instead of a blank view with nothing in
   the log.

## Distribution changes

Your FOMOD's dependency line changes from PrismaUI to Magelight; pages move from
`Data/PrismaUI/views/...` to `Data/Magelight/<modId>/views/...` (the folder is your
namespace — see [MANIFEST.md](MANIFEST.md)). Declare `minHost` and the host itself renders
the "needs Magelight a.b.c" panel for users on an older host. Bundle-versus-require and the
licensing your mod inherits are covered in [DISTRIBUTION.md](DISTRIBUTION.md) — both hosts
ship the same Ultralight free-tier runtime, so nothing about your license posture changes.

If your mod is Papyrus-only, you were never on PrismaUI — start at
[PAPYRUS.md](PAPYRUS.md) instead; there is nothing to unlearn.

## A suggested port order

1. Build your frontend against `@magelight/vite-plugin` (or set `base`/`crossorigin`/es2022
   by hand) and iterate in a plain browser with the SDK's mock host — no game needed.
2. Swap the native calls per the table; delete your Escape poller and FocusMenu plumbing.
3. Smoke-test in the desktop harness (`tools/desktop-harness`), then in game with
   `"devMode": true` for hot reload and the on-page error banner (page console output is in
   `Magelight.log` either way).
4. Ship to a small test group *with both hosts installed* — coexistence is the thing to
   exercise, and it's the configuration your users will actually have during any transition.
