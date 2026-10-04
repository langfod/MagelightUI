# check_stage.ps1 — release/stage lint for Magelight (run after build.ps1).
#
# Checks (each guards a failure class that has bitten or would ship silently):
#   1. Namespaced runtime: the stage carries the renamed DLLs and NONE of Ultralight's original names.
#   2. No original module-name strings survive INSIDE any staged DLL (the same-length byte patch
#      must have reached every import; a leftover 'Ultralight.dll' rebinds to another host's copy).
#   3. Required stage files exist (host DLL, driver, resources, focus-menu SWF, licence texts).
#   4. CMake project VERSION parses and matches the built DLL's PLUGIN_VERSION string.
#   4b. MagelightGPU.dll exports every MgGpu_* function the host resolves (the optional ones too: a stage
#      built from a stale driver would silently lose MSAA or images, since the host treats them as absent).
#   5. (optional) api/MagelightUI_API.h is byte-identical to SeverActions' vendored copy.
#
# Usage: powershell -ExecutionPolicy Bypass -File tools/check_stage.ps1 [-Stage C:\b\mgl\stage] [-SaHeader <path>] [-NoPex]
#   -NoPex: skip the Magelight.pex requirement — for CI runners, which have no
#   Papyrus compiler (it ships with the game). A release zip must pass WITHOUT it.
param(
    [string]$Stage = "C:\b\mgl\stage",
    [string]$SaHeader = "",
    [switch]$NoPex
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$fail = 0
function Fail($msg) { Write-Host "FAIL  $msg" -ForegroundColor Red; $script:fail++ }
function Pass($msg) { Write-Host "ok    $msg" }

$plug = Join-Path $Stage "SKSE\Plugins\Magelight"
$hostDll = Join-Path $Stage "SKSE\Plugins\Magelight.dll"

# 1. runtime names
$expected = @("Magelight1.dll", "Magelight1Core.dll", "MgWCore.dll", "MgACore.dll", "MagelightGPU.dll")
$stock = @("Ultralight.dll", "UltralightCore.dll", "WebCore.dll", "AppCore.dll")   # Ultralight's original names
foreach ($n in $expected) { if (Test-Path (Join-Path $plug $n)) { Pass "stage has $n" } else { Fail "stage missing $n" } }
foreach ($n in $stock) { if (Test-Path (Join-Path $plug $n)) { Fail "original Ultralight DLL name in stage: $n (breaks coexistence with other Ultralight-based UI mods)" } }

# 2. original names inside staged binaries
$dlls = @($hostDll) + ($expected | ForEach-Object { Join-Path $plug $_ })
foreach ($d in $dlls) {
    if (-not (Test-Path $d)) { continue }
    $bytes = [System.IO.File]::ReadAllBytes($d)
    $ascii = [System.Text.Encoding]::ASCII.GetString($bytes)
    foreach ($n in $stock) {
        $idx = $ascii.IndexOf($n, [System.StringComparison]::OrdinalIgnoreCase)
        if ($idx -ge 0) {
            # Allow only when embedded in a longer token (e.g. 'MagelightGPU.dll' contains no original name;
            # 'UltralightCore.dll' contains 'Ultralight' but not 'Ultralight.dll').
            Fail ("original module name '{0}' found inside {1} at 0x{2:X}" -f $n, (Split-Path -Leaf $d), $idx)
        }
    }
    if ($ascii.IndexOf("Ultralight.dll", [System.StringComparison]::OrdinalIgnoreCase) -lt 0) { Pass ("no original names in " + (Split-Path -Leaf $d)) }
}

# 3. required files
$required = @($hostDll, (Join-Path $plug "resources\icudt67l.dat"), (Join-Path $plug "resources\cacert.pem"),
              (Join-Path $Stage "Interface\magelightfocus.swf"),
              (Join-Path $plug "license\EULA.txt"), (Join-Path $plug "license\NOTICES.txt"),
              (Join-Path $plug "license\LGPL-2.1.txt"), (Join-Path $plug "license\LICENSE.txt"),
              (Join-Path $plug "license\THIRD_PARTY_LICENSES.txt"), (Join-Path $plug "license\Ultralight-NOTICES.md"),
              (Join-Path $plug "license\Ultralight-LICENSE.txt"))
if (-not $NoPex) { $required += Join-Path $Stage "Scripts\Magelight.pex" }
foreach ($f in $required) {
    if (Test-Path $f) { Pass ("present: " + ($f.Substring($Stage.Length + 1))) } else { Fail "missing: $f" }
}

# 4. version coherence
$cmake = Get-Content (Join-Path $root "CMakeLists.txt") -Raw
$m = [regex]::Match($cmake, 'VERSION\s+(\d+\.\d+\.\d+)')
if (-not $m.Success) { Fail "CMakeLists.txt: no project VERSION" } else {
    $ver = $m.Groups[1].Value
    if (Test-Path $hostDll) {
        $ascii = [System.Text.Encoding]::ASCII.GetString([System.IO.File]::ReadAllBytes($hostDll))
        if ($ascii.Contains("Magelight v" + ($ver -replace '\.', '-')) -or $ascii.Contains($ver)) { Pass "host DLL carries version $ver" }
        else { Fail "host DLL does not contain version string $ver (stale build?)" }
    }
    $gpuDllFile = Join-Path $plug "MagelightGPU.dll"
    if (Test-Path $gpuDllFile) {
        $gpuVer = (Get-Item $gpuDllFile).VersionInfo.FileVersion
        if ($gpuVer -eq $ver -or $gpuVer -eq "$ver.0") { Pass "GPU driver FileVersion $gpuVer" }
        else { Fail "MagelightGPU.dll FileVersion '$gpuVer' != CMake VERSION $ver (rebuild the driver)" }
    }
}

# 4b. GPU driver exports (read from the PE export directory, not a string search)
function Get-PeExportNames([string]$path) {
    $b = [System.IO.File]::ReadAllBytes($path)
    $pe = [BitConverter]::ToInt32($b, 0x3C)
    if ([BitConverter]::ToUInt32($b, $pe) -ne 0x4550) { return $null }
    $nSec = [BitConverter]::ToUInt16($b, $pe + 6)
    $optSize = [BitConverter]::ToUInt16($b, $pe + 20)
    $opt = $pe + 24
    $magic = [BitConverter]::ToUInt16($b, $opt)
    $dirs = if ($magic -eq 0x20B) { $opt + 112 } else { $opt + 96 }
    $expRva = [BitConverter]::ToUInt32($b, $dirs)
    if ($expRva -eq 0) { return @() }
    $secTab = $opt + $optSize
    $toOff = {
        param([uint32]$rva)
        for ($i = 0; $i -lt $nSec; $i++) {
            $s = $secTab + 40 * $i
            $va = [BitConverter]::ToUInt32($b, $s + 12)
            $size = [Math]::Max([BitConverter]::ToUInt32($b, $s + 8), [BitConverter]::ToUInt32($b, $s + 16))
            if ($rva -ge $va -and $rva -lt $va + $size) { return [BitConverter]::ToUInt32($b, $s + 20) + ($rva - $va) }
        }
        return -1
    }
    $exp = & $toOff $expRva
    if ($exp -lt 0) { return $null }
    $count = [BitConverter]::ToUInt32($b, $exp + 24)
    $namesOff = & $toOff ([BitConverter]::ToUInt32($b, $exp + 32))
    $names = @()
    for ($i = 0; $i -lt $count; $i++) {
        $o = & $toOff ([BitConverter]::ToUInt32($b, $namesOff + 4 * $i))
        $e = $o; while ($b[$e] -ne 0) { $e++ }
        $names += [System.Text.Encoding]::ASCII.GetString($b, $o, $e - $o)
    }
    return $names
}
$gpuDll = Join-Path $plug "MagelightGPU.dll"
if (Test-Path $gpuDll) {
    $exports = Get-PeExportNames $gpuDll
    if ($null -eq $exports) { Fail "MagelightGPU.dll: no readable PE export directory" } else {
        # Required ones (the host falls back to the CPU path without them), then the optional ones.
        $wanted = @("MgGpu_Create", "MgGpu_Destroy", "MgGpu_GetGPUDriver", "MgGpu_HasCommandsPending",
                    "MgGpu_DrawCommandList", "MgGpu_GetTextureSRV", "MgGpu_SetSampleCount",
                    "MgGpu_RegisterExternalTexture", "MgGpu_SetExternalTextureSRV", "MgGpu_UnregisterExternalTexture")
        $missing = @($wanted | Where-Object { $exports -notcontains $_ })
        if ($missing.Count -eq 0) { Pass ("GPU driver exports all {0} MgGpu_* functions the host resolves" -f $wanted.Count) }
        else { Fail ("MagelightGPU.dll lacks export(s): " + ($missing -join ", ") + " (stale driver build?)") }
    }
}

# 5. vendored header sync
if ($SaHeader -ne "") {
    $ours = Get-Content (Join-Path $root "api\MagelightUI_API.h") -Raw
    if (Test-Path $SaHeader) {
        $theirs = Get-Content $SaHeader -Raw
        if (($ours -replace "`r`n", "`n") -eq ($theirs -replace "`r`n", "`n")) { Pass "API header in sync with $SaHeader" }
        else { Fail "API header differs from the SA vendored copy: $SaHeader" }
    } else { Fail "SA header not found: $SaHeader" }
}

if ($fail -gt 0) { Write-Host "`n$fail check(s) FAILED" -ForegroundColor Red; exit 1 }
Write-Host "`nAll stage checks passed" -ForegroundColor Green
exit 0
