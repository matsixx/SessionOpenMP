# SessionOpenMP updater.
#
# Fetches the latest release from GitHub and installs it over your existing copy, keeping your
# settings. Plain text on purpose -- read it before you run it, that is the point.
#
#   .\update.ps1            install the latest release if there is one
#   .\update.ps1 -Check     only report what is installed vs available, change nothing
#   .\update.ps1 -GameDir "D:\path\to\...\Binaries\Win64"
#   .\update.ps1 -ZipPath "C:\Downloads\SessionOpenMP-0.6.0b.zip"
#                           install a zip you already have, without contacting GitHub
#
# What it does, in order:
#   1. finds your game folder                (never guesses silently -- it tells you which one)
#   2. reads the version out of the installed main.dll
#   3. asks api.github.com for the latest release of matsixx/SessionOpenMP
#   4. downloads that release's .zip and prints its SHA-256
#   5. copies the new files in, SKIPPING the files that hold your settings
#
# What it will not do: run anything it downloads, contact any host other than GitHub, delete your
# settings, or touch anything outside the game's Win64 folder.
param(
    [switch]$Check,
    [string]$GameDir = "",
    [string]$ZipPath = ""
)

$ErrorActionPreference = "Stop"
$Repo = "matsixx/SessionOpenMP"

# Files that belong to YOU, not to the release. Never overwritten.
$Preserve = @(
    "SessionOpenMP_prefs.txt",     # F1 / pause-menu settings, including "Hide my address"
    "SessionOpenMP_bans.txt",      # your ban list
    "SessionTweaks.ini",           # SessionTweaks settings
    "UE4SS-settings.ini"           # UE4SS's own config, in case you have tuned it
)
# Mods\mods.txt is handled separately: your on/off choices are kept and any NEW mod is appended.

function Say([string]$m)  { Write-Host $m }
function Step([string]$m) { Write-Host "" ; Write-Host $m -ForegroundColor Cyan }
function Warn([string]$m) { Write-Host "  ! $m" -ForegroundColor Yellow }
function Good([string]$m) { Write-Host "  $m" -ForegroundColor Green }

# Older Windows defaults to TLS 1.0, which GitHub refuses.
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

# ---------------------------------------------------------------- 1. where is the game?
function Find-GameDir {
    if ($GameDir -ne "") { return $GameDir }
    # Beside this script is the normal case: you dropped the updater in the Win64 folder.
    $here = $PSScriptRoot
    if ($here -and (Test-Path (Join-Path $here "SessionGame-Win64-Shipping.exe"))) { return $here }
    # Otherwise try the usual install locations on every fixed drive.
    $tails = @(
        "Steam\steamapps\common\Session\SessionGame\Binaries\Win64",
        "SteamLibrary\steamapps\common\Session\SessionGame\Binaries\Win64",
        "Program Files\Epic Games\SessionSkateSim\SessionGame\Binaries\Win64",
        "Epic Games\SessionSkateSim\SessionGame\Binaries\Win64"
    )
    foreach ($d in [System.IO.DriveInfo]::GetDrives()) {
        if ($d.DriveType -ne "Fixed" -or -not $d.IsReady) { continue }
        foreach ($t in $tails) {
            $p = Join-Path $d.RootDirectory.FullName $t
            if (Test-Path (Join-Path $p "SessionGame-Win64-Shipping.exe")) { return $p }
        }
    }
    return ""
}

