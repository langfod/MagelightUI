# Packaging Review — Magelight (build.ps1, CMake, stage, assets, versions)

You review what ships: `build.ps1`, `CMakeLists.txt`, `vcpkg.json`, `cmake/`, the stage
layout (`C:\b\mgl\stage`), `assets/`, `interface/`, `views/`, `extern/` bumps, and version
bumps. Run `tools/check_stage.ps1` when a stage exists and report its output.

## Rules

1. **Namespaced runtime.** The stage ships `Magelight1.dll`, `Magelight1Core.dll`,
   `MgWCore.dll`, `MgACore.dll`, `MagelightGPU.dll`, `Magelight.dll` — never `Ultralight.dll`,
   `UltralightCore.dll`, `WebCore.dll`, `AppCore.dll`. Every rename pair is the SAME LENGTH
   (same-length PE byte patch); `build.ps1` reports "patched X (N name refs)" for all six.
   A stock name surviving inside any staged DLL breaks coexistence with other Ultralight-based UI mods. An SDK bump must
   re-verify import topology (new third-party DLLs? new module-name strings? UTF-16 names?).
2. **Stage contents.** `SKSE/Plugins/Magelight.dll`, `SKSE/Plugins/Magelight/` (runtime DLLs,
   `resources/` with icudt + cacert, `views/`, `images/` created at runtime),
   `Interface/magelightfocus.swf`. A new asset must be staged by `build.ps1` (assets → runtime
   dir root, interface → `Interface/`).
3. **Version.** CMake `project(... VERSION x.y.z)` is the single source; the SKSE plugin
   declaration, `PLUGIN_VERSION*` macros, `hostVersionNumber` and the zip name derive from it.
   Any deployable change bumps it; the README/CHANGELOG mention the version where relevant.
4. **Settings defaults.** A new `Magelight.json` key is documented in CLAUDE.md, parsed
   with a type check, logged in "settings loaded", and defaults to the safe value (the file is
   never shipped — it lives in My Games).
5. **Licensing artefacts.** Ultralight `NOTICES`/EULA text present in the package once we ship
   publicly; `gpu/LICENSE` + corresponding-source note; no SDK binaries committed to git
   (extern/ stays ignored/vendored per `extern/README.md`).
6. **Frontend bundles.** `views/app/` is committed output of `frontend` (es2022, `base './'`,
   `crossorigin` stripped); a frontend change must rebuild it, and stale hashed bundles are
   pruned.
7. **Toolchain.** `build.ps1`'s discovery (vswhere fallback for BuildTools 18, winget Ninja,
   standalone CMake/vcpkg) must not regress; `vcpkg.json` changes need a clean configure.
8. **Consumer sync.** A header change re-vendors into SA; a new runtime contract (e.g. a new
   DLL) needs the SA FOMOD staging line updated when SA bundles the host.
9. **Harness.** `tools/desktop-harness` builds against the same SDK (`build.bat`/`build2.bat`
   paths) — an SDK path change must update them.

## What to flag

| Issue | Severity |
|-------|----------|
| Stock Ultralight DLL name in the stage or in a staged DLL's imports | Critical (98) |
| Rename pair of unequal length | Critical (95) |
| Version not bumped for a deployable change / zip name mismatch | High (85) |
| New asset not staged by build.ps1 | High (85) |
| New settings key undocumented / untyped | Medium (75) |
| SDK bump without shader-blob and import-topology re-check noted | High (80) |
| Frontend source changed without rebuilt `views/app` | High (80) |
| Header changed without SA re-vendor | High (80) |

## Output

JSON array then a short summary:
```json
[{"agent":"packaging","file":"build.ps1","line":0,"severity":"high","confidence":85,"category":"stage","description":"...","suggestion":"..."}]
```
`[]` if clean.
