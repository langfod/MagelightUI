# Troubleshooting

Everything below starts in **`Magelight.log`**, in `Documents\My Games\<game>\SKSE\`, where
`<game>` is `Skyrim Special Edition` (Steam SE/AE), `Skyrim Special Edition GOG` (GOG) or
`Skyrim VR`. On a PC whose Documents folder is in OneDrive, that is
`OneDrive\Documents\My Games\...`. The host logs one line per view it creates, per manifest it
reads, per page that fails to load, and per hotkey press. Read it before anything else, and copy
it before you start the game again: each launch overwrites it. Line one names the running
version (`Magelight v0.30.3 loading`).

## Nothing from any Magelight mod appears

- **`failed to load <runtime DLL> (GetLastError=126)`**: Windows could not load the Ultralight
  runtime. With no Visual C++ redistributable at all the line names `Magelight1Core.dll` (the
  first one loaded); with an old one it usually names `MgWCore.dll`. First check that `Magelight1Core.dll`, `MgWCore.dll`,
  `Magelight1.dll` and `MgACore.dll` are in `Data\SKSE\Plugins\Magelight\` (a missing file gives
  the same code). If they are, install or repair the **Microsoft Visual C++ Redistributable
  2015-2022 (x64)**, latest version: the runtime needs `MSVCP140_2.dll` and
  `VCRUNTIME140_1.dll`, which older builds of it lack.
- **`Ultralight runtime not found — overlay disabled this session`**: the runtime folder is
  missing or incomplete. Reinstall Magelight UI, and if another mod also ships Magelight, see
  "Two copies of Magelight" below.
- **No `Magelight.log` at all**: SKSE did not load the plugin. Check the requirements for your
  runtime (SKSE64 or SKSEVR, and the matching Address Library) and SKSE's own log.

## Blank page?

The number one question for any web-in-game UI. In order of likelihood:

1. **The page did not load.** Look for `view N page '...' missing — inline banner fallback` or
   `view N FAILED to load - ...` (the mod also gets a `ViewLoadFailed` event). The path is
   printed exactly as resolved. Manifest and Papyrus paths are relative to the mod folder (a
   Papyrus page that is not there is refused); C++ paths are relative to
   `Data/SKSE/Plugins/Magelight/` unless they start with a drive letter.
2. **Vite put `crossorigin` on the script tag.** Over `file://` the page origin is null and the
   module is refused silently. Use `@magelight/vite-plugin`, or strip the attribute and set
   `base: './'` yourself.