Step "Locating the game..."
$dir = Find-GameDir
if ($dir -eq "") {
    Warn "Could not find the game."
    Say  "  Put this script in the folder containing SessionGame-Win64-Shipping.exe and run it again,"
    Say  "  or pass the folder yourself:"
    Say  "      .\update.ps1 -GameDir `"D:\Games\Session\SessionGame\Binaries\Win64`""
    exit 1
}
Good $dir

$modDll = Join-Path $dir "Mods\SessionOpenMP\dlls\main.dll"

# ---------------------------------------------------------------- 2. what is installed?
function Get-InstalledVersion([string]$dll) {
    if (-not (Test-Path $dll)) { return "" }
    # The version is compiled into the DLL as a wide string: "  |  OpenMP 0.6.0b".
    $bytes = [System.IO.File]::ReadAllBytes($dll)
    $text  = [System.Text.Encoding]::Unicode.GetString($bytes)
    $m = [regex]::Match($text, 'OpenMP\s+(\d+\.\d+\.\d+[a-z]?)')
    if ($m.Success) { return $m.Groups[1].Value }
    return ""
}

Step "Checking what you have installed..."
$installed = Get-InstalledVersion $modDll
if ($installed -eq "") {
    if (Test-Path $modDll) { Warn "main.dll is there but its version could not be read -- treating as out of date" }
    else                   { Warn "SessionOpenMP is not installed here yet -- this will install it" }
} else {
    Good "SessionOpenMP $installed"
}

# ---------------------------------------------------------------- 3. what is available?
# A zip you already have skips GitHub entirely -- for installing offline, or from a build you made
# yourself. Everything downstream (validation, preserved files, mods.txt merge) is identical.
if ($ZipPath -ne "") {
    if (-not (Test-Path $ZipPath)) { Warn "No such file: $ZipPath"; exit 1 }
    Step "Using a local package..."
    Good (Split-Path $ZipPath -Leaf)
    $m = [regex]::Match((Split-Path $ZipPath -Leaf), 'SessionOpenMP-(\d+\.\d+\.\d+[a-z]?)')
    if ($m.Success) { $latest = $m.Groups[1].Value } else { $latest = "(local package)" }
    $latestTag = "v$latest"
    $localZip = $ZipPath
} else {
    $localZip = ""
}

if ($localZip -eq "") {
Step "Asking GitHub for the latest release..."
try {
    $rel = Invoke-RestMethod -Uri "https://api.github.com/repos/$Repo/releases/latest" `
                             -Headers @{ "User-Agent" = "SessionOpenMP-Updater" } -TimeoutSec 30
} catch {
    # 404 here means GitHub answered and said "this repo has no published releases" -- a different
    # thing from being unable to reach it, and worth saying so rather than blaming the connection.
    $code = 0
    if ($_.Exception.Response) { $code = [int]$_.Exception.Response.StatusCode }
    if ($code -eq 404) {
        Warn "No releases have been published for this repository yet."
        Say  "  Nothing to update to. Try again once a release is out:"
        Say  "      https://github.com/$Repo/releases"
    } elseif ($code -eq 403) {
        Warn "GitHub rate-limited this machine (HTTP 403). It resets within the hour."
        Say  "  Or download the release by hand: https://github.com/$Repo/releases/latest"
    } else {
        Warn "Could not reach GitHub: $($_.Exception.Message)"
        Say  "  Check your connection, or download the release by hand:"
        Say  "      https://github.com/$Repo/releases/latest"
    }
    exit 1
}

$latestTag = $rel.tag_name
$latest    = $latestTag -replace '^v', ''
Good "latest release: $latestTag"

$asset = $rel.assets | Where-Object { $_.name -like "SessionOpenMP-*.zip" } | Select-Object -First 1
if (-not $asset) {
    Warn "That release has no SessionOpenMP-*.zip attached to it."
    Say  "  Download it manually from https://github.com/$Repo/releases/latest"
    exit 1
}
}   # end: consult GitHub

if ($localZip -eq "" -and $installed -ne "" -and $installed -eq $latest) {
    Write-Host ""
    Good "You are up to date (SessionOpenMP $installed)."
    exit 0
}

Write-Host ""
if ($installed -eq "")            { Say "  Will install $latest" }
elseif ($installed -eq $latest)   { Say "  Reinstalling $latest" }   # only reachable with -ZipPath
else                              { Say "  Update available: $installed  ->  $latest" }

if ($Check) {
    Write-Host ""
    Say "  -Check was given, so nothing was changed."
    exit 0
}

# ---------------------------------------------------------------- 4. the game must be closed
# Only THIS install matters: someone with two copies installed can update one while playing the
# other. If the running process will not tell us its path (it can refuse when elevated), assume it
# is the one we are about to write to and stop -- overwriting a loaded DLL fails halfway.
$blocking = $false
foreach ($p in @(Get-Process -Name "SessionGame-Win64-Shipping" -ErrorAction SilentlyContinue)) {
    $exe = ""
    try { $exe = $p.Path } catch { $exe = "" }
    if ($exe -eq "") { $blocking = $true; break }
    if ($exe.ToLower().StartsWith($dir.ToLower())) { $blocking = $true; break }
}
if ($blocking) {
    Write-Host ""
    Warn "Session is running from this folder. Close the game and run this again -- its files are locked while it is open."
    exit 1
}

