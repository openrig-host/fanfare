<#
.SYNOPSIS
    Fanfare Rig Doctor CLI - Diagnose, diff, and surgically rehabilitate Fanfare .json song setups.

.EXAMPLE
    # Inspect a specific setup (checks JSON, VST3 paths, patch sizes, .bak diff, and OpenRig_log clues):
    powershell -ExecutionPolicy Bypass -File tools\rig_doctor.ps1 -Path "$env:APPDATA\Fanfare\songs\Core Setups\Last Waltz -Oct 2026 - Nektar-1.json"

.EXAMPLE
    # Audit all setups in %APPDATA%\Fanfare\songs:
    powershell -ExecutionPolicy Bypass -File tools\rig_doctor.ps1 -ScanAll

.EXAMPLE
    # Surgically remove Strip 16 chain 2 into a new repaired copy without touching the original:
    powershell -ExecutionPolicy Bypass -File tools\rig_doctor.ps1 -Path "$env:APPDATA\Fanfare\songs\Core Setups\Last Waltz -Oct 2026 - Nektar-1.json" -RemovePlugin "16:2" -SaveCopy
#>
param(
    [string]$Path = "",
    [switch]$ScanAll,
    [string[]]$RemovePlugin = @(),    # Format: "Strip:Chain" (1-based, e.g. "16:2", or "Aux1:1", "FOH:1", "IEM:1")
    [string[]]$ResetPatch = @(),      # Format: "Strip:Chain" (clears Base64 state to "" while keeping plugin)
    [string[]]$DisablePlugin = @(),   # Format: "Strip:Chain" (sets enabled = false)
    [ValidateSet("", "true", "false")]
    [string]$CleanSlate = "",
    [switch]$RestoreBak,
    [switch]$SaveCopy,
    [switch]$InPlace
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$fanfareDir = Join-Path $env:APPDATA "Fanfare"
$songsDir   = Join-Path $fanfareDir "songs"
$logFile    = Join-Path $fanfareDir "OpenRig_log.txt"
$logBakFile = Join-Path $fanfareDir "OpenRig_log.bak.txt"

function Test-RequiresStaThread([string]$vstPath) {
    if ([string]::IsNullOrWhiteSpace($vstPath)) { return $false }
    $patterns = @(
        "Super 8", "Kontakt", "Replika", "Raum", "Massive", "Komplete", "Guitar Rig",
        "Phasis", "Flair", "Choral", "Bite", "Dirt", "Freak", "Supercharger", "Driver",
        "Solid Bus", "Solid Dynamics", "Solid EQ", "Transient Master",
        "VC 2A", "VC 76", "VC 160", "RC 24", "RC 48", "FM8", "Absynth", "Reaktor", "Battery",
        "Native Instruments", "HALion", "EZkeys", "EZdrummer", "EZbass", "Superior Drummer", "Toontrack",
        "Omnisphere", "Keyscape", "Trilian", "Stylus RMX", "Spectrasonics"
    )
    foreach ($p in $patterns) {
        if ($vstPath.IndexOf($p, [System.StringComparison]::OrdinalIgnoreCase) -ge 0) {
            return $true
        }
    }
    return $false
}

function Format-ByteSize([long]$bytes) {
    if ($bytes -ge 1MB) { return "{0:N2} MB" -f ($bytes / 1MB) }
    if ($bytes -ge 1KB) { return "{0:N1} KB" -f ($bytes / 1KB) }
    return "$bytes B"
}

function Get-PluginMap($rigObj) {
    $map = [ordered]@{}
    if ($null -ne $rigObj.PSObject.Properties["channels"] -and $null -ne $rigObj.channels) {
        for ($i = 0; $i -lt $rigObj.channels.Count; $i++) {
            $ch = $rigObj.channels[$i]
            $sName = if ($null -ne $ch.PSObject.Properties["name"]) { [string]$ch.name } else { "Slot $($i+1)" }
            if ($null -ne $ch.PSObject.Properties["chain"] -and $null -ne $ch.chain) {
                for ($c = 0; $c -lt $ch.chain.Count; $c++) {
                    $pv = $ch.chain[$c]
                    if ($null -eq $pv) { continue }
                    $key = "$($i+1):$($c+1)"
                    $map[$key] = [PSCustomObject]@{
                        Key      = $key
                        Location = "Strip $($i+1) c$($c+1) ($sName)"
                        Name     = [string]$pv.name
                        Path     = [string]$pv.path
                        State    = if ($null -ne $pv.PSObject.Properties["state"]) { [string]$pv.state } else { "" }
                        Enabled  = if ($null -ne $pv.PSObject.Properties["enabled"]) { [bool]$pv.enabled } else { $true }
                    }
                }
            }
        }
    }
    if ($null -ne $rigObj.PSObject.Properties["auxReturns"] -and $null -ne $rigObj.auxReturns) {
        for ($a = 0; $a -lt $rigObj.auxReturns.Count; $a++) {
            $ax = $rigObj.auxReturns[$a]
            if ($null -ne $ax.PSObject.Properties["chain"] -and $null -ne $ax.chain) {
                for ($c = 0; $c -lt $ax.chain.Count; $c++) {
                    $pv = $ax.chain[$c]
                    if ($null -eq $pv) { continue }
                    $key = "Aux$($a+1):$($c+1)"
                    $map[$key] = [PSCustomObject]@{
                        Key      = $key
                        Location = "Aux $($a+1) c$($c+1)"
                        Name     = [string]$pv.name
                        Path     = [string]$pv.path
                        State    = if ($null -ne $pv.PSObject.Properties["state"]) { [string]$pv.state } else { "" }
                        Enabled  = if ($null -ne $pv.PSObject.Properties["enabled"]) { [bool]$pv.enabled } else { $true }
                    }
                }
            }
        }
    }
    foreach ($bus in @(@{Prop="fohChain"; Prefix="FOH"}, @{Prop="iemChain"; Prefix="IEM"})) {
        $pName = $bus.Prop
        if ($null -ne $rigObj.PSObject.Properties[$pName] -and $null -ne $rigObj.$pName) {
            for ($c = 0; $c -lt $rigObj.$pName.Count; $c++) {
                $pv = $rigObj.$pName[$c]
                if ($null -eq $pv) { continue }
                $key = "$($bus.Prefix):$($c+1)"
                $map[$key] = [PSCustomObject]@{
                    Key      = $key
                    Location = "Master $($bus.Prefix) c$($c+1)"
                    Name     = [string]$pv.name
                    Path     = [string]$pv.path
                    State    = if ($null -ne $pv.PSObject.Properties["state"]) { [string]$pv.state } else { "" }
                    Enabled  = if ($null -ne $pv.PSObject.Properties["enabled"]) { [bool]$pv.enabled } else { $true }
                }
            }
        }
    }
    return $map
}

function Save-HistoryBackup([string]$targetPath) {
    if (-not (Test-Path -LiteralPath $targetPath)) { return }
    $item = Get-Item -LiteralPath $targetPath
    $histDir = Join-Path $fanfareDir ("backups\history\songs\" + $item.BaseName)
    New-Item -ItemType Directory -Path $histDir -Force | Out-Null
    $stamp = (Get-Date).ToString("yyyyMMdd-HHmmss")
    $snapPath = Join-Path $histDir ($stamp + $item.Extension)
    Copy-Item -LiteralPath $targetPath -Destination $snapPath -Force
    Copy-Item -LiteralPath $targetPath -Destination ($targetPath + ".bak") -Force
}

if ($ScanAll) {
    Write-Host "=== Scanning all Fanfare setups in $songsDir ===" -ForegroundColor Cyan
    $files = Get-ChildItem -LiteralPath $songsDir -Recurse -Filter "*.json" | Sort-Object FullName
    foreach ($f in $files) {
        try {
            $raw = [System.IO.File]::ReadAllText($f.FullName)
            $rig = $raw | ConvertFrom-Json
            $pmap = Get-PluginMap $rig
            $missing = @($pmap.Values | Where-Object { -not (Test-Path -LiteralPath $_.Path) })
            $bakPath = $f.FullName + ".bak"
            $diffCount = 0
            if (Test-Path -LiteralPath $bakPath) {
                try {
                    $bakMap = Get-PluginMap ([System.IO.File]::ReadAllText($bakPath) | ConvertFrom-Json)
                    foreach ($k in $pmap.Keys) {
                        if (-not $bakMap.Contains($k) -or $bakMap[$k].Path -ne $pmap[$k].Path -or $bakMap[$k].State -ne $pmap[$k].State) {
                            $diffCount++
                        }
                    }
                } catch {}
            }
            $status = if ($missing.Count -gt 0) { "MISSING_VST($($missing.Count))" } else { "OK" }
            $color  = if ($missing.Count -gt 0) { "Yellow" } else { "Green" }
            $bakTag = if ($diffCount -gt 0) { " [+$diffCount diff vs .bak]" } else { "" }
            Write-Host ("[{0}] {1} ({2}, {3} plugins){4}" -f $status, $f.Name, (Format-ByteSize $f.Length), $pmap.Count, $bakTag) -ForegroundColor $color
        } catch {
            Write-Host ("[CORRUPT JSON] {0}: {1}" -f $f.FullName, $_.Exception.Message) -ForegroundColor Red
        }
    }
    exit 0
}

if ([string]::IsNullOrWhiteSpace($Path)) {
    Write-Host "Usage: .\tools\rig_doctor.ps1 -Path <song.json> [-RemovePlugin '16:2'] [-ResetPatch '2:1'] [-SaveCopy | -InPlace] | -ScanAll" -ForegroundColor Yellow
    exit 1
}

if (-not (Test-Path -LiteralPath $Path)) {
    throw "Song file not found: $Path"
}

$fileItem = Get-Item -LiteralPath $Path
$bakPath  = $fileItem.FullName + ".bak"

if ($RestoreBak) {
    if (-not (Test-Path -LiteralPath $bakPath)) {
        throw "No .bak file exists next to $($fileItem.FullName)"
    }
    $bakCopy = [System.IO.File]::ReadAllBytes($bakPath)
    Save-HistoryBackup $fileItem.FullName
    [System.IO.File]::WriteAllBytes($fileItem.FullName, $bakCopy)
    Write-Host "Restored $($fileItem.Name) from .bak (previous version saved in backups\history)." -ForegroundColor Green
    exit 0
}

$rawJson = [System.IO.File]::ReadAllText($fileItem.FullName)
$rig = $rawJson | ConvertFrom-Json
$pmap = Get-PluginMap $rig

$bakMap = $null
if (Test-Path -LiteralPath $bakPath) {
    try {
        $bakMap = Get-PluginMap ([System.IO.File]::ReadAllText($bakPath) | ConvertFrom-Json)
    } catch {}
}

$fresh = if ($null -ne $rig.PSObject.Properties["freshBuild"]) { [bool]$rig.freshBuild } else { $false }
$chCount = if ($null -ne $rig.PSObject.Properties["channels"] -and $null -ne $rig.channels) { $rig.channels.Count } else { 0 }
$scCount = if ($null -ne $rig.PSObject.Properties["scenes"] -and $null -ne $rig.scenes) { $rig.scenes.Count } else { 0 }

Write-Host ""
Write-Host ("=== RIG DOCTOR: {0} ===" -f $fileItem.Name) -ForegroundColor Cyan
Write-Host ("Path       : {0}" -f $fileItem.FullName)
Write-Host ("Size / Mod : {0}  |  {1}" -f (Format-ByteSize $fileItem.Length), $fileItem.LastWriteTime)
Write-Host ("Summary    : {0} strips, {1} plugins, {2} scenes, CleanSlate={3}" -f $chCount, $pmap.Count, $scCount, $fresh)
Write-Host ""

$rows = foreach ($entry in $pmap.Values) {
    $exists = Test-Path -LiteralPath $entry.Path
    $sta = Test-RequiresStaThread $entry.Path
    $stateBytes = 0
    $b64Ok = $true
    if ($entry.State.Length -gt 0) {
        # JUCE MemoryBlock::toBase64Encoding format is "<byteLength>.<juceBase64Chars>"
        if ($entry.State -match '^(\d+)\.[A-Za-z0-9+\-.,]+$') {
            $stateBytes = [long]$Matches[1]
        } else {
            try {
                $stateBytes = ([Convert]::FromBase64String($entry.State)).Length
            } catch {
                $b64Ok = $false
            }
        }
    }
    $diff = ""
    if ($null -ne $bakMap) {
        if (-not $bakMap.Contains($entry.Key)) { $diff = "+NEW vs .bak" }
        elseif ($bakMap[$entry.Key].Path -ne $entry.Path) { $diff = "REPLACED ($($bakMap[$entry.Key].Name))" }
        elseif ($bakMap[$entry.Key].State -ne $entry.State) { $diff = "PATCH EDITED vs .bak" }
    }
    [PSCustomObject]@{
        Key    = $entry.Key
        Slot   = $entry.Location
        Plugin = if ($entry.Enabled) { $entry.Name } else { "$($entry.Name) (off)" }
        Thread = if ($sta) { "STA Msg" } else { "MTA Work" }
        Patch  = if ($b64Ok) { Format-ByteSize $stateBytes } else { "BAD_B64" }
        Disk   = if ($exists) { "OK" } else { "MISSING" }
        Diff   = $diff
    }
}
$rows | Format-Table -AutoSize

# Check recent log clues
$songStem = $fileItem.BaseName
$logLines = @()
foreach ($lf in @($logBakFile, $logFile)) {
    if (Test-Path -LiteralPath $lf) {
        $tail = Get-Content -LiteralPath $lf -Tail 400 -ErrorAction SilentlyContinue
        foreach ($ln in $tail) {
            if ($ln -match [regex]::Escape($songStem) -or $ln -match "stalled|timed out|poison|faulted|Failed to load rig") {
                $logLines += $ln
            }
        }
    }
}
if ($logLines.Count -gt 0) {
    Write-Host "--- Recent OpenRig_log Clues ---" -ForegroundColor Yellow
    $logLines | Select-Object -Last 10 | ForEach-Object { Write-Host "  $_" }
    Write-Host ""
}

# Apply requested surgical edits if any
$hasEdits = ($RemovePlugin.Count -gt 0) -or ($ResetPatch.Count -gt 0) -or ($DisablePlugin.Count -gt 0) -or ($CleanSlate -ne "")
if ($hasEdits) {
    if (-not $SaveCopy -and -not $InPlace) {
        throw "Specify -SaveCopy (writes '<Name> (Repaired).json') or -InPlace (backs up original first, then updates file)."
    }

    function Resolve-ChainTarget($rigRoot, [string]$spec) {
        if ($spec -match '^(\d+):(\d+)$') {
            $sIdx = [int]$Matches[1] - 1
            $cIdx = [int]$Matches[2] - 1
            return @{ Array = $rigRoot.channels[$sIdx].chain; Index = $cIdx }
        }
        if ($spec -match '^Aux(\d+):(\d+)$') {
            $aIdx = [int]$Matches[1] - 1
            $cIdx = [int]$Matches[2] - 1
            return @{ Array = $rigRoot.auxReturns[$aIdx].chain; Index = $cIdx }
        }
        if ($spec -match '^FOH:(\d+)$') {
            return @{ Array = $rigRoot.fohChain; Index = ([int]$Matches[1] - 1) }
        }
        if ($spec -match '^IEM:(\d+)$') {
            return @{ Array = $rigRoot.iemChain; Index = ([int]$Matches[1] - 1) }
        }
        throw "Invalid plugin key '$spec'. Expected 'Strip:Chain' (e.g. '16:2'), 'Aux1:1', 'FOH:1', or 'IEM:1'."
    }

    foreach ($spec in $RemovePlugin) {
        $t = Resolve-ChainTarget $rig $spec
        $t.Array[$t.Index] = $null
        Write-Host "[EDIT] Removed plugin at $spec" -ForegroundColor Yellow
    }
    foreach ($spec in $ResetPatch) {
        $t = Resolve-ChainTarget $rig $spec
        if ($null -ne $t.Array[$t.Index]) {
            $t.Array[$t.Index].state = ""
            Write-Host "[EDIT] Reset patch state at $spec" -ForegroundColor Yellow
        }
    }
    foreach ($spec in $DisablePlugin) {
        $t = Resolve-ChainTarget $rig $spec
        if ($null -ne $t.Array[$t.Index]) {
            $t.Array[$t.Index].enabled = $false
            Write-Host "[EDIT] Disabled plugin at $spec" -ForegroundColor Yellow
        }
    }
    if ($CleanSlate -ne "") {
        $rig | Add-Member -NotePropertyName "freshBuild" -NotePropertyValue ($CleanSlate -eq "true") -Force
        Write-Host "[EDIT] Set freshBuild = $CleanSlate" -ForegroundColor Yellow
    }

    $outJson = $rig | ConvertTo-Json -Depth 32
    if ($SaveCopy) {
        $outFile = Join-Path $fileItem.DirectoryName ($fileItem.BaseName + " (Repaired).json")
        [System.IO.File]::WriteAllText($outFile, $outJson, (New-Object System.Text.UTF8Encoding($false)))
        Write-Host "Saved repaired setup copy to: $outFile" -ForegroundColor Green
    } elseif ($InPlace) {
        Save-HistoryBackup $fileItem.FullName
        [System.IO.File]::WriteAllText($fileItem.FullName, $outJson, (New-Object System.Text.UTF8Encoding($false)))
        Write-Host "Repaired $($fileItem.FullName) in-place (backup saved to .bak and backups\history)." -ForegroundColor Green
    }
}
