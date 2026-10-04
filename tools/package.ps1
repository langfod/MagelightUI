# package.ps1 - assemble the release zips from the build stage.
#
#   .\build.ps1 [-Examples]                    # stage first
#   .\tools\package.ps1 -Version 0.30.0 [-Suffix "-dev"] [-Out <folder>]   (default: .\dist)
#   -Stage defaults to build.ps1's: $env:MG_BUILD_DIR\stage when MG_BUILD_DIR is set, else C:\b\mgl\stage.
#
# Packaging is a release gate: tools\check_stage.ps1 runs against the stage first
# (a stage that fails lint does not zip; a release carries Magelight.pex), and a
# MagelightUI-<ver>-pdb.zip symbols archive lands beside the zips (keep it with
# the release, never ship it to players). The full checklist: RELEASING.md.
#
# Every zip is a plain Data-root layout (SKSE\, Scripts\, ... at the top), so a
# mod manager installs it with no installer and no choices:
#   MagelightUI-<ver>.zip           the runtime: what a player needs to run mods that use
#                                   Magelight, and nothing else. The licence folder is part
#                                   of it (Ultralight's EULA and NOTICES are distribution terms).
#   MagelightUI-<ver>-DevTools.zip  the WebKit Web Inspector and the host's dev pages
#                                   (views\probe, views\app, views\*.html), for mod authors.
#   MagelightUI-<ver>-Examples.zip  Magelight\<ModId>\ (the example mods), for mod authors.
# The runtime zip's SKSE\, Interface\ and Scripts\ are also what another mod bundles
# (docs/DISTRIBUTION.md).
#
# Zip entries are written with '/' separators through System.IO.Compression.
# Compress-Archive writes '\', which non-Windows unzippers (Info-ZIP, macOS,
# MO2 on Linux/Proton) treat as a filename character and extract as a flat mess.
param(
    [Parameter(Mandatory = $true)][string]$Version,
    [string]$Suffix = "",
    [string]$Out = "",
    [string]$Stage = $(if ($env:MG_BUILD_DIR) { Join-Path $env:MG_BUILD_DIR 'stage' } else { 'C:\b\mgl\stage' })
)
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
if (-not $Out) { $Out = Join-Path $root "dist" }

$dll = Join-Path $Stage "SKSE\Plugins\Magelight.dll"
if (-not (Test-Path $dll)) { throw "stage not found - run build.ps1 first ($Stage)" }

# PROVENANCE. The stage is a shared build output: it holds whichever branch
# built last. Say what is being zipped, and refuse a version mismatch - the
# FileVersion resource and SKSEPlugin_Version both come from CMake's VERSION.
$dllVer = (Get-Item $dll).VersionInfo.FileVersion
$dllWhen = (Get-Item $dll).LastWriteTime
Write-Host ("staged Magelight.dll ({2}): v{0} built {1:yyyy-MM-dd HH:mm}" -f $dllVer, $dllWhen, $Stage)
if ($dllVer -and ($dllVer -ne $Version)) {
    throw "staged Magelight.dll is $dllVer but -Version says $Version - rebuild from the branch you mean to ship, or pass the version the stage actually holds"
}

# RELEASE GATE. The stage lint is part of packaging, not optional homework: a
# byte-patch regression or a missing file fails HERE, not on a user's disk.
# (No -NoPex: a packaged release must carry Magelight.pex.)
& (Join-Path $root "tools\check_stage.ps1") -Stage $Stage
if ($LASTEXITCODE -ne 0) { throw "stage lint failed - fix the stage, do not package around it" }

$work = Join-Path (Split-Path $Stage -Parent) "package-stage"
if (Test-Path $work) { Remove-Item $work -Recurse -Force }
$runtime = Join-Path $work "runtime"
$dev     = Join-Path $work "devtools"
$ex      = Join-Path $work "examples"
New-Item -ItemType Directory -Path $runtime, $dev, $ex -Force | Out-Null

# Runtime = the stage; the dev tools and the examples are moved out of it.
Copy-Item (Join-Path $Stage "*") -Destination $runtime -Recurse -Force
$plug  = Join-Path $runtime "SKSE\Plugins\Magelight"
$insp  = Join-Path $plug "inspector"
$views = Join-Path $plug "views"
$exSrc = Join-Path $runtime "Magelight"

