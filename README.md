# Magelight UI

**GPU-rendered HTML UI for Skyrim SE/AE/VR mods.** Write your mod's menus, HUD widgets and popups
as web pages; Magelight renders them with Ultralight™ (a WebKit-based engine) straight onto the
game's own Direct3D device, routes mouse and keyboard to them, and gives you three ways to drive
them: a `manifest.json` (no code), Papyrus, or C++.

Named for the spell that puts a light exactly where you need it, and a quiet nod to the engine
inside. MIT licensed — see [Licensing](#licensing).

```
Data/Magelight/CompassPlus/manifest.json          ← a folder is a mod
Data/Magelight/CompassPlus/views/hud/index.html   ← any HTML/CSS/JS, or a React build
```

## Three ways in

| Tier | You write | Best for |
|---|---|---|
| **Manifest** — [docs/MANIFEST.md](docs/MANIFEST.md) | `manifest.json` + pages | HUD widgets, static panels, anything a page can do on its own. Zero code, zero DLL, exists from data-load |
| **Papyrus** — [docs/PAPYRUS.md](docs/PAPYRUS.md) | a script against `Magelight.psc` | Config menus and popups driven from quest/scene logic. Host events arrive as ModEvents |
| **C++** — [api/MagelightUI_API.h](api/MagelightUI_API.h) | an SKSE plugin | Everything: per-frame data, texture images (render an actor into a page), custom listeners, UI-mode ownership |

Pages talk to the host through two verbs the host installs on every page — `window.magelight.send`
and `window.magelight.on` — typed by [`@magelight/sdk`](packages/sdk) with React hooks in
[`@magelight/react`](packages/react) and a Vite plugin that emits game-ready view folders
([docs/SDK.md](docs/SDK.md)); the packages are on npm, and `npm create magelight-view MyMod`
scaffolds a React mod. The **manifest path below needs none of this** and is the fastest way to
see a page on screen.

## Requirements

| Runtime | Needs |
|---|---|
| Skyrim SE 1.5.97 | SKSE64, Address Library for SKSE Plugins (SE) |
| Skyrim AE 1.6.x and 1.7.x | SKSE64 (2.3.0 or newer for 1.7.x), Address Library for SKSE Plugins (AE) |
| Skyrim VR 1.4.15 | SKSEVR, VR Address Library for SKSEVR |

Every runtime also needs the Microsoft Visual C++ Redistributable 2015-2022 (x64). Without it the
Ultralight runtime cannot load and no Magelight page appears (the log says
`failed to load ... (GetLastError=126)`).

## Quick start (manifest, five minutes)

1. Install Magelight UI (a plain zip). To try the example mods first, build them from this
   repository (`build.ps1 -Examples`, see "Building the host").
2. Make `Data/Magelight/MyMod/manifest.json`:
   ```json
   { "modId": "MyMod", "name": "My Mod", "minHost": "0.16.0",
     "views": { "hud": { "path": "views/hud/index.html", "layer": "hud", "anchor": "top-right",
                         "x": 24, "y": 24, "w": 320, "h": 96, "clickThrough": true, "startVisible": true, "hotkey": "F11" } } }
   ```
3. Put any `index.html` at `views/hud/`. Launch: it is on screen; F11 hides and shows it.
4. `Magelight.log` prints one line per manifest and names every problem with the file and the
   view. It is in `Documents\My Games\Skyrim Special Edition\SKSE\` (`Skyrim VR` on VR,
   `Skyrim Special Edition GOG` on the GOG edition).

For a page that takes input, use `"layer": "panel"` without `clickThrough`; its hotkey then
opens it in **UI mode** (cursor up, game controls suspended, keyboard to the page; the same key
closes it, and so does Escape unless the page takes Escape itself). One view holds UI mode at a
time, across all mods. Pick a hotkey the game does not use (not F5 Quicksave or F9 Quickload)
and that no other mod binds: one binding per key, and the first binder keeps it.

## What the host does for you

- **GPU rendering** on the game's device, transparent compositing, per-view z-order in four
  layers (`hud` < `panel` < `popup` < `system`); CPU fallback if the GPU path cannot start.
- **Input**: mouse via the game's input sink, keys and text via the window; an own-drawn cursor;
  clipboard; hotkeys that never fire over an engine menu, the console or game text entry, and
  never open another mod's page while you are typing. On flat, pages take mouse and keyboard;
  controller navigation inside a page is up to each mod. On VR, the controller laser points
  and the trigger clicks.
- **Files**: a page reads its own mod folder (`Data/Magelight/<Mod>/`, or its own directory for
  a page kept elsewhere) and the host's runtime dir, nothing else — no other mod's files, no
  `..` out of `Data`, no arbitrary drive path (0.26.4; the log names the first 32 refused reads).
- **Network**: none by default (0.30.0). A page reaches only its files unless its mod opts in:
  loopback (`http(s)` to this machine, for a local server) from the manifest
  (`"network": "loopback"`), Papyrus (`SetNetworkPolicy`) or the mod's DLL; the internet only
  from the mod's DLL (`SetNetworkPolicy(mod, NetworkPolicy::Any)`). Each opt-in is logged.
- **Storage**: by default each mod's pages get their own localStorage/IndexedDB/cookie jar, one
  per game install: every save, character and mod-manager profile shares it, and loading a save
  does not roll it back. Jars are shared by name: `"default"` is the jar all v1-v3 views use,
  and a custom session name is shared with every mod that names it.
- **Script ownership** (0.30.0): Papyrus cannot tell one script from another, so scripts can
  look up any mod's views but act only on script-owned mods (registered from Papyrus, or
  declared by a manifest no DLL adopted) whose storage jar no plugin uses; a mod registered
  from a DLL is out of their reach. Among script-owned mods, any script can drive any mod's
  views, run JavaScript in them and read their page storage back: keep secrets out of page
  storage.
- **Page identity** (0.30.0): a page's `window.magelight.send` reaches only its own view's
  listeners; one page cannot act as another view.
- **Lifecycle**: create, show, reload, navigate, destroy; load failures name the page in the log
  instead of leaving a blank rectangle.
- **Dev loop** (`"devMode": true` in `Magelight.json`): edit a page and it reloads in game; JS
  errors paint a banner on the page itself. The WebKit Web Inspector (staged by `build.ps1`,
  not in the player download) is hosted by Magelight; a C++ mod opens it with `ShowInspector` (there is no key for
  it).
- **Coexistence**: a version gate lists mods that need a newer host; the Ultralight runtime ships
  under private names so another Ultralight-based plugin cannot collide with it.

## Documentation

- [docs/MANIFEST.md](docs/MANIFEST.md) — the manifest schema
- [docs/PAPYRUS.md](docs/PAPYRUS.md) — the script API and its ModEvents
- [docs/SDK.md](docs/SDK.md) — the page contract and the npm packages
- [docs/CPP.md](docs/CPP.md) — the C++ tier (a minimal plugin, threading + versioning rules)
- [api/MagelightUI_API.h](api/MagelightUI_API.h) — the raw C ABI (self-documenting header, v1–v4)
  and [api/MagelightUI_Mod.h](api/MagelightUI_Mod.h) — the optional header-only convenience wrapper
- [docs/DISTRIBUTION.md](docs/DISTRIBUTION.md) — shipping a mod that depends on Magelight (bundle vs require, licensing)
- [docs/PRISMAUI_MIGRATION.md](docs/PRISMAUI_MIGRATION.md) — migrating a mod to Magelight (or running both hosts side by side)
- [CHANGELOG.md](CHANGELOG.md) — which host version added which feature (what to put in `minHost`)
- [docs/TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md) — **blank page?** start here
- [CONTRIBUTING.md](CONTRIBUTING.md) — build from source, the ABI and invariant rules for PRs
- [SECURITY.md](SECURITY.md) — report sandbox-boundary issues privately

## Building the host

CMake + CommonLib (MinLL/CommonLibVR, an MIT continuation of CommonLibSSE-NG, through vcpkg),
the Ultralight 1.4.0b SDK under `extern/ultralight/` (fetched, not committed). It needs Visual
Studio's C++ tools and a standalone vcpkg: `VCPKG_ROOT`, or `C:\vcpkg` when that is unset
(CONTRIBUTING.md step 2). `build.ps1` builds
to `C:\b\mgl`, stages an installable layout under `C:\b\mgl\stage` (`-Examples` builds and
stages the example mods too; run `npm ci && npm run build` at the repo root first),
`tools/check_stage.ps1` lints it, `tools/package.ps1 -Version x.y.z` produces the release
zips (runtime, DevTools, Examples, symbols). `CLAUDE.md` carries the build/deploy/test quick reference and the invariants every change
must respect.

## Origin

Magelight began as a bespoke in-game web-UI host for
[SeverActions](https://github.com/Severause/SeverActions): same web stack, our own host, so the
React frontend (tens of thousands of lines) ported almost unchanged and the problems that were
properties of the old host — CPU rasterization, locked view files, no lifecycle control, no VR
path — went away. The host layer is deliberately the seam that would let the engine be swapped
later without touching any frontend. SeverActions runs on it in the field.

## Licensing

Magelight UI is **MIT** (see `LICENSE`, `LICENSE.txt` in the download): the host plugin, the API header, the Papyrus script,
the SDK packages, the examples and the docs. Build on it, fork it, ship it inside your mod;
keep the notice.

Third-party components keep their own terms, listed in `NOTICES.txt` (with the full license
texts of the compiled-in libraries in `THIRD_PARTY_LICENSES.txt`); both sit at the root of the
download and are installed beside the runtime under `SKSE/Plugins/Magelight/license/`:

- **Ultralight** runtime — reaches players under Ultralight's End User License Agreement
  (`EULA.txt`); Magelight's author uses it under the Ultralight Free License Agreement v1. The
  four DLLs are Ultralight 1.4.0b's, renamed, with the module-name strings inside them
  rewritten in place (same length) so they import each other under the new names, with the
  written permission of Ultralight, Inc.; no code is changed. The private names let two
  Ultralight-based plugins coexist in one process. That permission is the author's: a mod that
  bundles Magelight ships its files as they are and never renames or patches Ultralight's DLLs
  itself ([docs/DISTRIBUTION.md](docs/DISTRIBUTION.md)).
- **MagelightGPU.dll** — LGPL-2.1, derived from Ultralight's AppCore D3D11 driver and isolated in
  its own DLL behind a C ABI; its corresponding source is the `gpu/` folder of this repository
  at each release's tag (`v0.31.0` for 0.31.0).
- **Web Inspector** (staged by `build.ps1`, not in the player download) — Apple BSD.
- Compiled into `Magelight.dll`: CommonLibVR (MinLL's MIT continuation of CommonLibSSE-NG),
  {fmt}, spdlog, nlohmann/json, DirectXMath and DirectXTK — MIT; rapidcsv, Xbyak and the
  OpenVR headers — BSD 3-Clause. The example and test pages carry React (MIT).

Ultralight (c) 2024 Ultralight, Inc. All rights reserved. Ultralight is a trademark of
Ultralight, Inc. Please see the accompanying NOTICES.txt for full text.
