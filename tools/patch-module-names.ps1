# tools/patch-module-names.ps1
# Byte-replaces module-name strings inside already-copied binaries.
#
# Dumb on purpose: the rename map lives in cmake/stage/Runtime.cmake (one owner)
# and arrives as a file. This script knows nothing about which names are ours or
# why - it only swaps equal-length byte sequences.
#
# Why a script and not CMake: CMake has no binary read-modify-write. A hex-blob
# rewrite in CMake would be unreadable and unmaintainable.
#
# Replacements MUST be the same length or the PE layout shifts - that invariant is
# enforced here (throw), not assumed.
#
# Usage (normally invoked by cmake/stage/Runtime.cmake):
#   patch-module-names.ps1 -MapFile <path> -FilesCsv <path>
#
# Both inputs are FILES rather than inline lists, because PowerShell's script
# parameter binder will not absorb a bare list into a [string[]]: "-Map a b" binds
# one element and rejects the second as a stray positional. A '=' inside an
# argument is separately read as a parameter assignment, so "a.dll=b.dll" does not
# survive as one argument either. Files sidestep all of it.
#
# Map file: one 'from to' pair per line, separated by a SPACE. Not '|' or '=':
# '=' is read as a parameter assignment when passed inline, and CMake eats both
# '|' and ';' as list separators when writing the file. A space is inert to both.
# Files file: one path per line; blank lines ignored.

param(
    [Parameter(Mandatory = $true)]
    [string]$MapFile,

    [Parameter(Mandatory = $true)]
    [string]$FilesCsv
)

$ErrorActionPreference = "Stop"

$files = @()
foreach ($line in [System.IO.File]::ReadAllLines($FilesCsv)) {
    $t = $line.Trim()
    if ($t) { $files += $t }
}
if ($files.Count -eq 0) { throw "no files to patch (from $FilesCsv)" }

$pairs = [ordered]@{}
foreach ($line in [System.IO.File]::ReadAllLines($MapFile)) {
    $line = $line.Trim()
    if (-not $line -or $line.StartsWith('#')) { continue }
    $parts = $line.Split(' ', 2)
    if ($parts.Count -ne 2) { throw "bad map line '$line' (expected 'from to')" }
    $pairs[$parts[0]] = $parts[1]
}
if ($pairs.Count -eq 0) { throw "rename map $MapFile yielded no pairs" }

# Longest name first, so "UltralightCore.dll" is never half-eaten by the
# "Ultralight.dll" pass. Correctness depends on the ORDER of this list.
$ordered = $pairs.Keys | Sort-Object { -$_.Length }

Add-Type -TypeDefinition @"
using System;
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

foreach ($f in $files) {
    if (-not (Test-Path -LiteralPath $f)) { throw "patch target missing: $f" }
    $bytes = [System.IO.File]::ReadAllBytes($f)
    $total = 0
    foreach ($from in $ordered) {
        $to = $pairs[$from]
        # ASCII and UTF-16LE: import and delay-load descriptors are ASCII, but a
        # literal LoadLibraryW argument is UTF-16. Both must be patched.
        $total += [MgPatch]::ReplaceAll($bytes, [System.Text.Encoding]::ASCII.GetBytes($from),   [System.Text.Encoding]::ASCII.GetBytes($to))
        $total += [MgPatch]::ReplaceAll($bytes, [System.Text.Encoding]::Unicode.GetBytes($from), [System.Text.Encoding]::Unicode.GetBytes($to))
    }
    [System.IO.File]::WriteAllBytes($f, $bytes)
    Write-Host ("  patched " + (Split-Path $f -Leaf) + " (" + $total + " name refs)")
}