$devViews = Join-Path $dev "SKSE\Plugins\Magelight\views"
New-Item -ItemType Directory -Path $devViews -Force | Out-Null
if (Test-Path $insp) { Move-Item $insp -Destination (Join-Path $dev "SKSE\Plugins\Magelight\inspector") }
else { Write-Warning "no inspector in the stage - the DevTools zip will have no Web Inspector" }
# The host's test pages (Magelight.json demoViews / imageProbe) are dev
# surfaces, not something every player installs: they go with the tools.
foreach ($p in @((Join-Path $views "probe"), (Join-Path $views "app"))) {
    if (Test-Path $p) { Move-Item $p -Destination $devViews }
}
Get-ChildItem $views -File -Filter "*.html" -ErrorAction SilentlyContinue | Move-Item -Destination $devViews

if (Test-Path $exSrc) { Move-Item $exSrc -Destination (Join-Path $ex "Magelight") }
else { Write-Warning "no examples in the stage (build.ps1 -Examples) - the Examples zip will be empty" }

# What the runtime zip must still hold once the tools are out of it.
foreach ($must in @("SKSE\Plugins\Magelight\license\EULA.txt", "SKSE\Plugins\Magelight\views\keyboard\index.html",
                    "SKSE\Plugins\Magelight\views\gate\index.html", "SKSE\Plugins\Magelight\resources\icudt67l.dat",
                    "SKSE\Plugins\Magelight\MgWCore.dll", "Scripts\Magelight.pex", "Interface\magelightfocus.swf")) {
    if (-not (Test-Path (Join-Path $runtime $must))) { throw "runtime zip is missing $must - the stage is incomplete" }
}

# The same stamp SeverActions writes into its bundled copy (same path, same
# format), so whichever copy wins Magelight.dll in the mod manager also wins
# VERSION.txt and names itself truthfully.
Set-Content -Path (Join-Path $plug "VERSION.txt") -Encoding ASCII -Value (
    "Magelight UI {0} (built {1:yyyy-MM-dd HH:mm}) - standalone" -f $Version, $dllWhen)

# Zip - '/' entry names, deterministic walk. Nothing sits above the Data-root
# folders: a mod manager would install a top-level README into Data.
Add-Type -AssemblyName System.IO.Compression.FileSystem
if (-not (Test-Path $Out)) { New-Item -ItemType Directory -Path $Out -Force | Out-Null }
function Write-Zip([string]$Folder, [string]$ZipPath) {
    if (Test-Path $ZipPath) { Remove-Item $ZipPath -Force }
    $zip = [System.IO.Compression.ZipFile]::Open($ZipPath, 'Create')
    $n = 0
    foreach ($f in (Get-ChildItem $Folder -Recurse -File | Sort-Object FullName)) {
        $rel = $f.FullName.Substring($Folder.Length + 1).Replace('\', '/')
        [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, $f.FullName, $rel, 'Optimal') | Out-Null
        $n++
    }
    $zip.Dispose()
    $mb = [math]::Round((Get-Item $ZipPath).Length / 1MB, 1)
    Write-Host ("{0} ({1} MB, {2} files)" -f $ZipPath, $mb, $n)
    return $n
}
$base = Join-Path $Out ("MagelightUI-" + $Version + $Suffix)
Write-Host "runtime:  " -NoNewline; $null = Write-Zip $runtime ($base + ".zip")
Write-Host "devtools: " -NoNewline; $null = Write-Zip $dev ($base + "-DevTools.zip")
Write-Host "examples: " -NoNewline; $null = Write-Zip $ex ($base + "-Examples.zip")

# Symbols zip beside the others (never inside them: players do not need them).
# The vectored-handler crash lines decode against the shipped build's PDBs.
$buildDir = Split-Path $Stage -Parent
$pdbs = @("Magelight.pdb", "MagelightGPU.pdb") |
    ForEach-Object { Join-Path $buildDir $_ } | Where-Object { Test-Path $_ }
if ($pdbs.Count -gt 0) {
    $symZip = $base + "-pdb.zip"
    if (Test-Path $symZip) { Remove-Item $symZip -Force }
    $sz = [System.IO.Compression.ZipFile]::Open($symZip, 'Create')
    foreach ($p in $pdbs) {
        [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($sz, $p, (Split-Path $p -Leaf), 'Optimal') | Out-Null
    }
    $sz.Dispose()
    Write-Host ("symbols:  " + $symZip + " (" + $pdbs.Count + " PDB(s) - keep with the release; never ship to players)")
} else {
    Write-Warning "no PDBs in $buildDir - a Release rebuild emits them; crash telemetry is undecodable without the matching symbols"
}
