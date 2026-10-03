# extern/

## ultralight-free-sdk-1.4.1-dev-win-x64/ (not committed)

The Ultralight **1.4.1** SDK, build of record `1.4.1-263f6bd` (see
`VERSION.txt` inside the SDK; `build.ps1` refuses any other build, so moving
to a new one is a deliberate edit of that pin).

**1.4.1 is a login-gated developer build — there is no public CDN or `curl`
URL for it.** (The open dev CDN that served 1.4.0b went away when 1.4.1
landed; the release CDN's `latest` still serves 1.3.0, checked 2026-09-01.)
Getting it takes a human at a browser:

1. Sign in at **<https://ultralig.ht/download>**.
2. Select the **1.4.1-dev** release. The free-edition Windows x64 download
   is an archive named `ultralight-free-sdk-1.4.1-dev-win-x64.zip`.
3. Save it into `extern/`, then extract it so the SDK's own top-level folder
   ends up as `extern/ultralight-free-sdk-1.4.1-dev-win-x64/` — i.e. the
   `include/`, `lib/`, `bin/` folders sit directly in that one folder, not
   nested under a second copy of the name:

        7z x -oextern extern\ultralight-free-sdk-1.4.1-dev-win-x64.zip

   (No 7-Zip? Windows 11's own bsdtar reads zip; unlike 7-Zip's `-o`, its
   `-C` needs the folder to exist:
   `mkdir extern\ultralight-free-sdk-1.4.1-dev-win-x64` then
   `C:\Windows\System32\tar.exe -xf extern\ultralight-free-sdk-1.4.1-dev-win-x64.zip -C extern\ultralight-free-sdk-1.4.1-dev-win-x64`.)

4. Delete the archive once extracted — `.gitignore` covers `extern/*.zip` and
   `extern/*.7z`, but keeping it just invites a stale re-extract later.

**The folder name is the SDK's own archive name, on purpose.** An earlier
revision vendored this as plain `extern/ultralight`, which said nothing about
which build it was; `CMakeLists.txt`, `build.ps1` and the harness all name the
explicit folder so the tree cannot silently drift onto a different SDK.

CI cannot do this unattended — there is no token-authenticated download. See
**"The Ultralight SDK pin"** in [CONTRIBUTING.md](../CONTRIBUTING.md) and
RELEASING.md section 2 for how the workflow's cache is seeded by hand.

Layout expected by CMakeLists.txt: `include/`, `lib/`, `bin/`, `resources/`.
The SDK also ships top-level `shaders/`, `samples/`, `tools/` and
`inspector/`, which the build consumes only as noted below.

1.4.1 moved the shader build to CMake and the outputs with it: the D3D11
binaries `gpu/shaders/` mirrors are now under
`shaders/generated/headers/d3d11/*.h` (named `fill_ps.h`, `vertex_quad_vs.h`,
`filter_blur_ps.h`, ...), not the old `shaders/hlsl/bin/*_fxc.h`. The previous
sources survive at `shaders/legacy/` but the set is INCOMPLETE there —
`legacy/hlsl/ps/filter_dropshadow.hlsl` is a zero-byte file and compiles to
nothing, so a legacy-based driver cannot run drop-shadow filters. Use the
generated set. `include/Ultralight/Config.h` is CMake-generated and arrives
with the SDK archive; if it is ever missing, configure the SDK once
(`cmake -B build` in the SDK folder) to emit it.

## appcore-ref/ (committed)

Verbatim upstream AppCore D3D11 driver sources (LGPL-2.1) that
`gpu/MagelightGpuDriver.cpp` was adapted from — kept as provenance for the
LGPL corresponding-source obligation. See `gpu/README.md`.
