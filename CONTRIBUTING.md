# Contributing to Magelight UI

Thanks for helping. The short version: keep the ABI append-only, respect the invariants,
and check rendering changes in the desktop harness before asking for a game test.

## Building the host

1. Fetch the Ultralight 1.4.1-dev SDK into `extern/ultralight-free-sdk-1.4.1-dev-win-x64/` —
   see [extern/README.md](extern/README.md). It is a **login-gated developer build**: sign
   in at <https://ultralig.ht/download>, choose `1.4.1-dev`, and extract the free-edition
   archive into `extern/`. There is no public CDN or `curl` URL for it, so this step is
   manual by design (~90 MB of third-party binaries, fetched not committed).
2. Install standalone vcpkg (`git clone https://github.com/microsoft/vcpkg C:\vcpkg`, then
   `C:\vcpkg\bootstrap-vcpkg.bat`), or set `VCPKG_ROOT` to an existing standalone one;
   `build.ps1` falls back to `C:\vcpkg` and ignores the copy Visual Studio bundles.
   Then `powershell -ExecutionPolicy Bypass -File build.ps1` — configures with CMake/Ninja/MSVC
   + vcpkg, builds `Magelight.dll` and `MagelightGPU.dll`, and stages an installable
   layout at `C:\b\mgl\stage`. Add `-Examples` to stage the example mods too. It refuses
   any Ultralight SDK but the pinned build of record, and compiles `Magelight.pex` with the
   game's Papyrus compiler, found through Steam's library folders or the
   `MAGELIGHT_SKYRIM_DIR` environment variable (the Skyrim Special Edition game folder).
3. `tools/check_stage.ps1` lints the stage (namespaced runtime names patched everywhere,
   required files, version coherence). It runs in CI on every PR.

### The Ultralight SDK pin

`build.ps1` and CI build only against the SDK build of record, `1.4.1-263f6bd`. That build is
**login-gated** — Ultralight distributes it only to a signed-in browser session at
<https://ultralig.ht/download>, with no public CDN, no `curl` URL and no token download, so it
cannot be fetched unattended at all. That shapes everything below:

- **Locally**, you supply the SDK yourself (step 1 above). A copy from another machine that
  has it is equally good; the folder name carries the version, so `build.ps1` and the pin
  check catch a mismatch. If you genuinely need a different build, open an issue: moving the
  pin is the maintainer's call, because the folder name, the renamed runtime and the notices
  all have to be re-verified against it (RELEASING.md sections 0 and 2).
- **In CI**, pull requests build against the pinned SDK the Actions cache holds from `main`.
  That cache is the only unattended source. If it has expired the job fails with the
  procedure on every run until a maintainer re-seeds it (RELEASING.md section 2); nothing in
  a pull request can fix it.

The npm packages need no game: `npm ci && npm run build` at the repo root builds all
of `packages/` and the React example (run it before `build.ps1 -Examples`, which builds
the example against them). Scaffold a throwaway mod against your clone with
`node packages/create-magelight-view/index.js MyTest --local`.

## Testing a change

- **Rendering / page behavior:** `tools/desktop-harness` runs the GPU driver (or AppCore's
  reference) on any page without the game — always the first stop. See the README in that
  folder.
- **In game:** deploy the stage to a test install (game closed) and check `Magelight.log`
  in `Documents\My Games\Skyrim Special Edition\SKSE\` (`Skyrim VR` on VR,
  `Skyrim Special Edition GOG` on GOG). There is no automated test suite yet — say in the
  PR what you ran.

## The rules that get PRs rejected

- **The ABI is append-only** (`api/MagelightUI_API.h`). New calls go at the struct tail,
  the export table initializer stays in struct member order, and
  `python tools/check_abi.py` must pass (CI enforces it). Never reorder, retype, or
  renumber — a swapped pair compiles fine and calls the wrong function in every consumer.
- **The invariants in [CLAUDE.md](CLAUDE.md) are law.** Each one cost a debugging arc:
  one Ultralight thread, the load-listener locking rules, view lifetime in
  `ApplyPendingLifecycle`, POD-only frames inside SEH guards, keyboard containment in UI
  mode. Read them before touching `src/`.
- **The LGPL seam stays clean.** AppCore-derived code lives in `gpu/` only, reached from
  the host only through `gpu/MagelightGpuApi.h` (C ABI), modifications marked `// MG:`.
- **Bump the version** (`CMakeLists.txt` `project(... VERSION x.y.z)`) and add a
  CHANGELOG.md entry for every deployable change. The "added in" table is how consumers
  pick their `minHost`. The full release checklist is [RELEASING.md](RELEASING.md).
- Docs and code ship together: behavior changes update MANIFEST/PAPYRUS/CPP/SDK docs in
  the same PR.

## Style

Match the file you are in. Comments say only what the code cannot: why, the contract
other code relies on, a warning that stops a known regression. One or two sentences, no
history (dates and incidents belong in the commit message or the CHANGELOG). No
reformatting sweeps, no drive-by refactors — a tidy, reviewable diff.

## Licensing of contributions

Contributions are accepted under the licence of the folder they land in: MIT for the
repository, LGPL-2.1 for `gpu/`.

## AI-assisted contributions

Welcome — this project is developed heavily with AI assistance. If you use an agent,
point it at [CLAUDE.md](CLAUDE.md) (architecture, invariants, build) and the review
rubrics in [.claude/agents/](.claude/agents/) (threading, ABI, GPU driver, packaging,
frontend, docs), and say in the PR that it was involved.

## Security issues

Never put the details of a security problem in a public issue — see
[SECURITY.md](SECURITY.md).
