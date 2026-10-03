# Magelight UI — Claude Code Project Instructions

Magelight is a GPU-rendered web UI host for Skyrim SE/AE and VR: an SKSE plugin that
hooks `IDXGISwapChain::Present`, runs Ultralight 1.4 (WebKit 615) on the game's D3D11 device
through our own LGPL-isolated driver DLL, composites views over the game, routes input, and
exposes a versioned C ABI (`api/MagelightUI_API.h`) that other SKSE plugins consume. The
first consumer is SeverActions; the goal is a public framework.

Repo: `https://github.com/Severause/MagelightUI`. Work on a branch and open a PR; the
maintainer reviews and merges. Run `git remote -v` before any push and push only to the
remote you mean. No double quotes in commit messages; trailer
`Co-Authored-By: Claude <model> <noreply@anthropic.com>`.

## Layout

```
src/Magelight.cpp/.h        the host (hook, thread rules, views, input, cursor, images, events)
src/MagelightApi4.cpp/.h    the v4 mod registry (identity, events, UI-mode owner, results)
src/MagelightApiExport.cpp  the exported C ABI tables (v1..v4) — ORDER = struct member order
src/MagelightManifest.cpp   Data/Magelight/<ModId>/manifest.json loader (docs/MANIFEST.md)
src/MagelightPapyrus.cpp    the Papyrus tier natives (papyrus/Magelight.psc, docs/PAPYRUS.md)
src/MagelightDevWatch.cpp   devMode hot reload (folder watchers -> ReloadView)
papyrus/Magelight.psc       the script consumers compile against (staged as Scripts/Magelight.pex)
views/gate/                 the version-gate notice page
examples/                   manifest example mods (build.ps1 -Examples stages them)
api/MagelightUI_API.h       the PUBLIC header consumers vendor (SA: Native/src/MagelightUI_API.h)
gpu/                        MagelightGPU.dll — AppCore-derived D3D11 GPUDriver, LGPL-2.1, C ABI only
extern/ultralight/          vendored 1.4.0b SDK (dev CDN); extern/appcore-ref/ = upstream driver
frontend/, views/           React demo (Vite, es2022) + probe/badge pages (dev surfaces)
assets/, interface/         loose runtime files, the blank focus-menu SWF (the cursor is drawn in code: src/MagelightCursorArt.h)
tools/desktop-harness/      run our driver (or AppCore's) on the desktop — no game needed
docs/                       MANIFEST.md, CPP.md, SDK.md, PAPYRUS.md, VR_PRESENTER.md
build.ps1                   build → C:\b\mgl, stage → C:\b\mgl\stage (namespaced runtime patch)
```

## Invariants (each cost a debugging arc — do not regress)

1. **Ultralight lives on ONE thread**: created on the game's main thread (= in-game present
   thread) after `kPostLoadGame`/`kNewGame` (`s_worldReady`, `s_mainThreadId`); frames from any
   other thread skip all Ultralight work (`s_ulThreadId`). Violations fastfail with no crashlog.
2. **Listener callbacks fire on the render thread** (JS listeners, DOM ready, console, load
   failed). Game state = `GameTask::Post` (`MagelightGameTask.h` says why), never a raw
   `SKSE::GetTaskInterface()->AddTask`; mods use `PostGameTask`. A new main-thread entry point
   (a hook, a sink, an engine menu) holds a `GameTask::Scope` so its posts go through the pump. UI-mode entry/exit runs on the
   game thread (`SetUIMode` is an SEH-wrapped function: no locals with destructors inside it).
3. **Namespaced runtime**: never ship stock Ultralight DLL names (coexistence with other Ultralight-based UI mods; Windows
   binds imports by base name). `build.ps1` byte-patches same-length names; re-verify at every
   SDK bump. `C:\b\mgl\MagelightGPU.dll` is the unpatched copy the desktop harness uses.
4. **LGPL isolation**: AppCore-derived code stays in `gpu/` + `MagelightGPU.dll`, reached only
   through `gpu/MagelightGpuApi.h` (C ABI). Never include driver internals in the host; never
   copy driver code into `src/`. Modifications carry `// MG:` markers.
5. **Own FileSystem** (`MlFileSystem`): fixed MIME table (registry MIME kills module scripts),
   query-string strip, percent-decode, absolute drive-letter paths. `resources/` must resolve.
6. **Mouse rides the SKSE input sink + MenuCursor**; the window never gets WM mouse messages.
   `ToggleControls(..., storeState=false)`; `ForceExitUIMode` on every load boundary.
7. **Own engine menu** (`MagelightFocus`, kUsesCursor) on UI-mode entry — the engine drives
   MenuCursor only while a cursor-using menu is topmost.
