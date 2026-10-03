# tools/stage-examples.ps1
# Stages examples\<ModId>\ -> <stage>\Magelight\<ModId>\, opt-in.
#
# An example with a package.json is a Vite project, so it is built first; its
# output lands inside the example folder under views/. npm may be absent, and a
# failing vite build must not abort the whole stage (one broken example used to
# take every other example with it) - so failures warn and continue.
#
# Then the tree is copied and pruned: node_modules, build inputs and the manifest
# config files are repo-side, not player-side.

param(
    [Parameter(Mandatory = $true)][string]$RepoRoot,
    [Parameter(Mandatory = $true)][string]$StageDir
)

$ErrorActionPreference = "Stop"

$exSrc = Join-Path $RepoRoot "examples"
if (-not (Test-Path $exSrc)) { exit 0 }

if (Get-Command npm -ErrorAction SilentlyContinue) {
    Get-ChildItem $exSrc -Directory | Where-Object { Test-Path (Join-Path $_.FullName "package.json") } | ForEach-Object {
        Push-Location $_.FullName
        try {
            # Native stderr under ErrorActionPreference=Stop is a TERMINATING error:
            # one failing vite build used to abort the whole stage. Catch, warn, go on.
            $prevEap = $ErrorActionPreference; $ErrorActionPreference = "Continue"
            if (-not (Test-Path "node_modules")) { & npm install --silent 2>&1 | Out-Null }
            & npm run build --silent 2>&1 | Out-Null
            $rc = $LASTEXITCODE
            $ErrorActionPreference = $prevEap
            if ($rc -ne 0) {
                Write-Warning ("example {0}: vite build failed (workspace packages built? run npm run build at the repo root)" -f $_.Name)
            } else {
                Write-Host ("  example {0}: built" -f $_.Name)
            }
        } catch { Write-Warning ("example {0}: build error - {1}" -f $_.Name, $_.Exception.Message) }
        finally { Pop-Location }
    }
}

$exDst = Join-Path $StageDir "Magelight"
if (Test-Path $exDst) { Remove-Item $exDst -Recurse -Force }
New-Item -ItemType Directory -Path $exDst -Force | Out-Null
Copy-Item (Join-Path $exSrc "*") -Destination $exDst -Recurse -Force

Get-ChildItem $exDst -Recurse -Directory -Force |
    Where-Object { $_.Name -in @("node_modules", "src", "scripts", ".vite") } |
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue
# Only the top level of each staged example: a nested index.html is part of the
# example's own views/, not build config.
Get-ChildItem $exDst -Recurse -File -Force |
    Where-Object { $_.Name -in @("package.json", "package-lock.json", "vite.config.ts", "tsconfig.json", "index.html") -and $_.Directory.Parent.FullName -eq $exDst } |
    Remove-Item -Force -ErrorAction SilentlyContinue

Write-Host "  Examples staged -> $exDst"