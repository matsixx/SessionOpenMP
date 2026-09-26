# Put the freshly built SessionTweaks DLL into both installs, WHETHER OR NOT THE GAME IS RUNNING.
#
# Windows will not let you overwrite a DLL a process has mapped, but it will happily let you RENAME
# one: the running game keeps the old file open under its new name while the new file takes its
# place, and the next launch loads the new one. That is what dist/update/update.ps1 does for users
# (Copy-Replacing), and dev deploys use it for the same reason -- "close the game first" is not a
# step anyone should have to take to test a build.
#
#   powershell -ExecutionPolicy Bypass -File tools\deploy_tweaks.ps1
#
param(
    [string]$Dll = "$PSScriptRoot\..\build\tweaks\Release\main.dll"
)
$ErrorActionPreference = "Stop"

$targets = @(
    "F:\Steam\steamapps\common\Session\SessionGame\Binaries\Win64\Mods\SessionTweaks\dlls\main.dll",
    "C:\Program Files\Epic Games\SessionSkateSim\SessionGame\Binaries\Win64\Mods\SessionTweaks\dlls\main.dll"
)

if (-not (Test-Path $Dll)) { throw "no build at $Dll" }
# The version lives in the DLL as plain bytes; read it out of the file rather than trusting a build
# that may not have landed (a deploy that reports a number it did not verify is worse than none).
function Get-DllVersion {
    param([string]$Path)
    $bytes = [IO.File]::ReadAllBytes($Path)
    $text  = [Text.Encoding]::ASCII.GetString($bytes)
    # @(...) matters: one match comes back as a bare string, and indexing a string gives a CHARACTER.
    $found = @([regex]::Matches($text, "3\.19\.\d{3}") | ForEach-Object { $_.Value } | Sort-Object -Unique)
    if ($found.Count -eq 0) { return "?" }
    return $found[$found.Count - 1]
}
$version = Get-DllVersion $Dll

foreach ($to in $targets) {
    $dir = Split-Path $to -Parent
    if (-not (Test-Path $dir)) { Write-Host "skip (no install): $to"; continue }
    $renamed = $false
    try {
        Copy-Item $Dll $to -Force -ErrorAction Stop
    } catch {
        # Locked: rename it aside and drop the new one in behind it.
        $aside = "$to.omp-old-" + [Guid]::NewGuid().ToString("N").Substring(0, 8)
        Move-Item $to $aside -Force -ErrorAction Stop
        Copy-Item $Dll $to -Force -ErrorAction Stop
        try { Remove-Item $aside -Force -ErrorAction Stop } catch { }   # still mapped: swept next time
        $renamed = $true
    }
    # Sweep up anything an earlier deploy left behind once the game let go of it.
    foreach ($f in @(Get-ChildItem $dir -File -Filter "*.omp-old-*" -ErrorAction SilentlyContinue)) {
        try { Remove-Item $f.FullName -Force -ErrorAction Stop } catch { }
    }
    # Read the version back out of the file that is now in place, not out of the one we copied.
    $got = Get-DllVersion $to
    $how = if ($renamed) { " (game running: renamed aside, restart to load it)" } else { "" }
    Write-Host "$got -> $to$how"
    if ($got -ne $version) { throw "deployed $got but built $version" }
}
