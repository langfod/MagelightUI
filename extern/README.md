# extern/

## ul-sdk-2.0.0-beta.2-win-x64-free/ (not committed)

The Ultralight **2.0.0-beta.2** SDK, build of record `2.0.0-beta.2` (see
`VERSION.txt` inside the SDK; `build.ps1` refuses any other build, so moving
to a new one is a deliberate edit of that pin).

**Two ways to get the 2.0.0-beta.2 archive:**

1. **The download page** — <https://ultralig.ht/download>: sign in, select the
   **2.0.0-beta.2** release; the free-edition Windows x64 package is an archive
   named `ul-sdk-2.0.0-beta.2-win-x64-free` (`.zip`).
2. **The direct download API** — no sign-in, scriptable (CI uses it):

       curl -LOJ "https://ultralig.ht/api/v1/sdk/download?platform=windows-x64&version=2.0.0-beta.2"

   The API serves the archive as `ul-sdk-2.0.0-beta.2-win-x64-free.7z` (the page's
   download is a `.zip` of the same folder).

   `platform` is one of `linux-x64`, `macos-arm64`, `windows-x64`; `version`
   takes a build such as `2.0.0-beta.2` or `latest`. Append `&format=json` to
   get the package's url, filename and hash as JSON instead of downloading,
   and `GET https://ultralig.ht/api/v1/sdk/releases` lists every free package
   with its checksum.

Whichever way you got it:

3. Save it into `extern/`, then extract it so the SDK's own top-level folder
   ends up as `extern/ul-sdk-2.0.0-beta.2-win-x64-free/` — i.e. the
   `include/`, `lib/`, `bin/` folders sit directly in that one folder, not
   nested under a second copy of the name:

        7z x -oextern extern\ul-sdk-2.0.0-beta.2-win-x64-free.zip   # .zip from the page, .7z from the API

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

CI fetches method 2 unattended: on an Actions-cache miss it downloads the
pinned build from the API directly (`.github/workflows/ci.yml`), checks its
`VERSION.txt`, and saves the cache again.

## ultralight-free-sdk-1.4.1-dev-win-x64/ (retained for reference, not committed)

The previous **1.4.1** SDK (build of record `1.4.1-263f6bd`), kept in the tree
for A/B comparison against 2.0 during the bump. It is no longer referenced by
the build: `CMakeLists.txt`, `build.ps1` and the harness all point at the 2.0
folder above. If a machine needs it, fetch it the same way (free-edition Windows x64
archive named `ultralight-free-sdk-1.4.1-dev-win-x64` from <https://ultralig.ht/download>
— browser or download API — extracted to its own top-level folder).

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
