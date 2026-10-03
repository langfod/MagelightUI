# build.ps1 - Magelight UI build script (adapted from SeverActionsNative's).
# Configure+build with Ninja/MSVC into C:\b\mgl, then stage a ready-to-install
# mod layout under C:\b\mgl\stage.
#
# Layout, the namespaced-runtime byte patch, Papyrus and the examples all live in
# cmake/Stage.cmake, driven by the "stage" build target. This script is the
# toolchain: discover VS, vcpkg, Ninja and cmake, then hand off to the preset.

param(
    [switch]$Clean,
    [switch]$Examples   # stage examples\<ModId>\ manifest mods under stage\Magelight\
)

$ErrorActionPreference = "Stop"
$script:BuildStartTime = Get-Date

Write-Host "=== Magelight UI build ===" -ForegroundColor Cyan

# ── Ultralight SDK build of record ───────────────────────────────────────────
# 1.4.1 is a LOGIN-GATED developer build: you sign in at
# https://ultralig.ht/download, pick "1.4.1-dev", and the browser drops a
# ultralight-free-sdk-1.4.1-dev-win-x64.zip. There is NO public CDN for it,
# so CI cannot fetch it unattended — see RELEASING.md section 2 for how the
# workflow is seeded. The folder keeps the archive's own name so what sits on
# disk says which build it is.
$ulDir = "extern\ultralight-free-sdk-1.4.1-dev-win-x64"
$ulBuildOfRecord = "1.4.1-263f6bd"
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

