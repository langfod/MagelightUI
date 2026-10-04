# build.ps1 - Magelight UI build script (adapted from SeverActionsNative's).
# Configure+build with Ninja/MSVC into C:\b\mgl (MG_BUILD_DIR overrides it, so two
# checkouts can build side by side), then stage a ready-to-install mod layout
# under <build dir>\stage.

param(
    [switch]$Clean,
    [switch]$Examples   # stage examples\<ModId>\ manifest mods under stage\Magelight\
)

$ErrorActionPreference = "Stop"
$script:BuildStartTime = Get-Date

Write-Host "=== Magelight UI build ===" -ForegroundColor Cyan

# ── Ultralight SDK build of record ───────────────────────────────────────────
# The dev CDN's "latest" tracks master, so a fresh fetch can be a newer engine
# than the one the notices name and the DLL-rename permission covers. Moving
# to another build means editing this pin on purpose (and re-checking both).
$ulBuildOfRecord = "1.4.0b.081c48b"
$ulVersionFile = Join-Path $PSScriptRoot "extern\ultralight\VERSION.txt"
if (-not (Test-Path $ulVersionFile)) {
    Write-Host "extern\ultralight\VERSION.txt missing - fetch the SDK per extern\README.md" -ForegroundColor Red; exit 1
}
# "$(...)", not [string]: an empty file yields no output, which [string] leaves $null.
$ulVersion = "$(Get-Content $ulVersionFile -Raw)".Trim()
if ($ulVersion -ne $ulBuildOfRecord) {
    Write-Host "Ultralight SDK is '$ulVersion', but this tree is pinned to $ulBuildOfRecord - use the build of record (extern\README.md)" -ForegroundColor Red
    exit 1
}
Write-Host "  Ultralight SDK: $ulVersion"

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

$buildDir = if ($env:MG_BUILD_DIR) { $env:MG_BUILD_DIR } else { "C:\b\mgl" }
if ($Clean -and (Test-Path $buildDir)) { Remove-Item -Recurse -Force $buildDir }
if (-not (Test-Path $buildDir)) { New-Item -ItemType Directory -Path $buildDir -Force | Out-Null }

# ── Configure ────────────────────────────────────────────────────────────────
$cmakeArgs = @(
    "-G", "Ninja",
    "-B", $buildDir,
    "-S", $PSScriptRoot,
    "-DCMAKE_BUILD_TYPE=Release",
    "-DCMAKE_TOOLCHAIN_FILE=$vcpkgToolchain",
    "-DVCPKG_TARGET_TRIPLET=x64-windows-static",
    "-DCMAKE_MAKE_PROGRAM=$ninja",
    "-DCMAKE_C_COMPILER=cl.exe",
    "-DCMAKE_CXX_COMPILER=cl.exe"
)
& $cmake $cmakeArgs
if ($LASTEXITCODE -ne 0) { Write-Host "CMake configure FAILED" -ForegroundColor Red; exit 1 }

# ── Build (with the SAN-style stale-DLL defenses) ────────────────────────────
$mainCpp = Join-Path $PSScriptRoot "src\main.cpp"
if (Test-Path $mainCpp) { (Get-Item $mainCpp).LastWriteTime = Get-Date }

