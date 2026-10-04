# Papyrus tier — the `Magelight` script

`Scripts/Magelight.pex` (source in `papyrus/Magelight.psc`) ships with the host. Every function is
Global; a script needs nothing but `Magelight.psc` in its compile path. Under the hood this is a
thin consumer of the v4 C++ registry, so a Papyrus mod is a real mod: its own slug, its own
storage session, the same UI-mode mutex and hotkey rules as a DLL.

## Minimal mod

Magelight keeps nothing in the save. Mods, views, listeners and hotkeys live in memory and are
gone after the game restarts, so run your setup on every game load, register for the ModEvents
there too, and find the view by name instead of trusting an id saved from an earlier session.
`OnPlayerLoadGame` fires only on a script attached to the player, so put this script on a
ReferenceAlias filled with the player (Specific Reference: `PlayerRef`) in a start-game-enabled
quest.

```papyrus
Scriptname MyScriptModPlayerAlias extends ReferenceAlias

int view

Event OnInit()                ; a new game, or the mod added to an existing save
    Setup()
EndEvent

Event OnPlayerLoadGame()      ; every load
    Setup()
EndEvent

Function Setup()
    RegisterForModEvent("Magelight_ViewDomReady", "OnDomReady")
    RegisterForModEvent("Magelight_JS_save", "OnSave")
    If !Magelight.RegisterMod("MyScriptMod", "My Mod")
        Return                ; Magelight is missing, or a DLL owns this slug (Magelight.log says which)
    EndIf
    ; A second load in the same game session still has the view: find it before creating one.
    view = Magelight.FindView("MyScriptMod", "config")
    If view == 0
        view = Magelight.CreateView("MyScriptMod", "config", "views/config/index.html", 80, 80, 520, 360, "panel")
    EndIf
    Magelight.RegisterListener(view, "save")
    Magelight.BindHotkey(view, 199, 1)      ; Home (DIK 199) toggles UI mode
EndFunction

Event OnDomReady(string eventName, string strArg, float numArg, Form sender)
    If numArg as int == view
        Magelight.Call(view, "setState", "{\"volume\":70}")
    EndIf
EndEvent

Event OnSave(string eventName, string strArg, float numArg, Form sender)
    Debug.Trace("page saved: " + strArg)
EndEvent
```

Pages live in `Data/Magelight/<modId>/` (relative `htmlPath` resolves there), exactly like a
manifest mod. A mod that ships a `manifest.json` gets its views created at data-load with no
script at all; `RegisterMod` with the same slug then **adopts** them, and `FindView(modId, name)`
returns their ids — so the script only has to drive. A manifest mod that a DLL has adopted is
the DLL's: see [Which mods a script can drive](#which-mods-a-script-can-drive).

