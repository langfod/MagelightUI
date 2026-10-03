# tools/compile-papyrus.ps1
# Compiles papyrus\Magelight.psc -> <stage>\Scripts\Magelight.pex, plus the
# source next to it under <stage>\Source\Scripts.
#
# Needs the GAME folder, which the repo does not have. Search order:
#   1. -SkyrimDir (the caller may already know it)
#   2. MAGELIGHT_SKYRIM_DIR
#   3. Steam's registry SteamPath + libraryfolders.vdf, first hit wins
# Then require Papyrus Compiler\PapyrusCompiler.exe inside that folder.
#
# A machine with no Skyrim installed gets a WARNING and no pex, not a failure:
# the pex is only needed by mods that call our natives.

param(
    [Parameter(Mandatory = $true)][string]$RepoRoot,
    [Parameter(Mandatory = $true)][string]$StageDir,
    [string]$SkyrimDir = $env:MAGELIGHT_SKYRIM_DIR
)

$ErrorActionPreference = "Stop"

$gameDirs = @()
if ($SkyrimDir) { $gameDirs += $SkyrimDir }

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

$pcExe = $null
foreach ($cand in $gameDirs) {
    if (-not (Test-Path -LiteralPath $cand)) { continue }
    if (Test-Path (Join-Path $cand "Papyrus Compiler\PapyrusCompiler.exe")) {
        $pcSrc = Join-Path $cand "Data\Source\Scripts"
        $pcExe = Join-Path $cand "Papyrus Compiler\PapyrusCompiler.exe"
        break
    }
}

if (-not $pcExe) {
    Write-Warning "Papyrus compiler not found (set MAGELIGHT_SKYRIM_DIR or -SkyrimDir) - Magelight.pex NOT staged"
    exit 0
}

$pexDir = Join-Path $StageDir "Scripts"
$pscDir = Join-Path $StageDir "Source\Scripts"
New-Item -ItemType Directory -Path $pexDir, $pscDir -Force | Out-Null

$psc = Join-Path $RepoRoot "papyrus\Magelight.psc"
& $pcExe $psc "-f=$pcSrc\TESV_Papyrus_Flags.flg" "-i=$RepoRoot\papyrus;$pcSrc" "-o=$pexDir" | Out-Null
if ($LASTEXITCODE -ne 0 -or -not (Test-Path (Join-Path $pexDir "Magelight.pex"))) {
    throw "Papyrus compile failed: $psc"
}
Copy-Item $psc -Destination $pscDir -Force
Write-Host "  Papyrus: Magelight.pex compiled"