3. **Modern syntax.** Ultralight 1.4's WebKit (615) parses through `es2022`; newer is
   unverified. Target `es2022` (the vite-plugin's default) — and do not lower the Ultralight
   SDK without dropping to `es2019` (1.3-era WebKit 610 rejected `?.`/`??`).
4. **A JS exception during mount.** With `"devMode": true` in `Magelight.json` it paints a red
   banner on the page; with or without it, it is in the log as `[view N console/ERROR]`.
5. **It loaded but is transparent.** Pages start with a transparent background; if your CSS
   sets `html { background: transparent }` and nothing else paints, you see the game. Give the
   root element a background.
6. **The view exists but is hidden.** `startVisible` defaults to false; a manifest panel shows
   only when its hotkey (or a DLL/script) opens it. Check the `hotkey <scancode> -> view N`
   lines.

## A page cannot reach its server

Since 0.30.0 a page reaches nothing over the network unless its mod opts in: loopback (a server
on this machine) through the manifest (`"network": "loopback"`), Papyrus `SetNetworkPolicy` or
the mod's DLL; any other host only through the DLL. The log names a mod's policy when the mod
changes it, and the first 32 refused requests of the session. Views created through the v1-v3
C++ `CreateView` belong to no mod: they are file-only and cannot opt in.

## The hotkey does nothing

- A refused binding is logged with the mod and the reason:
  `BindHotkey: scancode N is bound by <mod>` (the first binder keeps a key), or
  `BindHotkey: that scancode is the host's own toggle key` (PageUp unless `toggleKey` in
  `Magelight.json` says otherwise). A manifest adds its file and view name to the line.
- A hotkey never *opens* a page while an engine menu, the console, or a game text field is
  active (`hotkey ignored — an engine menu, the console or text entry is open`), and while a
  page has key focus only that page's own key fires (to close it).
- The user may have rebound or disabled it in `Magelight.json` (`"hotkeys"`); the log says
  `hotkey for 'Mod/view' rebound ...` or `disabled by Magelight.json`.

## I can move but the page stays / I cannot move

UI mode = cursor up + game controls suspended + keyboard to the page. To leave it: Escape,
unless the page has taken Escape for itself (a dialog closing itself); the page's own hotkey,
which always closes it; or the host toggle key (PageUp by default), which always leaves UI mode.
On VR, B or Y leaves it unless the mod bound those buttons itself. Leaving UI mode does not hide
a page by itself (a mod may want it visible, HUD-style); a hotkey close does hide. If controls
stay suspended after a crash-to-menu, load again: the host resets UI mode on every load.

## Two mods fight over the screen

One view holds UI mode at a time, across all mods. The second requester is told `Busy`, the
holder hears `FocusDenied`, and `kUIModeFlagQueue` waits its turn instead. This is by design; a
mod that must be on top uses the `popup` or `system` layer for stacking, not UI mode.

## Two copies of Magelight

A mod such as SeverActions can ship its own copy of Magelight. If you also install Magelight UI
by itself, the copy that wins the file conflict is the one that runs; in MO2 that is the mod
lower in the left pane. Let the newer copy win. Line one of `Magelight.log` names the running
version, and a "Magelight UI needs updating" page means an older copy won.

## Text fields do not type / paste

Ctrl+C/V/X work as of 0.10.2. If typing lands in the game instead of the page, the view is not
the UI-mode view: only the focused view receives keys.

## Performance

Each visible view costs a texture and a paint when it changes; idle pages cost nothing. Keep
HUD widgets small, avoid `position: fixed` full-screen containers on HUD layers, and prefer
`startVisible: false` for panels. Fullscreen views repaint on resolution change. A page's
JavaScript runs on the game's main thread, so heavy script work, or console output in a loop
(every line goes to the log), costs frame time; strip console logging from release builds.

## Rendering oddities (Ultralight 1.4 GPU path)

A rounded box whose borders differ per side and is not fully opaque can draw wedge artifacts.
Keep such boxes opaque or drop the radius. Known engine defect.

## Where things are

`<My Games>` is `Documents\My Games\<game>` as described at the top.

| | |
|---|---|
| Log | `<My Games>\SKSE\Magelight.log` (overwritten each launch) |
| Settings | `<My Games>\SKSE\Magelight.json`, created by hand (see "Magelight.json" below) |
| Page storage | `<My Games>\SKSE\Magelight-cache\<modId>\`: one folder per mod, shared by every save, character and mod-manager profile, not rolled back when a save loads. Delete a mod's folder (game closed) to reset it |
| Runtime | `Data\SKSE\Plugins\Magelight\` |
| Manifest mods | `Data\Magelight\<ModId>\` |

## Magelight.json

Optional; every key has a default. Create it by hand beside `Magelight.log` (a copy in
`Data\SKSE\Plugins\Magelight\` is read only when there is none there). The log names the file
it read, or says that defaults are in effect.

```json
{ "toggleKey": 201, "logLevel": "info", "hotkeys": { "Magelight.Badge/panel": "F4", "Magelight.Badge/badge": 0 } }
```

| Key | Default | Meaning |
|---|---|---|
| `toggleKey` | `201` (Page Up) | DirectInput scancode of the host toggle key, which always leaves UI mode. A number, not a key name |
| `hotkeys` | none | Rebind or disable any mod's hotkey: `"ModId/viewName": "F7"` (a key name or scancode), `0`, `"none"` or `"off"` disables |
| `devMode` | `false` | Hot reload of page files and the on-page JS error banner |
| `logLevel` | `"info"` | `trace`, `debug`, `info`, `warn` or `error` |
| `forceCpu` | `false` | Skip the GPU driver and use Ultralight's CPU renderer |
| `presentHook` | `"auto"` | Where pages are drawn onto the frame. `auto` draws inside dxgi's own Present only when another mod hooked Present first (an upscaler, for one) and could otherwise draw over the pages, never behind a d3d11.dll or dxgi proxy (ENB, ReShade, Skyrim Upscaler) and never on VR. `late` always does, `vtable` never. Try `late` when a page opens (sound, paused game) but stays invisible |
| `composite` | `"auto"` | The stage pages are drawn at. `present` draws them over the finished frame at Present; `ui` draws them in the game's own UI pass, as part of the game's menus (click-through HUD pages stay at Present). `auto` uses `ui` when Skyrim Upscaler is installed (its HUD Fix keeps the game's UI apart from the scene, and frame generation drops anything drawn at Present), never on VR. Try `ui` when a page opens but stays invisible behind an upscaler or frame generation |
| `fontHinting`, `fontGamma` | `"normal"`, `1.8` | Text rendering: `smooth`, `normal`, `monochrome` or `none`; gamma 1.0-3.0 |
| `cursorFile`, `cursorHeight` | `"cursor.png"`, `36` | Cursor art (relative to the runtime folder, or absolute) and its height in pixels at 1080p (8-256) |
| `cursorHotspotX`, `cursorHotspotY` | `0.044`, `0.01` | The pointer pixel of the cursor art, 0-1 across and down |
| `stallWatchdog`, `stallThresholdMs` | `true`, `1500` | Log the present and main threads' stacks when no frame is presented for this long (250-60000 ms); the first time per session, every other thread's too |
| `vr` | see below | Skyrim VR only |

The `vr` block: `enabled` (`true`), `mirror` (`true`: keep drawing pages on the desktop mirror),
`panelFollow` (`false`: a panel stays where it opened; `true` glides it back when you look away,
tuned by `followAngleDeg` 45, `followMinDeg` 80 and `followDistM` 0.5), `panelDistanceM` (1.2),
`panelWidthM` (1.6), `panelHeightOffsetM` (-0.1), `panelWidth` / `panelHeight` (1600 / 900, the
page size of fullscreen views), `keyboard` (`true`: Magelight's own laser keyboard for text
fields), `runtimeKeyboard` (`false`), `beam` (`true`) and `beamAlpha` (0.55) for the laser,
`cursorScale` (0.012) and `cursorDot` (`true`) for its pointer, `suppressRuntimeLaser` (`true`:
hide OpenComposite's own menu laser over Magelight pages), `aimUseTip` (`true`) and
`aimPitchDeg` (-35) for the controller's aim.

## "Magelight VEH" and "Magelight stall" lines

`Magelight VEH: exception ...` records the first few serious exceptions raised on any thread
of the game, including ones another mod raises and handles itself; `Magelight stall: ...`
records the stacks of the present and main threads when no frame was presented for longer than
`stallThresholdMs` (a long save or a hitch can do that). The first sample of a session also
records every other game thread (up to 64, labelled `other`), which names the thread a hang is
waiting on: post the whole block with a hang report. They are diagnostics written only to the log, and on their
own they do not mean Magelight crashed. Nothing is sent anywhere.

## A key typed into a page triggered another mod

On flat, while UI mode is on, Magelight mutes the engine's keyboard device: its DirectInput poll reads and discards the buffered keys instead of turning them into button events, so no keyboard button event exists anywhere in the engine — not for SKSE input sinks, not for mods that hook the input dispatcher (Immersive Equipment Displays' Backspace UI key was the field case), not for Papyrus `OnKeyDown` — and nothing replays into the game when the page closes. The page keeps typing through the window's own key messages, and so do Escape and Magelight's bound hotkeys. It also raises the engine's text-entry flag for the mods that check it. Mouse and gamepad are untouched. On VR the device is not muted (the page is typed from the engine's key events); Magelight takes each key out of the event chain before the input sinks registered after its own. A mod that reads the keyboard through the OS (`GetAsyncKeyState`) can still react; report those with the mod name.
