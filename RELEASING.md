# Releasing Magelight UI

The mechanics of a public build. Everything except the smoke test is scripted; everything
scripted fails loudly rather than shipping a bad zip.

## 0. Before every release: the license basis

- **Ultralight licence.** A public release relies on section 2.3 (Limited Commercial
  Distribution) of the Ultralight Free License Agreement; section 2.2 covers academic
  research only. Section 2.3 is void for a company or incorporated entity with a turnover
  above US$100,000 in its last fiscal year. The licensee is Magelight's author, an individual
  under that limit. If that ever changes, stop and talk to Ultralight, Inc. before releasing.
- **The DLL rename.** The four Ultralight DLLs ship renamed, with the module-name strings
  inside them rewritten in place, under Ultralight, Inc.'s written permission. Keep that
  permission with the release records, and re-verify the rename (`tools/check_stage.ps1`)
  on every SDK bump.

## 1. Version and changelog

- Bump `project(... VERSION x.y.z)` in `CMakeLists.txt` — the single source (the DLL
  version resources, `SKSEPlugin_Version`, the zip name and the stage lint all read it).
- `CHANGELOG.md` gets the entry; the "added in" table is how consumers pick `minHost`.
- This release's tag is named as the LGPL source for `MagelightGPU.dll` in `NOTICES.txt`
  section 2, the Licensing section of `README.md`, and the Credits of the
  Nexus page (kept outside the repository): update the version in all three. The
  example log line at the top of `docs/TROUBLESHOOTING.md` names it too.
- The npm packages version with the host (`docs/SDK.md`); publish them per section 6 when
  the page contract changed.

## 2. Build and package

```
npm ci
npm run build
powershell -ExecutionPolicy Bypass -File build.ps1 -Clean -Examples
powershell -ExecutionPolicy Bypass -File tools\package.ps1 -Version X.Y.Z
```

- `extern/ultralight-free-sdk-1.4.1-dev-win-x64/` is not in git. Fetch it per
  `extern/README.md`: sign in at <https://ultralig.ht/download>, choose **1.4.1-dev**, and
  extract the free-edition archive into `extern/` under its own name. **It is a login-gated
  developer build — there is no public CDN or `curl` URL for it**, so this cannot be scripted;
  a copy from the machine that built the last release is equally good.
  `build.ps1` refuses any SDK whose `VERSION.txt` is not the pinned build of record
  (`1.4.1-263f6bd`).
