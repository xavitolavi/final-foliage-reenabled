# Final Foliage Reenabled @VERSION@ - installer / uninstaller for FINAL FANTASY VII REBIRTH (PC, Steam)
#
#   Install.bat    -> powershell -File FinalFoliage-Setup.ps1 -Action install
#   Uninstall.bat  -> powershell -File FinalFoliage-Setup.ps1 -Action uninstall
#
# Options:  -GameDir "<game folder>"   folder that contains End\Binaries\Win64\ff7rebirth_.exe (found automatically otherwise)
#           -WindMeters <n>            how far foliage wind reaches (default 80; 0 = never fades)
#           -ResetSettings             replace an existing FinalFoliage.ini with the default one (the old one is kept as .bak)
#
# What install does:
#   1. Checks that ff7rebirth_.exe is the supported game build (SHA-256 below) and backs it up as
#      ff7rebirth_.exe.finalfoliage-original.
#   2. Writes a few hundred bytes into the exe: a tiny loader for FinalFoliage.dll and the wind-distance fix.
#      Nothing else in the exe changes; uninstall puts the original back.
#   3. Copies FinalFoliage.dll + FinalFoliage.ini to End\Binaries\Win64 and zz_FinalFoliage_P.pak to End\Content\Paks.
#      Files from older builds (FoliageBend.*) are removed; an old FoliageBend.ini becomes FinalFoliage.ini.
param(
    [ValidateSet('install', 'uninstall')][string]$Action = 'install',
    [string]$GameDir = '',
    [double]$WindMeters = 80,
    [switch]$ResetSettings
)
$ErrorActionPreference = 'Stop'

$OrigSha = '@ORIGSHA@'
$Patches = @(
@PATCHES@
)
$CaveOffset = @CAVEOFF@
$CaveBytes = '@CAVEHEX@'
$LimitOffsets = @(@LIMITOFFS@)
$RelExe = 'End\Binaries\Win64\ff7rebirth_.exe'

function HexBytes([string]$h) { $b = New-Object byte[] ($h.Length / 2); for ($i = 0; $i -lt $b.Length; $i++) { $b[$i] = [Convert]::ToByte($h.Substring(2 * $i, 2), 16) }; return ,$b }
function Sha([string]$p) { return (Get-FileHash -Algorithm SHA256 -LiteralPath $p).Hash.ToLower() }
function Say([string]$m, [string]$c = 'Gray') { Write-Host $m -ForegroundColor $c }

