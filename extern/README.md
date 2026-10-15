# extern/

## ul-sdk-2.0.0-beta.2-win-x64-free/ (not committed)

The Ultralight **2.0.0-beta.2** SDK, build of record `2.0.0-beta.2` (see
`VERSION.txt` inside the SDK; `build.ps1` refuses any other build, so moving
to a new one is a deliberate edit of that pin).

**The 2.0 SDK is a login-gated developer build — there is no public CDN or
`curl` URL for it.** Getting it takes a human at a browser:

1. Sign in at **<https://ultralig.ht/download>**.
2. Select the **2.0.0-beta.2** release. The free-edition Windows x64 download
   is an archive named `ul-sdk-2.0.0-beta.2-win-x64-free.zip`.
3. Save it into `extern/`, then extract it so the SDK's own top-level folder
   ends up as `extern/ul-sdk-2.0.0-beta.2-win-x64-free/` — i.e. the
   `include/`, `lib/`, `bin/` folders sit directly in that one folder, not
   nested under a second copy of the name:

        7z x -oextern extern\ul-sdk-2.0.0-beta.2-win-x64-free.zip

   (No 7-Zip? Windows 11's own bsdtar reads zip; unlike 7-Zip's `-o`, its
   `-C` needs the folder to exist:
   `mkdir extern\ul-sdk-2.0.0-beta.2-win-x64-free` then
   `C:\Windows\System32\tar.exe -xf extern\ul-sdk-2.0.0-beta.2-win-x64-free.zip -C extern\ul-sdk-2.0.0-beta.2-win-x64-free`.)

4. Delete the archive once extracted — `.gitignore` covers `extern/*.zip` and
   `extern/*.7z`, but keeping it just invites a stale re-extract later.

**The folder name is the SDK's own archive name, on purpose.** An earlier
revision vendored this as plain `extern/ultralight`, which said nothing about
which build it was; `CMakeLists.txt`, `build.ps1` and the harness all name the
explicit folder so the tree cannot silently drift onto a different SDK.

CI cannot do this unattended — there is no token-authenticated download. See
**"The Ultralight SDK pin"** in [CONTRIBUTING.md](../CONTRIBUTING.md) and
RELEASING.md section 2 for how the workflow's cache is seeded by hand.

## ultralight-free-sdk-1.4.1-dev-win-x64/ (retained for reference, not committed)

The previous **1.4.1** SDK (build of record `1.4.1-263f6bd`), kept in the tree
for A/B comparison against 2.0 during the bump. It is no longer referenced by
the build: `CMakeLists.txt`, `build.ps1` and the harness all point at the 2.0
folder above. If a machine needs it, fetch it the same way (login-gated dev
build from <https://ultralig.ht/download>, free-edition Windows x64 archive
named `ultralight-free-sdk-1.4.1-dev-win-x64.zip`, extracted to its own
top-level folder).

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