An **absolute** `htmlPath` must resolve inside the game installation (0.28.6) — the game root or
anywhere under it, e.g. your mod's own `Data\` folder. Anything outside is refused and logged:
a page outside the game root would otherwise pin its whole parent folder as page-readable. A
relative `htmlPath` must be genuinely relative — no leading `\` or `/`, no UNC `\server\share` form
(those replace the mod folder on join instead of extending it) — and may not contain `..` (same
reason); the file must exist inside `Data\Magelight\<modId>\`, or the call is refused (0.30.0). The
C++ tier has none of these restrictions (a DLL can read what it wants anyway).

## Functions

Every call that acts on a mod or a view is refused for a plugin-owned mod, and for a mod whose
storage jar a plugin uses (see [Which mods a script can drive](#which-mods-a-script-can-drive)).

| Function | Notes |
|---|---|
| `GetVersion()` / `GetVersionString()` / `IsReady()` | packed `MAJOR*10000+MINOR*100+PATCH`; `IsReady` false once the overlay disabled itself |
| `RegisterMod(modId, displayName)` | idempotent; adopts a manifest mod that no DLL adopted; `false` when a DLL owns the slug or the mod's storage jar |
| `CreateView(modId, name, htmlPath, x, y, w, h, layer, anchor, clickThrough, startVisible)` | `w = h = 0` → fullscreen; layer `hud`/`panel`/`popup`/`system`; anchor corner + offset. Creates a new view on every call, so `FindView` first |
| `FindView(modId, name)` | a view by name (manifest or `CreateView`); works for any mod |
| `DestroyView` `ShowView` `IsVisible` `ReloadView` | `IsVisible` works for any mod |
| `RequestUIMode(view, pauseGame)` / `ReleaseUIMode(modId)` | one holder at a time; `false` + `GetLastError` when held. Escape leaves UI mode unless the page has taken Escape (`magelight.send('__escapecapture','1')`); the host's toggle key (PageUp unless `Magelight.json` sets `toggleKey`) always does |
| `IsUIModeActive()` / `GetUIModeView()` | work for any mod |
| `Call(view, functionName, argument)` | `window.functionName(argument)` on the page |
| `Eval(view, script)` | raw JS, fire and forget |
| `EvalAsync(view, script, token)` | raw JS with the value back: `Magelight_JSResult` (`token\|result`) or `Magelight_JSError` (`token\|exception`) |
| `BindHotkey(view, dxScancode, action)` | 1 toggle UI mode · 2 toggle UI mode paused · 3 toggle visible · 0 unbind. The host's toggle key (PageUp, 201) is refused. Avoid keys the game binds (F5 Quicksave, F9 Quickload), F12 (Steam's screenshot key) and keys other mods are likely to use: the press that opens a view also reaches the game |
| `SetCutout(view, x, y, w, h)` | see-through rectangle (view pixels); `w = 0` clears |
| `SetHibernate(view, idleMs)` | free a hidden view's texture after `idleMs`; showing reloads it |
| `SetFreezeWorld(view, freeze)` | 0.31.0. While the view holds UI mode with `pauseGame`, the game skips its 3D world render behind it (a frozen frame shows): for opaque fullscreen pages. Kept until changed; `false` on VR or when `Magelight.json` sets `"freezeWorld": false` |
| `RegisterListener(view, name)` | page calls `window.name(payload)` → ModEvent `Magelight_JS_name` |
| `SetNetworkPolicy(modId, policy)` | 0.30.0. `"file"` (the default) or `"loopback"`, case-insensitive; `true` when applied. `"any"`, an unknown word or a plugin-owned mod returns `false`. See [Network](#network) |
| `GetLastError(modId)` | why the mod's last failed host call failed (UI mode held, hotkey taken, …); works for any mod. The ownership refusals below are logged to `Magelight.log` only |
| `PlaySound(view, name)` | a UI sound through the game's audio (0.29.0): `ok`/`click` `cancel` `prevnext` `focus`/`hover` `open` `close` `inactive`, any vanilla `UI*`/`ITM*` descriptor by EditorID (`UIJournalOpen`, `ITMGoldUpSD`, …), `none`, or `Plugin.esp\|0xFormID` of a SNDR; unknown = silent, logged once |

## Events (ModEvents)

Register with `RegisterForModEvent("Magelight_<Event>", "Handler")` in your per-load setup. SKSE
delivers every ModEvent with four arguments, so declare all four:
`Event Handler(string eventName, string strArg, float numArg, Form sender)`. `strArg` is your
modId, or `modId|detail` when the event carries text (`ViewLoadFailed`, `FocusDenied` = the asking
mod, `UIModeRefused` = reason, `UIModeEntered` = `switched` when you retargeted). `numArg` is the
view id. `sender` is always None.

`Magelight_ViewDomReady` `Magelight_ViewLoadFailed` `Magelight_ViewReloaded` `Magelight_ViewDestroyed`
`Magelight_UIModeEntered` `Magelight_UIModeExited` `Magelight_FocusDenied` `Magelight_DisplayResized`
`Magelight_RenderDead` `Magelight_UIModeRefused`

`Magelight_JS_<name>` (from `RegisterListener`) carries the page's payload in `strArg`, and
`Magelight_JSResult` / `Magelight_JSError` (from `EvalAsync`) carry `token|value`; `numArg` is
the view id for all of them.

Events are global: every script hears every mod's events, so filter on the modId in `strArg`.
Console messages are not forwarded (they stay in `Magelight.log`).

## Which mods a script can drive

Papyrus calls carry no caller identity, so since 0.30.0 the host sorts mods by who registered
them:

- **Plugin-owned:** registered through the C++ API (`RegisterMod`/`RegisterModEx`), including a
  manifest mod a DLL adopted.
- **Script-owned:** registered by Papyrus `RegisterMod` or `CreateView`, or declared by a
  `manifest.json` that no DLL adopted.

A script may act only on script-owned mods. With a plugin-owned slug, `RegisterMod`,
`CreateView`, `ReleaseUIMode` and `SetNetworkPolicy` return `false` (or 0) and log one line (once
per function and slug, so a script retrying on every load does not flood the log). On a
plugin-owned mod's view, `DestroyView`, `ShowView`, `RequestUIMode`, `ReloadView`, `Call`, `Eval`,
`EvalAsync`, `BindHotkey`, `SetCutout`, `SetHibernate`, `SetFreezeWorld`, `RegisterListener` and `PlaySound` do
nothing and return `false` (or 0). `FindView`, `IsVisible`, `IsUIModeActive`, `GetUIModeView`,
`GetLastError`, `GetVersion`, `GetVersionString` and `IsReady` still work for any mod. A plugin
that wants scripts to open its panel exposes its own Papyrus native
(`examples/Magelight.InspectTarget` shows how). Before 0.30.0 a script could reuse a plugin's
slug; such a script now gets `false`.

Storage jars are shared by name, so the same refusals cover a script-owned mod whose jar a plugin
uses: a manifest mod with `"session": "default"` (the jar v1-v3 views and plugins that choose it
use), a mod whose modId equals a plugin's custom session name, or a mod whose jar a plugin used in
an earlier session. The host lists every jar a plugin has used (its modId or its custom session
name) in `My Games\<game>\SKSE\Magelight-cache\plugin jars.txt` and keeps scripts out of those
jars even when that plugin is no longer installed, since the jar's data stays on disk. Keep the
isolated default unless a DLL of yours adopts the mod. To give a slug back to scripts after its plugin is gone for good, delete its line from that file
(or the file) with the game closed.

Inside the script tier there is no such line. Papyrus cannot tell one script from another, so
every script can drive every script-owned mod's views: another script can run JavaScript in your
pages and read their storage back. The page storage and page JavaScript of a script-owned mod
are only as private as the script tier. Keep secrets (passwords, API keys, tokens) out of page
storage.

## Network

A page loads from a file and, by default, reads only files under its own mod folder and the
host's runtime folder: nothing over the network. A mod whose pages talk to a server on this
machine (a companion app, a local LLM server) opts in, once per load after `RegisterMod`:

```papyrus
Magelight.SetNetworkPolicy("MyScriptMod", "loopback")   ; http(s) to localhost, 127.x and ::1
```

or with `"network": "loopback"` in its `manifest.json` ([MANIFEST.md](MANIFEST.md)). It applies
to every view of the mod, and each change is logged. Internet reach is a plugin's call: only the
C++ `SetNetworkPolicy` grants `Any`, and a script or manifest asking for `"any"` is refused.
Before 0.30.0 every page could reach loopback without asking.

## Rules that carry over from the C++ tier

- Only the UI-mode holder gets keyboard input; HUD/click-through views never take UI mode.
- Hotkeys never open over an engine menu, the console or game text entry; while a page has key
  focus only its own key fires (to close).
- Pages are file-only unless the mod opts into loopback (see [Network](#network)).
- `Magelight_ViewDomReady` fires again after every reload; re-send state from there.