$buildLog = Join-Path $buildDir "build_output.log"
& $cmake --build $buildDir --config Release 2>&1 | Tee-Object -FilePath $buildLog
if ($LASTEXITCODE -ne 0) {
    Write-Host "Build FAILED" -ForegroundColor Red
    Select-String -Path $buildLog -Pattern '\berror C\d+|\berror LNK\d+|\bfatal error\b' |
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

# ── Stage a ready-to-install mod layout ──────────────────────────────────────
# stage\SKSE\Plugins\Magelight.dll
# stage\SKSE\Plugins\Magelight\{NAMESPACED Ultralight DLLs, resources\}
#
# COEXISTENCE (field-found 2026-08-31): another Ultralight-based UI host ships
# the same four Ultralight module names, and the Windows loader binds imports
# by BASE NAME - whichever host loads "Ultralight.dll" first poisons the other
# with the wrong build (Magelight loaded first alphabetically, and broke it). So our
# copies are RENAMED and every occurrence of the old names inside each binary -
# import descriptors, delay-load descriptors, any literal LoadLibrary strings,
# ASCII and UTF-16 - is patched to the new name. All replacements are
# SAME-LENGTH, so the PE layout is untouched and no code is changed. Longest name first so
# "UltralightCore.dll" is never half-eaten by the "Ultralight.dll" pass.
# Renaming Ultralight 1.4.0b's DLLs and rewriting their module-name strings is done
# with the written permission of Ultralight, Inc. That permission is the author's:
# other mods bundle Magelight's files as shipped and never rename or patch Ultralight's DLLs.
$stage = Join-Path $buildDir "stage\SKSE\Plugins"
$rtDir = Join-Path $stage "Magelight"
New-Item -ItemType Directory -Path $rtDir -Force | Out-Null

Add-Type -TypeDefinition @"
using System;
using System.Collections.Generic;
public static class MgPatch {
    public static int ReplaceAll(byte[] hay, byte[] from, byte[] to) {
        if (from.Length != to.Length) throw new ArgumentException("length mismatch");
        int count = 0;
        for (int i = 0; i + from.Length <= hay.Length; i++) {
            bool hit = true;
            for (int j = 0; j < from.Length; j++) { if (hay[i + j] != from[j]) { hit = false; break; } }
            if (hit) { Array.Copy(to, 0, hay, i, to.Length); count++; i += from.Length - 1; }
        }
        return count;
    }
}
"@

$renames = [ordered]@{
    "UltralightCore.dll" = "Magelight1Core.dll"
    "Ultralight.dll"     = "Magelight1.dll"
    "WebCore.dll"        = "MgWCore.dll"
    "AppCore.dll"        = "MgACore.dll"
}
function Patch-ModuleNames([string]$path) {
    $bytes = [System.IO.File]::ReadAllBytes($path)
    $total = 0
    foreach ($k in $renames.Keys) {
        $v = $renames[$k]
        $total += [MgPatch]::ReplaceAll($bytes, [System.Text.Encoding]::ASCII.GetBytes($k),   [System.Text.Encoding]::ASCII.GetBytes($v))
        $total += [MgPatch]::ReplaceAll($bytes, [System.Text.Encoding]::Unicode.GetBytes($k), [System.Text.Encoding]::Unicode.GetBytes($v))
    }
    [System.IO.File]::WriteAllBytes($path, $bytes)
    Write-Host ("  patched " + (Split-Path $path -Leaf) + " (" + $total + " name refs)")
}

Copy-Item $dll -Destination $stage -Force
Patch-ModuleNames (Join-Path $stage "Magelight.dll")

# The GPU backend rides in the runtime dir (NOT the SKSE plugins root, so SKSE
# never tries to load it as a plugin); its Ultralight import names get the
# same namespacing patch.
$gpuDll = Join-Path $buildDir "MagelightGPU.dll"
if (Test-Path $gpuDll) {
    Copy-Item $gpuDll -Destination $rtDir -Force
    Patch-ModuleNames (Join-Path $rtDir "MagelightGPU.dll")
} else {
    Write-Host "  (MagelightGPU.dll not built - CPU fallback only)" -ForegroundColor Yellow
}

$sdkBin = Join-Path $PSScriptRoot "extern\ultralight\bin"
foreach ($k in $renames.Keys) {
    $dst = Join-Path $rtDir $renames[$k]
    Copy-Item (Join-Path $sdkBin $k) -Destination $dst -Force
    Patch-ModuleNames $dst
}
# Resources: recreate the dir each run — Copy-Item -Recurse onto an EXISTING
# destination nests the source dir inside it (the resources\resources bug).
$resSrc = Join-Path $PSScriptRoot "extern\ultralight\resources"
$resDst = Join-Path $rtDir "resources"
Remove-Item $resDst -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Path $resDst -Force | Out-Null
Copy-Item (Join-Path $resSrc "*") -Destination $resDst -Recurse -Force
if (-not (Test-Path (Join-Path $resDst "icudt67l.dat"))) {
    Write-Host "resources staging failed (icudt67l.dat missing)" -ForegroundColor Red
    exit 1
}
# Views (milestone 4): the on-disk HTML frontend, loaded via file:/// against
# the runtime dir. Same recreate-first rule as resources.
$viewSrc = Join-Path $PSScriptRoot "views"
$viewDst = Join-Path $rtDir "views"
Remove-Item $viewDst -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Path $viewDst -Force | Out-Null
# Players get the host's own pages only: gate (the too-old notice) and
# keyboard (the VR laser keyboard). The dev pages - probe (imageProbe:true),
# app + badge (demoViews:true), playground (unreferenced) - stage with -Examples.
foreach ($v in @("gate", "keyboard")) { Copy-Item (Join-Path $viewSrc $v) -Destination (Join-Path $viewDst $v) -Recurse -Force }
if ($Examples) {
    foreach ($v in @("probe", "app")) { Copy-Item (Join-Path $viewSrc $v) -Destination (Join-Path $viewDst $v) -Recurse -Force }
    Copy-Item (Join-Path $viewSrc "*.html") -Destination $viewDst -Force
}
# Loose assets land in the runtime dir root.
$assetSrc = Join-Path $PSScriptRoot "assets"
if (Test-Path $assetSrc) {
    Copy-Item (Join-Path $assetSrc "*") -Destination $rtDir -Recurse -Force
}
# The engine menu's blank Scaleform movie -> Data\Interface.
$ifaceSrc = Join-Path $PSScriptRoot "interface"
if (Test-Path $ifaceSrc) {
    $ifaceDst = Join-Path $buildDir "stage\Interface"
    New-Item -ItemType Directory -Path $ifaceDst -Force | Out-Null
    Copy-Item (Join-Path $ifaceSrc "*") -Destination $ifaceDst -Recurse -Force
}

Write-Host ""
Write-Host "BUILD OK" -ForegroundColor Green
Write-Host ("  DLL:   " + $dll + "  (" + (Get-Item $dll).LastWriteTime + ")")
# Papyrus tier: compile papyrus\Magelight.psc -> stage\Scripts\Magelight.pex (+ source).
# The game folder comes from MAGELIGHT_SKYRIM_DIR, else from Steam's own list of
# library folders (registry + libraryfolders.vdf) - first hit wins.
$pcExe = $null
$gameDirs = @()
if ($env:MAGELIGHT_SKYRIM_DIR) { $gameDirs += $env:MAGELIGHT_SKYRIM_DIR }
$steamRoots = @("C:\Program Files (x86)\Steam")
try {
    $steamPath = (Get-ItemProperty -Path "HKCU:\Software\Valve\Steam" -Name SteamPath -ErrorAction Stop).SteamPath
    if ($steamPath) { $steamRoots = @($steamPath.Replace('/', '\')) + $steamRoots }
} catch { }
foreach ($steamRoot in $steamRoots) {
    # Join-Path THROWS (under $ErrorActionPreference = Stop) when the drive itself
    # does not exist, so skip absent roots outright.
    if (-not (Test-Path -LiteralPath $steamRoot)) { continue }
    $libs = @($steamRoot)
    $vdf = Join-Path $steamRoot "steamapps\libraryfolders.vdf"
    if (Test-Path -LiteralPath $vdf) {
        foreach ($m in (Select-String -LiteralPath $vdf -Pattern '"path"\s+"([^"]+)"' -AllMatches).Matches) {
            $libs += $m.Groups[1].Value.Replace('\\', '\')
        }
    }
    foreach ($lib in $libs) {
        if (Test-Path -LiteralPath $lib) { $gameDirs += (Join-Path $lib "steamapps\common\Skyrim Special Edition") }
    }
}
foreach ($cand in $gameDirs) {
    if (-not (Test-Path -LiteralPath $cand)) { continue }
    if (Test-Path (Join-Path $cand "Papyrus Compiler\PapyrusCompiler.exe")) {
        $pcDir = Join-Path $cand "Papyrus Compiler"
        $pcSrc = Join-Path $cand "Data\Source\Scripts"
        $pcExe = Join-Path $pcDir "PapyrusCompiler.exe"
        break
    }
}
if (-not $pcExe) { $pcExe = "C:\__no_papyrus_compiler__" }
if (Test-Path $pcExe) {
    $pexDir = Join-Path $buildDir "stage\Scripts"
    $pscDir = Join-Path $buildDir "stage\Source\Scripts"
    New-Item -ItemType Directory -Path $pexDir, $pscDir -Force | Out-Null
    $psc = Join-Path $PSScriptRoot "papyrus\Magelight.psc"
    & $pcExe $psc "-f=$pcSrc\TESV_Papyrus_Flags.flg" "-i=$PSScriptRoot\papyrus;$pcSrc" "-o=$pexDir" | Out-Null
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path (Join-Path $pexDir "Magelight.pex"))) { throw "Papyrus compile failed: $psc" }
    Copy-Item $psc -Destination $pscDir -Force
    Write-Host "  Papyrus: Magelight.pex compiled"
} else {
    Write-Warning "Papyrus compiler not found - Magelight.pex NOT staged"
}
# License texts -> runtime dir\license (NOTICES.txt names each file; the Ultralight EULA
# requires its own text to travel with the runtime).
$licDst = Join-Path $buildDir "stage\SKSE\Plugins\Magelight\license"
New-Item -ItemType Directory -Path $licDst -Force | Out-Null
Copy-Item (Join-Path $PSScriptRoot "LICENSE") -Destination (Join-Path $licDst "LICENSE.txt") -Force
Copy-Item (Join-Path $PSScriptRoot "NOTICES.txt") -Destination $licDst -Force
Copy-Item (Join-Path $PSScriptRoot "THIRD_PARTY_LICENSES.txt") -Destination $licDst -Force
Copy-Item (Join-Path $PSScriptRoot "gpu\LICENSE") -Destination (Join-Path $licDst "LGPL-2.1.txt") -Force
$ulLic = Join-Path $PSScriptRoot "extern\ultralight\license"
if (Test-Path $ulLic) {
    Copy-Item (Join-Path $ulLic "EULA.txt") -Destination $licDst -Force
    Copy-Item (Join-Path $ulLic "LICENSE.txt") -Destination (Join-Path $licDst "Ultralight-LICENSE.txt") -Force
    Copy-Item (Join-Path $ulLic "NOTICES.md") -Destination (Join-Path $licDst "Ultralight-NOTICES.md") -Force
} else { Write-Warning "extern\ultralight\license missing - Ultralight EULA NOT staged" }
# Web Inspector assets (SDK inspector\) -> runtime dir; used only with Magelight.json devMode.
$inspSrc = Join-Path $PSScriptRoot "extern\ultralight\inspector"
if (Test-Path $inspSrc) {
    $inspDst = Join-Path $buildDir "stage\SKSE\Plugins\Magelight\inspector"
    if (Test-Path $inspDst) { Remove-Item $inspDst -Recurse -Force }
    Copy-Item $inspSrc -Destination $inspDst -Recurse -Force
    Write-Host "  Inspector assets staged"
}
# Example manifest mods (examples\<ModId>\) -> stage\Magelight\<ModId>\ - opt-in:
# A build without -Examples must not carry what an earlier -Examples run staged.
if (-not $Examples) {
    Remove-Item (Join-Path $buildDir "stage\Magelight") -Recurse -Force -ErrorAction SilentlyContinue
}
# they are dev/demo surfaces, not something every user should see.
if ($Examples) {
    $exSrc = Join-Path $PSScriptRoot "examples"
    # Examples with a package.json are Vite projects: build them first (their
    # dist lands inside the example folder, under views/).
    if (Get-Command npm -ErrorAction SilentlyContinue) {
        Get-ChildItem $exSrc -Directory | Where-Object { Test-Path (Join-Path $_.FullName "package.json") } | ForEach-Object {
            Push-Location $_.FullName
            try {
                # Native stderr under ErrorActionPreference=Stop is a TERMINATING
                # error: one failing vite build used to abort the whole stage
                # (examples silently missing, 2026-09-03). Catch it, warn, go on.
                $prevEap = $ErrorActionPreference; $ErrorActionPreference = "Continue"
                if (-not (Test-Path "node_modules")) { & npm install --silent 2>&1 | Out-Null }
                & npm run build --silent 2>&1 | Out-Null
                $rc = $LASTEXITCODE
                $ErrorActionPreference = $prevEap
                if ($rc -ne 0) { Write-Warning ("example {0}: vite build failed (workspace packages built? run npm run build at the repo root)" -f $_.Name) }
                else { Write-Host ("  example {0}: built" -f $_.Name) }
            } catch { Write-Warning ("example {0}: build error - {1}" -f $_.Name, $_.Exception.Message) }
            finally { Pop-Location }
        }
    }
    if (Test-Path $exSrc) {
        $exDst = Join-Path $buildDir "stage\Magelight"
        New-Item -ItemType Directory -Path $exDst -Force | Out-Null
        Copy-Item (Join-Path $exSrc "*") -Destination $exDst -Recurse -Force
        Get-ChildItem $exDst -Recurse -Directory -Force | Where-Object { $_.Name -in @("node_modules", "src", "scripts", ".vite") } |
            Remove-Item -Recurse -Force -ErrorAction SilentlyContinue
        Get-ChildItem $exDst -Recurse -File -Force | Where-Object { $_.Name -in @("package.json", "package-lock.json", "vite.config.ts", "tsconfig.json", "index.html") -and $_.Directory.Parent.FullName -eq $exDst } |
            Remove-Item -Force -ErrorAction SilentlyContinue
        Write-Host "  Examples staged -> $exDst"
    }
}
# Folder READMEs are for the repo, not the player's Data folder.
Get-ChildItem (Join-Path $buildDir "stage") -Recurse -File -Filter "README.md" |
    Where-Object { $_.Directory.Name -ne "license" } | Remove-Item -Force
Write-Host ("  Stage: " + (Join-Path $buildDir "stage") + "  (install as a mod / copy into Data)")
