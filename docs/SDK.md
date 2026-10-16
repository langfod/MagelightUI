# The frontend tier — `@magelight/*`

Three npm packages and a scaffolder, all in `packages/` (npm workspaces; `npm run build` at the
repo root builds them). They are the field-hardened seed of SeverActions' `skse-api.ts` and
`build-ui.ps1`, turned into something any mod can install.

| Package | What |
|---|---|
| `@magelight/sdk` | `host` (presence, version, ids, capabilities, image URLs, `host.sound`), `bridge.on/send`, `channel<T>()`, pre-mount replay, browser mock with a floating panel |
| `@magelight/react` | `useChannel`, `useChannelEvent`, `useUIMode`, `useCapability`, `useHost`, `useSend`, `useSound`, `<HostGate>` |
| `@magelight/vite-plugin` | file:// safe output (`base './'`, es2022, no `crossorigin`), per-entry pruned `views/<name>/` folders |
| `create-magelight-view` | `npm create magelight-view MyMod` → manifest mod + React config page |

## The page contract (host side)

At window-object-ready, before any page script, the host installs:

```js
window.__MAGELIGHT__ = { version, versionNumber, viewId, modId, viewName, capabilities, dev, runtimeUrl }
window.magelight     = { send(channel, payload), on(channel, fn) → off, off(channel, fn), pending(channel), sound(name) }
```

- **Host → page**: `InteropCall(view, channel, text)` calls `window.<channel>(text)` if the page
  defined one (the classic shim), otherwise routes into `magelight._dispatch(channel, text)`,
  which **buffers until someone subscribes**. Nothing is lost to a late mount.
- **Page → host**: `magelight.send(channel, payload)` reaches the listener the mod registered
  for that channel (`RegisterJSListenerEx` / Papyrus `RegisterListener`). Objects are JSON text
  on the C++ side.
- **`__uimode`**: the host dispatches `'1'`/`'0'` when the view gains/loses UI mode.
- **`__sound`** (host 0.29.0): `magelight.sound(name)` / `send('__sound', name)` plays a UI sound through
  the game's audio (a page cannot itself); `hover:<name>` is throttled. `data-ml-sound="click"` /
  `data-ml-sound-hover="focus"` markup does it with no script. `capabilities.sound` says the host has it.
  The SDK's `host.sound` and React's `useSound()` call `magelight.sound` and do nothing on an older host.

Plain-HTML pages use the two verbs directly; the SDK types them and adds the mock for browser
development. Neither the SDK nor React is required — the contract is the two globals.

## What a page can reach

- **Files**: the union of every page's mod folder plus the host's runtime dir, nothing else.
  The file gate is view-blind (WebKit asks for files with no View in hand), so it cannot keep one
  page out of another's folder — a page reads any registered root. `capabilities.csp` says the
  network policy rides on a Content-Security-Policy written into every page (0.31.5).
- **Network**: nothing by default (file-only, since 0.30.0; before that the default also let
  a page reach this machine). The policy is now a Content-Security-Policy written into the page:
  a page cannot `fetch()` its own files, and a file-only page cannot open a WebSocket or read disk
  through synchronous XHR (Ultralight routes those past the request hook). A mod opts in for
  itself: the manifest key `"network": "loopback"`, or Papyrus `SetNetworkPolicy(modId,
  "loopback")` for a mod scripts own, adds http(s) and ws(s) to this machine (localhost and
  127.0.0.1 only — CSP cannot name `127.x.x.x` or `[::1]`, so a server at `[::1]` moves to
  localhost). Only a DLL can grant internet reach (C++ `SetNetworkPolicy(mod, Any)`). A page that
  fetches from a local server needs one of these.
- **Storage**: `localStorage`, IndexedDB and cookies are per mod by default (a mod that names
  a shared session, or `"default"`, shares that jar). They are not per save or per character,
  and they are only as private as the tier that owns the mod. Scripts cannot act on the views
  of a mod a DLL registered or adopted, nor of a mod whose jar a plugin uses. Papyrus cannot
  tell scripts apart, though, so any script can run JavaScript in, and read the storage of, any
  other mod that scripts own (a manifest mod no DLL adopted, or one a script registered). Keep
  secrets out of page storage.
- **Your own view only**: `magelight.send` reaches the listeners of the view the page is in; a
  page cannot call another view's (0.30.0).

## Versions and publishing

The packages are on npm and version with the host they match: 2.0.0 now. At each host
release that changes the contract, bump all four `packages/*/package.json` versions and every
cross-package range with them (`@magelight/react`'s sdk peer and dev ranges, the
`create-magelight-view` template, `examples/Magelight.ReactConfig`). From 1.0 a caret range
takes every later 1.x (`^1.0.0` installs 1.4.0), which the append-only page contract allows;
a new major moves every range. Before 1.0 a caret stopped at the next minor, so the 0.x
packages (`^0.30.0`) never install 2.0.0. Then run `npm install` at the
repo root (not `npm ci`) to refresh the lockfile, and `npm run clean && npm run build`. Each
package carries `prepack`, so its tarball always builds first, and the scoped packages publish
as public through `publishConfig`. Publish in dependency order: sdk, react, vite-plugin,
create-magelight-view.

## Scaffolding from a clone (`--local`)

`npm create magelight-view MyMod` installs the packages from npm. To scaffold against a clone
instead (unpublished package changes), build the workspace at the repo root
(`npm install && npm run build`) and run
`node packages/create-magelight-view/index.js MyMod --local`. The scaffold's `@magelight/*`
dependencies then point at the clone's package folders, and the template's
`resolve.dedupe` keeps Vite on one copy of React.