8. **es2022 bundles on the 1.4 SDK**; `crossorigin` stripped; `base: './'`.
9. **Blank pages must name themselves**: console listener + `OnFailLoading` → log + event.
10. **Public ABI is append-only** (see `.claude/agents/review-abi.md`). The export table's
    initializer order must equal the struct's member order — a swapped pair compiles fine and
    calls the wrong function in every consumer.
11. **Texture images are session-long**: never unregister an image a page may still reference;
    `UpdateTextureImage` re-points, `InvalidateImage` per change, dirty-gated (an unconditional
    per-frame invalidate repaints the fullscreen page every frame — the frozen-cursor bug).
12. **Known engine defect** (Ultralight core, both drivers): a rounded box with differing border
    sides that is not fully opaque draws wedge triangles. Not ours to fix; pages keep such boxes
    opaque or drop the radius. Repro: `tools/desktop-harness/pages/translucent-borders.html`.
13. **Views are erased only in `ApplyPendingLifecycle`** (render thread): registry entry erased
    BEFORE the View RefPtr drops; LoadURL/Reload/release on live pages happen OUTSIDE
    `s_viewsMutex` (they run page handlers synchronously, which re-enter the locking listeners).
14. **UI-mode ownership is decided on the game thread** (the host's entry task) and mirrored into
    the v4 registry from the Entered/Exited/Refused events — never claimed ahead of entry.

## Build, deploy, test

- Build: `powershell -ExecutionPolicy Bypass -File build.ps1` (toolchain discovery: VS BuildTools
  18 is invisible to vswhere; the script has the fallback). Output `C:\b\mgl`, stage
  `C:\b\mgl\stage`.
- Frontend: `cd frontend && npm run build` → `views/app/` (committed; DLL build needs no node).
- Deploy the stage to your test targets, game closed (`Get-Process SkyrimSE`): Steam
  `...\Skyrim Special Edition\Data` and/or your mod-manager mod folder (e.g.
  `<modlist>\mods\Magelight UI`). Zip: `tools\package.ps1 -Version <ver> -Suffix -dev`
  (never `Compress-Archive`: it writes `\` into entry names).
- Settings: `My Games\Skyrim Special Edition\SKSE\Magelight.json` (`toggleKey`, `demoViews`, `stallWatchdog`/`stallThresholdMs`,
  `imageProbe`, `forceCpu`, `cursorFile/Height/HotspotX/Y`). Log: `...\SKSE\Magelight.log`.
- **Desktop first**: any rendering question goes to `tools/desktop-harness` before a game cycle
  (our driver vs AppCore's on the same page; `inject.js` for live CSS bisects).
- Version: CMake `project(... VERSION x.y.z)` is the single source (`PLUGIN_VERSION*` macros).
  Bump it for every deployable change; the zip name follows it.

## Review

`/code-review` (`.claude/commands/code-review.md`) fans out to the rubrics in `.claude/agents/`:
threading, bugs, abi, gpu-driver, packaging, frontend, docs, simplicity. Run it before opening
a PR that touches `src/`, `api/`, `gpu/`, `build.ps1` or the SDK.

15. **Hotkeys never open over the game's own input.** `DispatchHotkey` refuses to
    enter UI mode while an engine menu is up, the console is open or
    `textEntryCount > 0`, and while a page has key focus only the ACTIVE view's
    own binding fires (to close). Anything looser turns a letter typed into a
    text field into another mod's panel opening.

16. **Ultralight load-listener callbacks can fire synchronously inside the call
    that triggers them.** `OnBeginLoading` runs INSIDE `LoadURL`, which
    `MaterializeViews` calls under `s_viewsMutex`; a listener that takes the
    same (non-recursive) mutex throws `system_error`, the SEH guard disables
    the overlay, and the whole UI is dead for the session (0.16.0 field log).
    Only the asynchronous callbacks (`OnWindowObjectReady`, `OnDOMReady`,
    `OnFailLoading`) may lock; state a synchronous one needs is set by the
    caller that requested the load.
17. **A key typed into a page must not exist anywhere else in the engine.**
    While UI mode is on, the engine keyboard device's Poll (BSIInputDevice
    vfunc 02) is skipped, so no keyboard button event is ever generated —
    not for SKSE input sinks, not for mods that hook the input dispatcher
    ahead of every sink (Immersive Equipment Displays opened its UI on
    Backspace typed into a search field and froze the cursor, 2026-09-03),
    not for Papyrus `OnKeyDown`. The text-entry flag is raised too, for the
    mods that check it. Typing, Escape and bound hotkeys all ride the window
    proc (WM_KEYDOWN / WM_CHAR are the OS, not DirectInput), so they keep
    working. Mouse and gamepad devices are untouched. Two things the sink
    layer alone could NOT do, which is why the mute lives at the device:
    a sink cannot precede a dispatcher hook, and unlinking events only
    shields sinks registered later. An embedding mod's own engine menu
    (SA's live-view carrier) opens ABOVE the focus menu and so receives the
    menu-mode user events; it must swallow them.
