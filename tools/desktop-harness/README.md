# Desktop harness — run Magelight's GPU driver without the game

Two tiny Win32 apps that load the stock Ultralight SDK runtime from
`extern/ultralight/bin` and render a page the way the in-game host does, so
rendering questions can be answered in a minute instead of a game cycle.

| App | Renderer | Purpose |
|---|---|---|
| `harness.exe` | **our** `MagelightGPU.dll` on a D3D11 swapchain; Update / RefreshDisplay / Render / `MgGpu_DrawCommandList` / composite the view RT — the host's frame pump | Reproduce and bisect what the game shows |
| `probe_appcore.exe` | Ultralight's own AppCore reference driver | Control: "is it our driver or the engine?" |

## Build (once)

```
build.bat      -> harness.exe        (needs the UNPATCHED C:\b\mgl\MagelightGPU.dll beside it, or
                                      $MG_BUILD_DIR\MagelightGPU.dll when you built with MG_BUILD_DIR:
                                      the stage copy imports the namespaced runtime names)
build2.bat     -> probe_appcore.exe
```

Both expect, next to the exe: the SDK's `AppCore.dll Ultralight.dll UltralightCore.dll WebCore.dll`,
and an `assets/` folder holding `resources/` (icudt + cacert from the SDK) plus the pages. To serve a
real mod view, junction its folder in: `mklink /J assets\sa "<...>\YourMod\views"` and
load `file:///sa/index.html`.

## Run

```
harness.exe [url] [viewW] [viewH]        default: file:///sa/index.html 2560 1440
```

The harness draws with 4x MSAA, the host's default; set `MG_MSAA` (1 off, 2, 4, 8) to compare.

`harness.exe` evaluates `inject.js` (next to the exe) into the page ~400 frames in, which is how a
live CSS bisect works: edit the file, rerun, screenshot. It also carries a scripted walk into
SeverActions' Inventory > Stats with a fake stats payload (see the frame hooks in `main`).

## Pages

- `pages/borders-plain.html` — border-shape cells (uniform, accent stripe, top bar, top-only, no radius, pseudo stripe), opaque colours. All render on every path.
- `pages/borders.html` — the same under SeverActions' shell (transform-scaled, composited page layer).
- `pages/translucent-borders.html` — **the Ultralight defect repro** (2026-09-02): rounded boxes with a differing side AND translucent colours, under five wrappers. At a 2560x1440 view some boxes draw their border sides as filled wedges / stray solid edges on BOTH drivers; fully opaque boxes never do. Root cause is inside UltralightCore's transparency-layer border painting (WebKit's `paintTranslucentBorderSides` / bleed-avoidance layer + per-side clip polygons). Workaround for pages: keep such boxes fully opaque, or remove the radius on row-type elements. See SeverActions `styles/host-magelight.css`.

## Screenshot from a script (PowerShell)

Start the exe, `Start-Sleep`, take `MainWindowHandle`, `GetWindowRect`, `Graphics.CopyFromScreen`,
save PNG, `Stop-Process`. The full snippet lives in the session notes; it is ~15 lines.