- **CI's SDK is the Actions cache entry `ultralight-sdk-1.4.1-263f6bd`**, saved by a passing
  run on `main` and restored by every branch and pull request. Because the build is
  login-gated, that cache is the **only** unattended source — on a cache miss with no
  `sdk_url` the job fails with the procedure rather than downloading something else. GitHub
  removes a cache unused for seven days (or when the repo's caches pass their size limit), so
  this will need re-seeding after quiet periods. To re-seed it:
  1. Repack a verified copy (its `VERSION.txt` reads `1.4.1-263f6bd`) from the repository
     root, keeping the SDK's own top-level folder **inside** the archive — the workflow
     extracts into `extern/` and expects that folder to land at
     `extern\ultralight-free-sdk-1.4.1-dev-win-x64\`:
     `7z a extern\ultralight-sdk-1.4.1-dev.7z .\extern\ultralight-free-sdk-1.4.1-dev-win-x64\*`.
     Put the archive in `extern\`: `.gitignore` covers `extern/*.7z` and not a `.7z`
     at the repository root.
  2. Put it behind a link the runner can download without credentials. Keep the link private
     and short-lived: it appears in the run's log, and the SDK is Ultralight's, not a
     download for us to publish.
  3. Actions → CI → Run workflow on `main`, with `sdk_url` set to that link. The step
     checks `VERSION.txt` against the pin (and nothing else, so pack only a copy you trust),
     and the cache is saved only if the whole job passes. Once it has, take the link down
     and delete `extern\ultralight-sdk-1.4.1-dev.7z`.

  Moving the pin instead is a deliberate change: `git grep -n 263f6bd` lists every place
  that names it (`build.ps1`, the CI cache key and check, `extern/README.md`), and the new
  build needs section 0's rename check and the notices re-verified. The SDK folder name
  carries the version too (`ultralight-free-sdk-<version>-dev-win-x64`), so it moves with the
  pin — as do `CMakeLists.txt`, `build.ps1`'s `$ulDir`, the CI paths, the harness `.bat`
  files, and `.gitignore`.
- `npm ci && npm run build` at the repo root first: it builds the `packages/` the
  ReactConfig example compiles against. On a fresh clone without it, `build.ps1` only
  warns and stages the example without its page, so check its output for
  `example Magelight.ReactConfig: built`.
- `-Clean` because the stage is shared build output — package from the branch you mean to
  ship (the packager also refuses a version mismatch against the staged DLL).
- Packaging runs `tools/check_stage.ps1` as a **release gate**: namespaced-runtime patch
  verification, required files, version coherence of both DLLs. A stage that fails lint
  does not zip. A release must carry `Magelight.pex`, so package on a machine with the
  Papyrus compiler (the game installs it).
- Output in `dist\`, each a plain Data-root zip that a mod manager installs with no
  installer: `MagelightUI-X.Y.Z.zip` (the runtime, the only thing a player needs),
  `MagelightUI-X.Y.Z-DevTools.zip` (the Web Inspector and the host's dev pages) and
  `MagelightUI-X.Y.Z-Examples.zip` (the example mods), plus `MagelightUI-X.Y.Z-pdb.zip`
  (symbols — **keep with the release, never ship to players**; the crash diagnostics'
  offsets decode only against the exact build's PDBs).

## 3. Smoke test (manual — the step you do not skip)

Install the runtime zip and then the Examples zip into a **clean MO2 profile** (no earlier
Magelight, current SKSE):

- The game launches; `Magelight.log` line one names the new version.
- F6 shows and hides the `Magelight.Badge` badge, F7 opens its panel.
- `Magelight.ReactConfig` opens with F8: UI mode takes the cursor, typing lands in the
  page, Escape closes, game controls resume.
- A `Magelight.json` override (`toggleKey`) applies.
- If the release touched the VR presenter: one head-locked quad opens/closes on
  SteamVR-native AND on OpenComposite.
- If the CommonLib pin (`ports/commonlibsse-ng`) changed since the last public release,
  smoke-test **every runtime**: SE 1.5.97, AE 1.6.x, AE 1.7.x and VR 1.4.15. On each,
  `Magelight.log` shows line one, `renderer created`, and one view opens.

## 4. History

The public repository began at its first public release, without the private repository's history.

## 5. Tag and publish

From the first public release on, every release is cut from the public repository's own history: work lands
there by pull request, and the release commit is a commit on its `main`. Nothing from the
private repository is ever pushed to the public one: a pushed commit carries its whole
ancestry, so a branch or tag made in a private clone would publish that clone's history.

- CI is green on the release commit.
- Annotated tag on the release commit, in a clone of the public repository:
  `git tag -a vX.Y.Z -m "Magelight UI X.Y.Z"`, then `git push origin vX.Y.Z` once
  `git remote -v` shows `origin` is `https://github.com/Severause/MagelightUI.git`.
- No GitHub release: mod authors build the DevTools and examples from the repository. Keep
  the `-pdb.zip` with the release records (never ship it to players).
- npm, when the packages changed: section 6.
- Nexus: only after the tag exists (NOTICES names `gpu/` at the tag as the LGPL source for
  `MagelightGPU.dll`). The runtime zip is the only file. Set the requirements per runtime as
  the page lists them.

## 6. npm packages

The four packages (`@magelight/sdk`, `@magelight/react`, `@magelight/vite-plugin`,
`create-magelight-view`) version with the host whose page contract they match, bumped only
when that contract changes (docs/SDK.md). On 0.x a caret range stops at the next minor
(`^0.29.0` means below 0.30.0), so every range has to move with the version.

1. On the release branch: set `version` in `packages/sdk`, `packages/react`,
   `packages/vite-plugin`, `packages/create-magelight-view` and the root `package.json`.
   Move `@magelight/react`'s peer range on `@magelight/sdk` (`>=X.Y.0`) and its dev range
   (`^X.Y.0`), and every `@magelight/*` range in `packages/create-magelight-view/template`
   and `examples/Magelight.ReactConfig` to `^X.Y.0`. Run `npm install` at the root (not
   `npm ci`) to regenerate `package-lock.json`, and commit it with the bump.
2. From the merged release commit with a clean `git status`: `npm run clean && npm run build`.
3. `npm pack --dry-run` in each package folder; check the file list and the version.
4. The registry shows the publishing account's email address to anyone who queries it
   (`npm view <package> maintainers`). Check which address that is before publishing:
   `npm profile get email`, and change it with `npm profile set email <address>` or on
   npmjs.com.
5. Publish in dependency order, `npm.cmd publish` from each folder, each asking for a 2FA
   code: `packages/sdk`, then `packages/react` (its build and its peer need the new SDK),
   then `packages/vite-plugin`, then `packages/create-magelight-view` last (its template
   asks for the other three at the new version).
6. Check: `npm view <package>@X.Y.Z version` for all four, and in a temp folder
   `npm create magelight-view@X.Y.Z T`, then `npm install` and `npm run build` in it.