# ── VS environment (vcvars64) ────────────────────────────────────────────────
# vswhere when it registers the install; manual search when it doesn't — a
# BuildTools 18 install can be invisible to vswhere on some machines while
# others resolve through vswhere. Mirrors SAN's build.ps1.
$vsPath = $null
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (Test-Path $vswhere) {
    $vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
}
if (-not $vsPath) {
    Write-Host "vswhere found no VC install - searching known locations..." -ForegroundColor Yellow
    $possiblePaths = @(
        "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools",
        "C:\Program Files\Microsoft Visual Studio\18\Community",
        "C:\Program Files\Microsoft Visual Studio\2024\Community",
        "C:\Program Files\Microsoft Visual Studio\2022\Community",
        "C:\Program Files\Microsoft Visual Studio\2022\Professional",
        "C:\Program Files\Microsoft Visual Studio\2022\Enterprise"
    )
    foreach ($p in $possiblePaths) {
        if (Test-Path (Join-Path $p "VC\Auxiliary\Build\vcvars64.bat")) { $vsPath = $p; break }
    }
}
if (-not $vsPath) { Write-Host "No Visual Studio with C++ tools found" -ForegroundColor Red; exit 1 }
Write-Host "  VS: $vsPath"
$vcvars = Join-Path $vsPath "VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path $vcvars)) { Write-Host "vcvars64.bat not found" -ForegroundColor Red; exit 1 }
$callerVcpkgRoot = $env:VCPKG_ROOT
cmd /c "`"$vcvars`" >nul 2>&1 && set" | ForEach-Object {
    if ($_ -match "^([^=]+)=(.*)$") { Set-Item -Path "env:$($Matches[1])" -Value $Matches[2] }
}

# Standalone vcpkg: the caller's VCPKG_ROOT, else C:\vcpkg. A root inside the VS
# install is the copy VS 18 bundles (what vcvars sets), which cannot find its own
# VS instance, so it is never used. VCPKG_VISUAL_STUDIO_PATH is the companion
# workaround: vcpkg cannot auto-detect VS 18.x either.
$vcpkgRoot = $callerVcpkgRoot
if ($vcpkgRoot -and ($vcpkgRoot.TrimEnd('\') + '\').StartsWith($vsPath.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) {
    Write-Host "  VCPKG_ROOT $vcpkgRoot is the VS-bundled vcpkg - ignored" -ForegroundColor Yellow
    $vcpkgRoot = $null
}
if (-not $vcpkgRoot) { $vcpkgRoot = "C:\vcpkg" }
$env:VCPKG_ROOT = $vcpkgRoot
$env:VCPKG_VISUAL_STUDIO_PATH = $vsPath
$vcpkgToolchain = Join-Path $vcpkgRoot "scripts\buildsystems\vcpkg.cmake"
if (-not (Test-Path $vcpkgToolchain)) {
    Write-Host "vcpkg toolchain not found at $vcpkgToolchain - install standalone vcpkg (CONTRIBUTING.md step 2) and set VCPKG_ROOT to it, or clone it to C:\vcpkg" -ForegroundColor Red
    exit 1
}
Write-Host "  vcpkg: $vcpkgRoot"

# Ninja: VS-bundled first (what SAN's cache uses), then
# PATH, then the winget install, then anything vcpkg has downloaded.
$ninja = Join-Path $vsPath "Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
if (-not (Test-Path $ninja)) {
    $ninja = $null
    $cand = @()
    $cmd = Get-Command ninja -ErrorAction SilentlyContinue
    if ($cmd) { $cand += $cmd.Source }
    $cand += "$env:LOCALAPPDATA\Microsoft\WinGet\Packages\Ninja-build.Ninja_Microsoft.Winget.Source_8wekyb3d8bbwe\ninja.exe"
    $vcpkgTools = Join-Path $vcpkgRoot "downloads\tools"
    if (Test-Path $vcpkgTools) {
        Get-ChildItem $vcpkgTools -Filter "ninja.exe" -Recurse -ErrorAction SilentlyContinue |
            Select-Object -First 1 | ForEach-Object { $cand += $_.FullName }
    }
    foreach ($p in $cand) { if ($p -and (Test-Path $p)) { $ninja = $p; break } }
}
if (-not $ninja) { Write-Host "ninja.exe not found (winget install Ninja-build.Ninja)" -ForegroundColor Red; exit 1 }
Write-Host "  Ninja: $ninja"

# CMake: PATH (vcvars provides it when the VS CMake component exists), else
# the standalone install.
$cmake = $null
$cmakeCmd = Get-Command cmake -ErrorAction SilentlyContinue
if ($cmakeCmd) { $cmake = $cmakeCmd.Source }
if (-not $cmake) {
    foreach ($p in @("C:\Program Files\CMake\bin\cmake.exe", "C:\Program Files (x86)\CMake\bin\cmake.exe")) {
        if (Test-Path $p) { $cmake = $p; break }
    }
}
if (-not $cmake) { Write-Host "cmake.exe not found" -ForegroundColor Red; exit 1 }
Write-Host "  CMake: $cmake"

$buildDir = "C:\b\mgl"
if ($Clean -and (Test-Path $buildDir)) { Remove-Item -Recurse -Force $buildDir }
if (-not (Test-Path $buildDir)) { New-Item -ItemType Directory -Path $buildDir -Force | Out-Null }

# ── Configure + build + stage ────────────────────────────────────────────────
# Configure settings live in CMakePresets.json; this script only supplies the
# machine-specific paths CMake cannot discover for itself (Ninja). Do not pass
# -DVCPKG_TARGET_TRIPLET or -DVCPKG_INSTALLED_DIR here: an explicit -D overrides
# the preset, which silently defeats the release triplet and the CI cache path.
#
# The "stage" target builds both DLLs and then runs cmake/Stage.cmake, which owns
# the layout, the namespaced-runtime byte patch, Papyrus and the examples.
# NOT $examples: PowerShell variable names are case-insensitive, so that would
# collide with the [switch]$Examples parameter above and fail the type coercion.
$stageExamples = "OFF"
if ($Examples) { $stageExamples = "ON" }
$cmakeArgs = @("--preset", "release", "-S", $PSScriptRoot,
               "-DCMAKE_MAKE_PROGRAM=$ninja", "-DMAGELIGHT_EXAMPLES=$stageExamples")
& $cmake @cmakeArgs
if ($LASTEXITCODE -ne 0) { Write-Host "CMake configure FAILED" -ForegroundColor Red; exit 1 }

# ── Build (with the SAN-style stale-DLL defenses) ────────────────────────────
$mainCpp = Join-Path $PSScriptRoot "src\main.cpp"
if (Test-Path $mainCpp) { (Get-Item $mainCpp).LastWriteTime = Get-Date }

$buildLog = Join-Path $buildDir "build_output.log"
& $cmake --build --preset release --target stage 2>&1 | Tee-Object -FilePath $buildLog
if ($LASTEXITCODE -ne 0) {
    Write-Host "Build/stage FAILED" -ForegroundColor Red
    Select-String -Path $buildLog -Pattern '\berror C\d+|\berror LNK\d+|\bfatal error\b|CMake Error' |
        Select-Object -First 10 | ForEach-Object { Write-Host "  $($_.Line)" -ForegroundColor Red }
    exit 1
}
$hiddenErrors = Select-String -Path $buildLog -Pattern '\berror C\d+|\berror LNK\d+' | Select-Object -First 5
if ($hiddenErrors) {
    Write-Host "Build log contains errors despite exit 0:" -ForegroundColor Red
    $hiddenErrors | ForEach-Object { Write-Host "  $($_.Line)" -ForegroundColor Red }
    exit 1
}

$dll = Join-Path $buildDir "Magelight.dll"
if (-not (Test-Path $dll)) { Write-Host "Magelight.dll missing after build" -ForegroundColor Red; exit 1 }
if ((Get-Item $dll).LastWriteTime -lt $script:BuildStartTime) {
    Write-Host "Magelight.dll is STALE (predates this build) - failing" -ForegroundColor Red
    exit 1
}

Write-Host ""
Write-Host "BUILD OK" -ForegroundColor Green
Write-Host ("  DLL:   " + $dll + "  (" + (Get-Item $dll).LastWriteTime + ")")
Write-Host ("  Stage: " + (Join-Path $buildDir "stage") + "  (install as a mod / copy into Data)")