function Find-Game {
    $cands = New-Object System.Collections.Generic.List[string]
    if ($GameDir) { $cands.Add($GameDir) }
    $d = $PSScriptRoot
    while ($d) { $cands.Add($d); $p = Split-Path $d -Parent; if ($p -eq $d) { break }; $d = $p }
    try {
        $steam = (Get-ItemProperty -Path 'HKCU:\Software\Valve\Steam' -Name SteamPath -ErrorAction Stop).SteamPath
        $libs = @($steam)
        $vdf = Join-Path $steam 'steamapps\libraryfolders.vdf'
        if (Test-Path $vdf) { foreach ($m in [regex]::Matches((Get-Content -Raw $vdf), '"path"\s+"([^"]+)"')) { $libs += $m.Groups[1].Value.Replace('\\', '\') } }
        foreach ($l in $libs) { $cands.Add((Join-Path $l 'steamapps\common\FINAL FANTASY VII REBIRTH')) }
    } catch { }
    foreach ($c in $cands) { if ($c -and (Test-Path (Join-Path $c $RelExe))) { return (Resolve-Path $c).Path } }
    while ($true) {
        $c = Read-Host 'Game folder not found. Paste the FINAL FANTASY VII REBIRTH folder (the one that contains "End")'
        if (-not $c) { throw 'No game folder given.' }
        $c = $c.Trim('"')
        if (Test-Path (Join-Path $c $RelExe)) { return (Resolve-Path $c).Path }
        Say "  $RelExe is not in that folder." Yellow
    }
}

try {
    Say "Final Foliage Reenabled @VERSION@ - $Action" Cyan
    $game = Find-Game
    Say "Game folder: $game"
    $running = Get-Process -Name 'ff7rebirth*' -ErrorAction SilentlyContinue | Where-Object { -not $_.Path -or $_.Path.StartsWith($game, [StringComparison]::OrdinalIgnoreCase) }
    if ($running) { throw 'The game is running. Close it and run this again.' }
    $win64 = Join-Path $game 'End\Binaries\Win64'
    $paks = Join-Path $game 'End\Content\Paks'
    $exe = Join-Path $game $RelExe
    $bak = "$exe.finalfoliage-original"
    $legacyBaks = @("$exe.foliagebend-original", "$exe.windmod-original")     # from older / development builds
    $oldFiles = @((Join-Path $win64 'FoliageBend.dll'), (Join-Path $win64 'FoliageBend.log'), (Join-Path $win64 'FoliageBend_classes.txt'), (Join-Path $paks 'zz_FoliageBend_P.pak'))
    $modDir = Join-Path $PSScriptRoot 'mod'

    if ($Action -eq 'install') {
        # ---- pristine copy of the exe
        $src = $null
        if ((Test-Path $bak) -and (Sha $bak) -eq $OrigSha) { $src = $bak }
        else { foreach ($lb in $legacyBaks) { if (-not $src -and (Test-Path $lb) -and (Sha $lb) -eq $OrigSha) { Copy-Item -LiteralPath $lb -Destination $bak -Force; $src = $bak } } }
        if (-not $src) {
            if ((Sha $exe) -eq $OrigSha) { Say 'Backing up the original exe...'; Copy-Item -LiteralPath $exe -Destination $bak -Force; $src = $bak }
            else {
                throw ("ff7rebirth_.exe is not the game build this version supports (it was probably updated, or another mod changed it). " +
                       "If another mod patched the exe, restore it first (Steam: Properties > Installed Files > Verify integrity) and run Install again.")
            }
        }
        # the backup from the previous version is no longer needed
        $prev = "$exe.foliagebend-original"
        if ((Test-Path $prev) -and (Test-Path $bak)) { Remove-Item -LiteralPath $prev -Force }
        # ---- patch
        $data = [IO.File]::ReadAllBytes($src)
        foreach ($p in $Patches) {
            $old = HexBytes $p.Old
            for ($i = 0; $i -lt $old.Length; $i++) { if ($data[$p.Off + $i] -ne $old[$i]) { throw ('Unexpected bytes at 0x{0:X} - unsupported game build.' -f $p.Off) } }
        }
        $cave = HexBytes $CaveBytes
        for ($i = 0; $i -lt $cave.Length; $i++) { if ($data[$CaveOffset + $i] -ne 0) { throw 'The code cave in the exe is not empty - unsupported game build.' } }
        foreach ($p in $Patches) { $new = HexBytes $p.New; [Array]::Copy($new, 0, $data, $p.Off, $new.Length) }
        [Array]::Copy($cave, 0, $data, $CaveOffset, $cave.Length)
        if ($WindMeters -lt 0) { $WindMeters = 0 }
        $limit = if ($WindMeters -eq 0) { [single]0 } else { [single](1.0 / ($WindMeters * 100.0)) }
        $lb = [BitConverter]::GetBytes($limit)
        foreach ($o in $LimitOffsets) { [Array]::Copy($lb, 0, $data, $o, 4) }
        [IO.File]::WriteAllBytes($exe, $data)
        Say ("Patched ff7rebirth_.exe (DLL loader + wind kept to {0})." -f $(if ($WindMeters -eq 0) { 'any distance' } else { "$WindMeters m" })) Green
        # ---- files
        # files from older builds
        $oldIni = Join-Path $win64 'FoliageBend.ini'
        $ini = Join-Path $win64 'FinalFoliage.ini'
        $migrated = $false
        if ((Test-Path $oldIni) -and -not (Test-Path $ini)) { Move-Item -LiteralPath $oldIni -Destination $ini; $migrated = $true; Say 'Kept your settings from the previous version (FoliageBend.ini -> FinalFoliage.ini).' }
        elseif (Test-Path $oldIni) { Move-Item -LiteralPath $oldIni -Destination "$oldIni.bak" -Force }
        foreach ($f in $oldFiles) { if (Test-Path $f) { Remove-Item -LiteralPath $f -Force } }
        Copy-Item -LiteralPath (Join-Path $modDir 'FinalFoliage.dll') -Destination $win64 -Force
        if (Test-Path $ini) {
            if ($ResetSettings) { Copy-Item -LiteralPath $ini -Destination "$ini.bak" -Force; Copy-Item -LiteralPath (Join-Path $modDir 'FinalFoliage.ini') -Destination $ini -Force; Say 'Settings reset to defaults (old file kept as FinalFoliage.ini.bak).' }
            elseif (-not $migrated) { Say 'Kept your existing FinalFoliage.ini (use -ResetSettings to replace it).' }
        } else { Copy-Item -LiteralPath (Join-Path $modDir 'FinalFoliage.ini') -Destination $ini }
        if (-not (Test-Path $paks)) { New-Item -ItemType Directory -Path $paks | Out-Null }
        Copy-Item -LiteralPath (Join-Path $modDir 'zz_FinalFoliage_P.pak') -Destination $paks -Force
        Say 'Installed. Start the game normally.' Green
        Say "Settings: $ini (save it while the game runs to apply changes)."
    } else {
        $restored = $false
        foreach ($b in @($bak) + $legacyBaks) {
            if (-not $restored -and (Test-Path $b) -and (Sha $b) -eq $OrigSha) { Copy-Item -LiteralPath $b -Destination $exe -Force; $restored = $true }
        }
        if ($restored) {
            if ((Sha $exe) -ne $OrigSha) { throw 'Restoring the exe failed (hash mismatch). Use Steam > Verify integrity of game files.' }
            foreach ($b in @($bak, "$exe.foliagebend-original")) { Remove-Item -LiteralPath $b -Force -ErrorAction SilentlyContinue }
            Say 'Original ff7rebirth_.exe restored.' Green
        } elseif ((Sha $exe) -eq $OrigSha) {
            Say 'The exe is already the original.'
        } else {
            Say 'No backup of the original exe found. Use Steam > Properties > Installed Files > Verify integrity to restore it.' Yellow
        }
        foreach ($f in @((Join-Path $win64 'FinalFoliage.dll'), (Join-Path $win64 'FinalFoliage.log'), (Join-Path $win64 'FinalFoliage_classes.txt'), (Join-Path $paks 'zz_FinalFoliage_P.pak')) + $oldFiles) {
            if (Test-Path $f) { Remove-Item -LiteralPath $f -Force }
        }
        $ini = Join-Path $win64 'FinalFoliage.ini'
        if (Test-Path $ini) { Move-Item -LiteralPath $ini -Destination "$ini.bak" -Force; Say 'Your settings were kept as FinalFoliage.ini.bak.' }
        Say 'Uninstalled.' Green
    }
} catch {
    Say ''
    Say ("ERROR: " + $_.Exception.Message) Red
    if ($_.Exception -is [UnauthorizedAccessException]) { Say 'Access denied: run Install.bat / Uninstall.bat as administrator.' Yellow }
    exit 1
}
