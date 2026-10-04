# Manifest mods — `Data/Magelight/<ModId>/manifest.json`

A folder under `Data/Magelight/` is a Magelight mod. Its `manifest.json` names the pages the
host should create at data-load, with no DLL and no script. A static HUD widget is a zero-code
mod; a config page bound to a hotkey is also fully zero-code (hotkeys landed in 0.12.0). A DLL that calls
`RegisterMod` with the same `modId` **adopts** the manifest mod (its views, session and identity
are already there) and can then drive them through the v4 API.

## Layout

```
Data/Magelight/CompassPlus/manifest.json
Data/Magelight/CompassPlus/views/hud/index.html      (+ assets, relative paths)
```

The folder name is the `modId`: leave `modId` out, or give the folder's name (letter case aside). A
manifest whose `modId` differs from its folder name is refused with a log line naming its path, so
a folder whose name is not a valid slug (`My HUD`, say) cannot hold a manifest mod: rename the
folder. A manifest can claim only the slug of the folder it sits in, and the folder is the
identity: a `manifest.json` added to another mod's folder claims that mod's slug, and a DLL that
registers the slug after the manifests load (kDataLoaded) adopts the views it declares. A plugin
whose DLL adopts a manifest mod should ship that manifest itself. Everything the manifest
references must live inside the folder (relative paths only, no `..`).

## Schema

```json
{
  "modId": "CompassPlus",
  "name": "Compass Plus",
  "version": "1.2.0",
  "minHost": "0.11.0",
  "session": "isolated",
  "network": "file",
  "views": {
    "hud": {
      "path": "views/hud/index.html",
      "layer": "hud",
      "anchor": "top-right",
      "x": 24, "y": 24, "w": 320, "h": 96,
      "clickThrough": true,
      "startVisible": true
    },
    "config": {
      "path": "views/config/index.html",
      "layer": "panel",
      "fullscreen": true
    }
  }
}
```

