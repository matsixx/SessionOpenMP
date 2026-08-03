# SessionOpenMP -- rebuild the tester zip from the last one + freshly built DLLs.
#
# WHY THIS EXISTS: the zip used to be assembled by hand in a scratch directory, which made two
# mistakes easy and silent -- shipping a stale main.dll after a code change, and dropping the
# LICENSE / notice files when the stage was rebuilt from scratch (GPL s7 requires the exception
# notice travel WITH the binary, and ImGui/MinHook are compiled INTO main.dll). This script refreshes
# in place and refuses to write a zip that is missing any of them.
#
# Usage (PowerShell 5.1):
#   .\tools\dist\refresh_dist.ps1
#   .\tools\dist\refresh_dist.ps1 -Zip "F:\SessionOpenMP\dist\SessionOpenMP-Test-2026-07-30.zip"
param(
    [string]$Zip  = "",
    [string]$Root = "F:\SessionOpenMP"
)

$ErrorActionPreference = "Stop"

if ([string]::IsNullOrEmpty($Zip)) {
    # Matches both the old SessionOpenMP-Test-<date> naming and versioned releases like
    # SessionOpenMP-0.5.0b. Deliberately anchored on "SessionOpenMP-" so it cannot pick up the
    # unrelated OpenMP-PingTest.zip that also lives in dist\.
    $Zip = Get-ChildItem (Join-Path $Root "dist\SessionOpenMP-*.zip") |
           Sort-Object LastWriteTime -Descending | Select-Object -First 1 -ExpandProperty FullName
}
if ([string]::IsNullOrEmpty($Zip) -or -not (Test-Path $Zip)) {
    throw "no package zip found (looked for dist\SessionOpenMP-*.zip) -- pass -Zip explicitly"
}

# Every UE4SS C++ mod this repo builds: <target output> -> <mod folder name>. Add a row when a new
# mod target lands and it ships automatically -- that is the whole point of the table.
#
# BOTH MODS ARE REQUIRED. The package is one product: SessionTweaks always ships with SessionOpenMP,
# so a missing DLL is a build that was not run, not an optional extra to leave out. Skipping it
# silently is exactly the stale/incomplete-package failure this script exists to prevent -- so a
# missing required DLL throws instead. (Set Required = $false for a genuinely optional future mod.)
$mods = @(
    @{ Name = "SessionOpenMP"; Required = $true; Dll = Join-Path $Root "build\Release\main.dll" },
    @{ Name = "SessionTweaks"; Required = $true; Dll = Join-Path $Root "build\tweaks\Release\main.dll" }
)
# Required at the zip ROOT. Missing any of these is a licensing defect, not a cosmetic one.
$required = @("LICENSE", "LICENSE-EXCEPTION.txt", "THIRD-PARTY-NOTICES.txt", "EOS-ThirdPartySoftwareNotice.txt")

# Files copied from the repo to the zip ROOT on every refresh. The player-facing README lives in the
# repo rather than only inside the zip, so it is reviewable, diffable, and cannot silently go stale
# while the install steps or the shipped mods change underneath it. It also carries the GPL section 6b
# written offer, which makes it a licensing document, not just instructions.
$rootData = @(
    @{ From = Join-Path $Root "dist\package-README.txt"; To = "README.txt" }
)

# Mods\ is an ALLOWLIST: only the folders in $mods survive, and inside each one only dlls\.
#
# WHY: the zip was seeded from a stock RE-UE4SS drop, so it carried ten of UE4SS's own sample and
# debug Lua mods -- console enabler, cheat-manager enabler, splitscreen, actor dumper, a Lua profiler,
# the BP mod loader. None of them is used by anything we ship (our hotkeys are read with
# GetAsyncKeyState, not UE4SS keybinds), and shipping them to testers is worse than just wasteful:
# a cheat manager and a console are extra variables in somebody's bug report.
# An allowlist rather than a delete-list on purpose -- a future UE4SS drop can add new sample mods,
# and the failure mode of a delete-list is that they silently reappear.
# The dlls-only rule means a mod that legitimately ships DATA needs a line here. The chat typeface is
# loaded at runtime from Mods/SessionOpenMP/fonts/, and without these entries the prune deletes it
# silently, shipping a build whose chat has no font. If you add data to a mod, add it here in the same
# commit. Exact names or wildcards both work.
# What the rule is FOR: it is what removes name_filter.txt, a 125 KB word list left over from an older
# design that nothing reads (mp_name.cpp compiles the list in, precisely so a player cannot delete it).
$modExtras = @("SessionOpenMP\fonts\Almarai-Regular.ttf",
               "SessionOpenMP\fonts\Almarai-Bold.ttf",
               "SessionOpenMP\fonts\OFL.txt")     # the chat typeface -- see THIRD-PARTY-NOTICES.txt
# Data this package ships alongside the DLLs, refreshed from the repo on every build so the zip can
# never drift from source. OFL.txt is not optional decoration: the licence requires it to travel with
# the font, so it is treated exactly like the licence files at the zip root.
$modData = @(
    @{ From = Join-Path $Root "assets\fonts\Almarai-Regular.ttf"; To = "SessionOpenMP\fonts\Almarai-Regular.ttf" },
    @{ From = Join-Path $Root "assets\fonts\Almarai-Bold.ttf";    To = "SessionOpenMP\fonts\Almarai-Bold.ttf" },
    @{ From = Join-Path $Root "assets\fonts\OFL.txt";             To = "SessionOpenMP\fonts\OFL.txt" }
)