# ---------------------------------------------------------------- 5. get the package
$tmp = Join-Path $env:TEMP ("SessionOpenMP_update_" + [Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $tmp | Out-Null
try {
    if ($localZip -ne "") {
        $zip = $localZip
        Step "Reading the package..."
    } else {
        Step "Downloading $($asset.name) ($([Math]::Round($asset.size / 1MB, 1)) MB)..."
        $zip = Join-Path $tmp $asset.name
        $pref = $ProgressPreference; $ProgressPreference = "SilentlyContinue"  # the bar makes this ~10x slower
        Invoke-WebRequest -Uri $asset.browser_download_url -OutFile $zip `
                          -Headers @{ "User-Agent" = "SessionOpenMP-Updater" } -TimeoutSec 600
        $ProgressPreference = $pref
        Good "downloaded"
    }

    $sha = (Get-FileHash $zip -Algorithm SHA256).Hash
    Say  "  SHA-256: $sha"
    Say  "  (the release page lists this too -- compare them if you want to be sure)"

    # ------------------------------------------------------------ 6. sanity-check the package
    Step "Checking the package..."
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $stage = Join-Path $tmp "x"
    Expand-Archive -LiteralPath $zip -DestinationPath $stage -Force
    if (-not (Test-Path (Join-Path $stage "Mods\SessionOpenMP\dlls\main.dll"))) {
        throw "the downloaded zip does not look like a SessionOpenMP package (no Mods\SessionOpenMP\dlls\main.dll) -- nothing was changed"
    }
    Good "looks right"

    # ------------------------------------------------------------ 7. install
    Step "Installing..."
    $copied = 0; $kept = @()
    Get-ChildItem $stage -Recurse -File | ForEach-Object {
        $rel = $_.FullName.Substring($stage.Length + 1)
        $dst = Join-Path $dir $rel

        if ($Preserve -contains $rel -and (Test-Path $dst)) { $kept += $rel; return }
        if ($rel -eq "Mods\mods.txt" -and (Test-Path $dst)) { return }   # merged below

        $dstDir = Split-Path $dst
        if (-not (Test-Path $dstDir)) { New-Item -ItemType Directory -Path $dstDir -Force | Out-Null }
        Copy-Item $_.FullName $dst -Force
        $copied++
    }
    Good "$copied file(s) updated"
    foreach ($k in $kept) { Say "  kept your $k" }

    # ------------------------------------------------------------ 8. mods.txt, merged not replaced
    # Your on/off choices survive an update, but a mod added by a new release still gets enabled.
    $mtDst = Join-Path $dir "Mods\mods.txt"
    $mtSrc = Join-Path $stage "Mods\mods.txt"
    if ((Test-Path $mtDst) -and (Test-Path $mtSrc)) {
        $yours = @{}
        $order = @()
        foreach ($line in (Get-Content $mtDst)) {
            $m = [regex]::Match($line, '^\s*([A-Za-z0-9_.-]+)\s*:\s*([01])')
            if ($m.Success) { $yours[$m.Groups[1].Value] = $m.Groups[2].Value; $order += $m.Groups[1].Value }
        }
        $added = @()
        foreach ($line in (Get-Content $mtSrc)) {
            $m = [regex]::Match($line, '^\s*([A-Za-z0-9_.-]+)\s*:\s*([01])')
            if ($m.Success -and -not $yours.ContainsKey($m.Groups[1].Value)) {
                $yours[$m.Groups[1].Value] = $m.Groups[2].Value
                $order += $m.Groups[1].Value
                $added += $m.Groups[1].Value
            }
        }
        $out = @("; SessionOpenMP package. Set a mod to 0 to disable it; an update keeps your choice.")
        foreach ($n in $order) { $out += ("{0} : {1}" -f $n, $yours[$n]) }
        Set-Content -Path $mtDst -Value $out -Encoding ASCII
        if ($added.Count -gt 0) { Say ("  mods.txt: added " + ($added -join ", ") + ", kept your existing choices") }
        else                    { Say  "  mods.txt: kept your existing choices" }
    }

    Write-Host ""
    Good "Updated to SessionOpenMP $latest."
    Say  "  Release notes: https://github.com/$Repo/releases/tag/$latestTag"
}
finally {
    if (Test-Path $tmp) { Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue }
}