| Key | Default | Meaning |
|---|---|---|
| `modId` | folder name | Must equal the folder name (letter case aside). Slug `[A-Za-z0-9_.-]`, 1..32 chars, unique per load order (case-insensitive) |
| `name` | `modId` | Display name for user-facing messages |
| `version` | `0.0.0` | Informational (`a.b.c`) |
| `minHost` | `0.0.0` | Refused with a log line if the running Magelight is older |
| `session` | `isolated` | `isolated`: this mod's own localStorage/IndexedDB jar under `Magelight-cache/<modId>/`. `default`: the shared jar (only if you must share with a v1-v3 consumer; scripts then cannot drive the mod, since that jar holds plugins' storage). A manifest whose `modId` equals a plugin's custom session name is refused |
| `network` | `file` | Host 0.30.0+. What the mod's pages may reach. `file`: files under the mod folder and the host's runtime folder, nothing over the network. `loopback`: also `http(s)` to this machine (`localhost`, `127.x`, `[::1]`), e.g. a companion app's local server. `any` is refused with a log line and the mod stays file-only: internet reach is a plugin's call (C++ `SetNetworkPolicy`). Before 0.30.0 every page could reach loopback; a mod that relied on that now declares `"loopback"` |
| `views.<name>.path` | required | Page, relative to the mod folder |
| `layer` | `panel` | `hud` < `panel` < `popup` < `system` (z-order tier; `hud` never takes UI mode) |
| `anchor` | `top-left` | `top-left` `top-right` `bottom-left` `bottom-right` — `x`/`y` offset from that corner |
| `x` `y` `w` `h` | 0 | Pixels; `w`/`h` required unless `fullscreen` |
| `fullscreen` | false | Tracks the backbuffer; geometry ignored |
| `clickThrough` | false | Never receives input, never focused (HUD widgets) |
| `startVisible` | false | Shown as soon as it materializes |
| `hotkey` | — | A DirectInput scancode number or a name: `F1`..`F12`, `Insert` `Home` `Delete` `End` `PageDown`, `Numpad0`..`Numpad9`, `Backslash` `Grave` `Minus` `Equals` (`0`, `none` or `off` = no key). A `hud`/click-through view toggles visibility; any other view toggles UI mode (opens with cursor + suspended controls; closes on the same key, or on Escape unless the page has taken Escape, and always on the host's toggle key). `PageUp` is the host's toggle key and is refused. Pick a key neither the game nor the host uses (not F5 Quicksave, F9 Quickload, F12 Steam's screenshot key or PageUp) and one other mods are unlikely to use: the press that opens a view also reaches the game. One binding per key process-wide; a clash is logged and the first binder keeps it. Players can rebind any view's key in `Magelight.json` (`"hotkeys": { "ModId/viewName": "Insert" }`) |
| `vr` | — | VR only. Per-view placement: `{ "mode": "lazy"｜"head"｜"world", "distance": 1.6, "width": 1.4, "heightOffset": -0.15 }`. `lazy` (default for panels) places the panel level in front of you and lets it stay put until your gaze drifts ~30° or you move ~0.5 m, then it glides back. `head` glues it to your view (HUD widgets). `width` is the panel's width in metres; its height follows the page aspect. Ignored on a flat runtime |
| `vrHotkey` | — | VR only. Open the view with a CONTROLLER instead of a key: `{ "button": "a", "modifier": "grip", "hand": "either" }`. Buttons: `menu` (B/Y), `grip`, `a` (A/X), `stick`, `trigger`, `touchpad`. `hand`: `either`｜`left`｜`right`. A bare button with no `modifier` also fires during gameplay, so prefer a modifier. Same gates as a key binding: never over an engine menu, the console or game text entry |
| `hotkeyPause` | false | With `hotkey` on a non-hud view: pause the game while the view holds UI mode |
| `loadOnShow` | false | Host 0.31.0+. The page is not loaded until the view is first shown (its hotkey, a script's `ShowView`/`RequestUIMode`, a driving DLL): no load cost for a panel that is rarely opened. Its DOM ready, and anything a driver queued for the page, come after that first show. A `startVisible` view loads at once anyway |
| `hibernateMs` | 0 | Hidden this long → the view frees its texture; showing it reloads the page. Use on panels that are closed most of the time |
| `sounds` | — | `{ "open": "open", "close": "close" }` — played by the host through the game's audio when this view enters / leaves UI mode (a page cannot play sound itself: Ultralight has no media stack). Names: `ok`/`click`, `cancel`, `prevnext`, `focus`/`hover`, `open`, `close`, `inactive` (the vanilla UIMenu* sounds), any vanilla `UI*`/`ITM*` descriptor by EditorID (`UIJournalOpen`, `ITMGoldUpSD`, …), `none`, or `Plugin.esp\|0xFormID` of any SNDR you ship. Button sounds need no code: put `data-ml-sound="click"` (on click) or `data-ml-sound-hover="focus"` (on hover, throttled) on the element. Host 0.29.0+ |
| `dev` (top level) | false | Hot reload of PAGE files (HTML/CSS/JS) under the mod folder, after a 400 ms quiet window. Editing `manifest.json` itself (geometry, hotkeys, adding a view) needs a game restart — those are read once at load. Ship it `false` |

Unknown keys are ignored and logged (with a did-you-mean for a case or spelling slip); a wrong-typed value (`"w": "320"`) is logged and the default used. Every problem is logged with the file name and the view name; a bad
view never stops the other views or other mods.

## What happens at runtime

- kDataLoaded: the folder is scanned, the mod registered, views created (they load in the first
  in-game frames: since 0.31.0 a hidden view waits its turn and the views start one per frame, a
  view being opened first; a `loadOnShow` view waits for its first show). `Magelight.log` prints
  one line per manifest and one per error.
- Pages see `window.__MAGELIGHT__` and can call any listener a driving DLL or script registered on them.
  Without a driver, a page is static (or self-contained JS).
- A DLL adopting the mod gets its `ModId` back from `RegisterMod` and can `RequestUIMode`,
  `RegisterJSListenerEx`, `ReloadView`, etc. on the manifest's views. Resolve a view's
  id from its name with `FindView(mod, name, &id)` (0.26.8+; the Papyrus tier's `FindView`
  is the same call) — or `CreateViewEx` more of your own. Adoption makes the mod
  plugin-owned: from then on a script can only read it (`FindView`, `IsVisible`, …), not drive
  it. A manifest mod no DLL adopts is script-owned, and any script may drive its views
  ([PAPYRUS.md](PAPYRUS.md#which-mods-a-script-can-drive)). Adopt at `kDataLoaded` at the
  latest: a script that touches the mod first (`RegisterMod`, `CreateView`,
  `SetNetworkPolicy`) owns it for the session, and your `RegisterMod` then returns `InvalidMod`.

## Sessions

Each mod's views run in an Ultralight Session named after the mod, so `localStorage`,
IndexedDB and cookies never collide between mods. The jar lives under
`My Games\Skyrim Special Edition\SKSE\Magelight-cache\<modId>\` (`Skyrim VR` on VR,
`Skyrim Special Edition GOG` on GOG). Views created through the
v1-v3 API stay in the shared `default` session (SeverActions relies on that today).

The jar belongs to the install, not the save: every save, character and mod-manager profile
shares it, and loading a save does not roll it back. Keep per-playthrough state in the save
(through your script or DLL).

Separate jars keep mods from colliding; they are not a secret store. Any DLL can run JavaScript
in any view, and any script in any view of a script-owned mod (a manifest mod no DLL adopted);
either can read the page's storage that way. Keep secrets (passwords, API keys, tokens) out of
page storage.