$stage = Join-Path $env:TEMP ("omp_dist_" + [System.Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $stage | Out-Null
try {
    Write-Host "expanding $Zip"
    Expand-Archive -LiteralPath $Zip -DestinationPath $stage -Force

    foreach ($m in $mods) {
        if (-not (Test-Path $m.Dll)) {
            if ($m.Required) {
                throw ("REFUSING to write the zip -- {0} is not built ({1}). Run: cmake --build {2}\build --config Release" -f $m.Name, $m.Dll, $Root)
            }
            Write-Host ("  SKIP {0}: not built ({1})" -f $m.Name, $m.Dll) -ForegroundColor Yellow
            continue
        }
        $dest = Join-Path $stage ("Mods\" + $m.Name + "\dlls")
        if (-not (Test-Path $dest)) { New-Item -ItemType Directory -Path $dest -Force | Out-Null }
        Copy-Item $m.Dll (Join-Path $dest "main.dll") -Force
        $stamp = (Get-Item $m.Dll).LastWriteTime
        Write-Host ("  {0}: main.dll updated ({1})" -f $m.Name, $stamp)
    }

    foreach ($d in $rootData) {
        if (-not (Test-Path $d.From)) { throw ("REFUSING to write the zip -- missing repo file: " + $d.From) }
        Copy-Item $d.From (Join-Path $stage $d.To) -Force
        Write-Host ("  root: {0}" -f $d.To)
    }

    # ---- refresh shipped data from the repo BEFORE the prune, so the allowlist judges what we just
    # wrote rather than whatever the previous zip happened to contain.
    foreach ($d in $modData) {
        if (-not (Test-Path $d.From)) { throw ("REFUSING to write the zip -- missing repo asset: " + $d.From) }
        $dest = Join-Path $stage ("Mods\" + $d.To)
        $destDir = Split-Path $dest
        if (-not (Test-Path $destDir)) { New-Item -ItemType Directory -Path $destDir -Force | Out-Null }
        Copy-Item $d.From $dest -Force
        Write-Host ("  data: Mods\{0}" -f $d.To)
    }

    # ---- prune Mods\ to the allowlist.
    $modsDir = Join-Path $stage "Mods"
    if (-not (Test-Path $modsDir)) { throw "Mods\ missing from the zip -- refusing to write a broken package" }
    $keepFolders = $mods | ForEach-Object { $_.Name }
    Get-ChildItem $modsDir -Directory | Where-Object { $keepFolders -notcontains $_.Name } | ForEach-Object {
        Write-Host ("  pruned Mods\{0}\ (not ours)" -f $_.Name) -ForegroundColor DarkGray
        Remove-Item $_.FullName -Recurse -Force
    }
    # Loose files at the Mods root: only mods.txt belongs there.
    Get-ChildItem $modsDir -File | Where-Object { $_.Name -ne "mods.txt" } | ForEach-Object {
        Write-Host ("  pruned Mods\{0}" -f $_.Name) -ForegroundColor DarkGray
        Remove-Item $_.FullName -Force
    }
    # Inside each of OUR mods, keep dlls\ and whatever $modExtras names. Nothing else.
    foreach ($m in $mods) {
        $md = Join-Path $modsDir $m.Name
        if (-not (Test-Path $md)) { continue }
        Get-ChildItem $md -Recurse -File | ForEach-Object {
            $rel = $_.FullName.Substring($modsDir.Length + 1)
            $keep = ($rel -like ($m.Name + "\dlls\*"))
            if (-not $keep) { foreach ($x in $modExtras) { if ($rel -like $x) { $keep = $true; break } } }
            if (-not $keep) {
                Write-Host ("  pruned Mods\{0}" -f $rel) -ForegroundColor DarkGray
                Remove-Item $_.FullName -Force
            }
        }
        Get-ChildItem $md -Recurse -Directory | Sort-Object FullName -Descending | ForEach-Object {
            if (-not (Get-ChildItem $_.FullName -Recurse -File)) { Remove-Item $_.FullName -Recurse -Force }
        }
    }

    # ---- mods.txt, written from the table rather than edited in place. UE4SS loads exactly what is
    # listed here, and now that the stock Lua mods are gone there is no ordering to preserve (the old
    # "keep the Keybinds block last" rule went with them).
    $modsTxt = Join-Path $stage "Mods\mods.txt"
    $lines = @("; SessionOpenMP tester package. Only the mods this package actually ships are listed;",
               "; UE4SS's own sample and debug Lua mods are deliberately not included.")
    foreach ($m in $mods) {
        if (-not (Test-Path $m.Dll)) { continue }
        $lines += ($m.Name + " : 1")
    }
    Set-Content -Path $modsTxt -Value $lines -Encoding ASCII
    Write-Host ("  mods.txt: {0}" -f (($mods | Where-Object { Test-Path $_.Dll } | ForEach-Object { $_.Name }) -join ", "))

    $missing = @()
    foreach ($r in $required) { if (-not (Test-Path (Join-Path $stage $r))) { $missing += $r } }
    if ($missing.Count -gt 0) { throw ("REFUSING to write the zip -- missing: " + ($missing -join ", ")) }

    if (Test-Path $Zip) { Remove-Item $Zip -Force }
    Compress-Archive -Path (Join-Path $stage "*") -DestinationPath $Zip -CompressionLevel Optimal
    $mb = [Math]::Round((Get-Item $Zip).Length / 1MB, 1)
    Write-Host ("wrote {0} ({1} MB)" -f $Zip, $mb) -ForegroundColor Green
}
finally {
    if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
}
