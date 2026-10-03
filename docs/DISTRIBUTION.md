# Shipping a mod that depends on Magelight UI

Your mod needs Magelight's runtime (the host DLL + the Ultralight runtime) to
be present. There are two ways to ensure that; pick one and tell your users
which.

## Option A — require Magelight as a separate download (simplest)

List **Magelight UI** as a requirement on your mod page and link it. Your mod
ships only your own files (`Data\Magelight\<YourMod>\...`, your ESP/DLL). The
user installs Magelight once; several Magelight mods share it.

- Pro: no duplication, no licensing surface of your own, updates to Magelight
  reach every mod at once.
- Con: one more required download; a user on too-old a Magelight sees your
  mod's views refused (the host logs the reason and shows the "too old" gate
  page). Set `minHost` (manifest) / `minHostVersion` (C++) to the oldest host
  your features need — see [CHANGELOG.md](../CHANGELOG.md).

## Option B — bundle Magelight inside your FOMOD (self-contained)

Stage Magelight's runtime as **required core** files in your own installer, so
one download installs both. This is what SeverActions does. The pattern:

1. Take the files from a published Magelight release's runtime zip (the main
   file on the Magelight UI Nexus page). A released build matches its
   crash symbols and the source tag its notices name; a local `build.ps1`
   stage is for testing.
2. Stage the **runtime files only** into a folder in your FOMOD tree, e.g.
   `98 Magelight UI\00 Core\` containing `SKSE\`, `Interface\`, `Scripts\`.
   Keep: `Magelight.dll`, the renamed Ultralight DLLs + `MagelightGPU.dll`,
   `resources\` (ICU data is mandatory), `views\gate` + `views\keyboard`,
   `Interface\magelightfocus.swf`, the Papyrus tier, and the
   whole `license\` folder. Drop: the Web Inspector, examples, `Source\`, the
   `views\probe`/`app` dev pages. (The runtime zip is exactly this set.)
3. Reference it from your `fomod/ModuleConfig.xml`. As **required core** it is
   one `<folder source="98 Magelight UI/00 Core" destination="" priority="1"/>`
   under `<requiredInstallFiles>`; stripping that prefix lays `SKSE\` /
   `Interface\` / `Scripts\` at the Data root.
4. Write a greppable `SKSE\Plugins\Magelight\VERSION.txt` naming the Magelight
   version you shipped. The standalone release writes the same file, so
   whichever copy wins the runtime files in a mod manager also wins the stamp.

- Pro: one download, no external requirement.
- Con: you carry the licensing obligations below, and you must keep the bundle
  reasonably current.

## The MO2 two-copies problem

If a user installs your bundle **and** a standalone Magelight (or two bundling
mods), MO2 gives each file both copies carry to the mod that sits lower in the
left pane. The runtime files are the same set in every copy, so the lower mod's
Magelight is the one that runs; an **older** copy winning silently drops newer
features, and mods that need the newer host show the "needs updating" page.
Tell users: if they have Magelight from more than one source, let the
**newest** copy win (in MO2, place it lower in the left pane) or keep only one.
Line one of `Magelight.log` names the version that is running, and a
`VERSION.txt` makes "which is newer" checkable before launch.

## Licensing you inherit when you bundle (Option B)

Magelight embeds **Ultralight** (a commercial WebKit engine). If you
redistribute Magelight's runtime inside your mod, you are redistributing
Ultralight's runtime, and it reaches your users under Ultralight's End User
License Agreement:

- **Ship the whole `license\` folder** unchanged. Ultralight's licence
  (`Ultralight-LICENSE.txt` §4.3) permits distribution to end users only under
  its EULA (`EULA.txt`), and §4.4 requires the notices to accompany the
  binaries. The GPU backend derives from LGPL code, so `LGPL-2.1.txt` must
  travel with `MagelightGPU.dll`. `NOTICES.txt` lists what's included, and
  `THIRD_PARTY_LICENSES.txt` carries the texts of the libraries compiled into
  the DLLs.
- **Ship Magelight's files as they come.** The four Ultralight DLLs are
  Ultralight 1.4.0b's, renamed, with the module-name strings inside them
  rewritten in place so they import each other under the new names, with the
  written permission of Ultralight, Inc. to Magelight's author. That
  permission is the author's, not yours: never rename or patch Ultralight's
  DLLs yourself, and do not strip the notices.
- **The LGPL source.** `MagelightGPU.dll` is LGPL-2.1; its corresponding source
  is the `gpu/` folder of the Magelight UI repository at the release's tag,
  which `NOTICES.txt` names. Bundle a published release so that tag exists, and
  keep `NOTICES.txt` with the DLL.
- Bundling makes you a distributor of Ultralight's runtime: read
  `Ultralight-LICENSE.txt` and `EULA.txt` before you do, and credit Ultralight
  on your mod page the way Magelight does (its copyright line and the §4.4
  legend "Please see the accompanying NOTICES.txt for full text.").
- If you would rather not take on Ultralight's redistribution terms yourself,
  **Option A (require, do not bundle)** is the conservative choice for a
  third-party author, because requiring a separately-installed Magelight puts
  the redistribution on the Magelight project, not on you.

Your own mod's code (your pages, your ESP, your plugin) is yours to license
however you like — the obligations above are only about the Magelight/Ultralight
binaries you redistribute. If your pages are built with React (the SDK
template), their JavaScript carries React, whose MIT notice goes with your mod.

## Versioning your dependency

Pin the **lowest** Magelight you actually need in `minHost` / `minHostVersion`,
not the newest — that maximises the set of users who can run your mod. The ABI
is append-only, so a newer host keeps every function an older `minHost`
promised. Sandbox rules can tighten in any release (0.30.0 made network access
file-only by default), and the CHANGELOG marks such changes **Breaking**. When
you adopt a feature, bump `minHost` to the version that added it (the
CHANGELOG's "Added" column) and say so on your mod page.
