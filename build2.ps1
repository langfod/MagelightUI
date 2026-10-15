# build2.ps1 - Magelight UI build script, simplified for a single machine.
#
# Same flow as build.ps1: configure + build with Ninja/MSVC into build\, then
# stage a ready-to-install mod layout under build\stage.
#
# The difference is the toolchain block, which is hardcoded here instead of
# discovered. That is deliberate - this script exists so the project builds on a
# machine whose only vcpkg is the copy VS bundles, which build.ps1 rejects on
# purpose. See local_projects/stage-2-build-scripts.md.
#
# Layout, staging and the namespaced-runtime patch all live in cmake/Stage.cmake.

param(
    [switch]$Clean,
    [switch]$Examples   # stage examples\<ModId>\ manifest mods under stage\Magelight\
)

$ErrorActionPreference = "Stop"

Write-Host "=== Magelight UI build ===" -ForegroundColor Cyan

# ── Ultralight SDK build of record ───────────────────────────────────────────
# 2.0 is fetched from https://ultralig.ht/download (the beta is a signed-in
# download; there is NO public CDN for it), so CI cannot fetch it unattended —
# see RELEASING.md section 2 for how the workflow is seeded. The folder keeps
# the archive's own name so what sits on disk says which build it is.
$ulDir = "extern\ul-sdk-2.0.0-beta.2-win-x64-free"
$ulBuildOfRecord = "2.0.0-beta.2"
$ulVersionFile = Join-Path $PSScriptRoot "$ulDir\VERSION.txt"
if (-not (Test-Path $ulVersionFile)) {
    Write-Host "$ulDir\VERSION.txt missing - fetch the SDK per extern\README.md" -ForegroundColor Red; exit 1
}
# "$(...)", not [string]: an empty file yields no output, which [string] leaves $null.
$ulVersion = "$(Get-Content $ulVersionFile -Raw)".Trim()
if ($ulVersion -ne $ulBuildOfRecord) {
    Write-Host "Ultralight SDK is '$ulVersion', but this tree is pinned to $ulBuildOfRecord - use the build of record (extern\README.md)" -ForegroundColor Red
    exit 1
}
Write-Host "  Ultralight SDK: $ulVersion ($ulDir)"

$vsPath = "C:\Program Files\Microsoft Visual Studio\18\Professional"
$ninja = Join-Path $vsPath "Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
Write-Host "  Ninja: $ninja"

$buildDir = "build"
if ($Clean -and (Test-Path $buildDir)) { Remove-Item -Recurse -Force $buildDir }
if (-not (Test-Path $buildDir)) { New-Item -ItemType Directory -Path $buildDir -Force | Out-Null }

# ── Configure + build + stage ────────────────────────────────────────────────
# Uses the "local" preset (in-tree build\), not "release" (C:\b\mgl). Configure
# settings live in CMakePresets.json; this script only supplies Ninja. Do not pass
# -DVCPKG_TARGET_TRIPLET or -DCMAKE_TOOLCHAIN_FILE here: an explicit -D overrides
# the preset. The VS-bundled toolchain comes from the preset's $env{VCPKG_ROOT},
# which vcvars64 (run by the interactive shell this script expects) sets.
# NOT $examples: PowerShell variable names are case-insensitive, so that would
# collide with the [switch]$Examples parameter above and fail the type coercion.
$stageExamples = "OFF"
if ($Examples) { $stageExamples = "ON" }
& cmake --preset local -S $PSScriptRoot "-DCMAKE_MAKE_PROGRAM=$ninja" "-DMAGELIGHT_EXAMPLES=$stageExamples"
if ($LASTEXITCODE -ne 0) { Write-Host "CMake configure FAILED" -ForegroundColor Red; exit 1 }

& cmake --build --preset local --target stage
if ($LASTEXITCODE -ne 0) {
    Write-Host "Build/stage FAILED" -ForegroundColor Red
    exit 1
}

$dll = Join-Path $buildDir "Magelight.dll"
Write-Host ""
Write-Host "BUILD OK" -ForegroundColor Green
Write-Host ("  DLL:   " + $dll + "  (" + (Get-Item $dll).LastWriteTime + ")")
Write-Host ("  Stage: " + (Join-Path $buildDir "stage") + "  (install as a mod / copy into Data)")