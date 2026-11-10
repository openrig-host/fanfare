<#
.SYNOPSIS
    Builds Fanfare Windows installers (.msi and .msix) for Microsoft Store & direct distribution.

.DESCRIPTION
    1. Builds a standalone x64 Windows Installer (.msi) with embedded CAB using WiX 3.14
       (automatically downloads portable WiX binaries into installer/.tools/wix314 if needed).
    2. Builds a Microsoft Store Desktop Bridge package (.msix) using Windows SDK makeappx.exe,
       auto-generating exact required Store PNG visual assets (50x50, 44x44, 150x150, 310x150)
       from Source/Resources/Fanfare_Icon.png.
    3. Optionally signs binaries with signtool.exe (-CertThumbprint or -PfxPath) and copies
       the versioned .msi into docs/downloads/ (-PublishToDocs) so GitHub Pages serves it on
       a direct HTTP 200 non-redirecting URL.

.EXAMPLE
    # Build v1.51 .msi + .msix and stage the .msi for GitHub Pages non-redirect URL:
    powershell -ExecutionPolicy Bypass -File tools\build_installers.ps1 -Version 1.51.0 -DisplayVersion 1.51 -PublishToDocs

.EXAMPLE
    # Re-stamp the .msix with exact Microsoft Partner Center Product Identity values:
    powershell -ExecutionPolicy Bypass -File tools\build_installers.ps1 `
        -Version 1.51.0 -DisplayVersion 1.51 `
        -StoreIdentityName "12345YourPublisher.Fanfare" `
        -StorePublisher "CN=12345678-ABCD-EF01-2345-6789ABCDEF01" `
        -StorePublisherDisplayName "Your Publisher Name"
#>
[CmdletBinding()]
param(
    [string]$ExePath = "",
    [string]$Version = "1.51.0",
    [string]$DisplayVersion = "1.51",
    [ValidateSet("Both", "Msi", "Msix")]
    [string]$Format = "Both",
    [switch]$PublishToDocs,

    # Microsoft Store Partner Center -> Product Management -> Product Identity values (for .msix)
    [string]$StoreIdentityName = "OpenRigHost.Fanfare",
    [string]$StorePublisher = "CN=OpenRigHost",
    [string]$StoreDisplayName = "Fanfare",
    [string]$StorePublisherDisplayName = "Fanfare",

    # Optional Authenticode signing (required by Partner Center for external Win32 .msi/.exe URLs;
    # NOT required for .msix uploaded directly to Partner Center)
    [string]$CertThumbprint = "",
    [string]$PfxPath = "",
    [string]$PfxPassword = "",
    [string]$TimestampUrl = "http://timestamp.digicert.com",

    # Optional override path to pre-downloaded WiX 3.14 folder
    [string]$WixBinDir = ""
)

$ErrorActionPreference = "Stop"
$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path

if ([string]::IsNullOrWhiteSpace($ExePath)) {
    $ExePath = Join-Path $RepoRoot "Fanfare.exe"
}
if (-not (Test-Path -LiteralPath $ExePath)) {
    throw "Fanfare executable not found at: $ExePath"
}
$ExePath = (Resolve-Path -LiteralPath $ExePath).Path

$IconIcoPath = Join-Path $RepoRoot "Source\Resources\icon.ico"
$IconPngPath = Join-Path $RepoRoot "Source\Resources\Fanfare_Icon.png"
if (-not (Test-Path -LiteralPath $IconIcoPath)) { throw "Missing icon.ico at $IconIcoPath" }
if (-not (Test-Path -LiteralPath $IconPngPath)) { throw "Missing Fanfare_Icon.png at $IconPngPath" }

# Normalize 3-part MSI version (X.Y.Z) and 4-part MSIX version (X.Y.Z.0)
$verParts = $Version.Split('.')
while ($verParts.Count -lt 3) { $verParts += "0" }
$MsiVersion = "$($verParts[0]).$($verParts[1]).$($verParts[2])"
$MsixVersion = if ($verParts.Count -ge 4) {
    "$($verParts[0]).$($verParts[1]).$($verParts[2]).$($verParts[3])"
} else {
    "$MsiVersion.0"
}

$InstallerDir = Join-Path $RepoRoot "installer"
$OutDir = Join-Path $InstallerDir "output"
# Stage temporary files in %TEMP% so SMB/UNC workspace paths never trip Win32 CLI tools
$TempWorkRoot = Join-Path $env:TEMP "fanfare_pkg_build"
if (Test-Path -LiteralPath $TempWorkRoot) {
    Remove-Item -LiteralPath $TempWorkRoot -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $OutDir, $TempWorkRoot | Out-Null

# Locate signtool.exe if signing is requested
function Find-WindowsSdkTool([string]$ToolName) {
    $cmd = Get-Command $ToolName -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    $candidates = Get-ChildItem "C:\Program Files (x86)\Windows Kits\10\bin\*\x64\$ToolName" -ErrorAction SilentlyContinue |
                  Sort-Object FullName -Descending
    if ($candidates) { return $candidates[0].FullName }
    return $null
}

function Invoke-CodeSign([string]$TargetFile) {
    if ([string]::IsNullOrWhiteSpace($CertThumbprint) -and [string]::IsNullOrWhiteSpace($PfxPath)) {
        return
    }
    $signtool = Find-WindowsSdkTool "signtool.exe"
    if (-not $signtool) {
        throw "Signing requested, but signtool.exe was not found in Windows Kits."
    }
    Write-Host "Signing $TargetFile ..." -ForegroundColor Cyan
    if (-not [string]::IsNullOrWhiteSpace($CertThumbprint)) {
        & $signtool sign /fd SHA256 /sha1 $CertThumbprint /tr $TimestampUrl /td SHA256 $TargetFile
    } else {
        if ([string]::IsNullOrWhiteSpace($PfxPassword)) {
            & $signtool sign /fd SHA256 /f $PfxPath /tr $TimestampUrl /td SHA256 $TargetFile
        } else {
            & $signtool sign /fd SHA256 /f $PfxPath /p $PfxPassword /tr $TimestampUrl /td SHA256 $TargetFile
        }
    }
    if ($LASTEXITCODE -ne 0) { throw "signtool.exe failed on $TargetFile (exit $LASTEXITCODE)" }
}

# Copy Fanfare.exe & icons into local temp workspace (and sign the staged copy if requested)
$StagedExe = Join-Path $TempWorkRoot "Fanfare.exe"
$StagedIco = Join-Path $TempWorkRoot "icon.ico"
Copy-Item -LiteralPath $ExePath -Destination $StagedExe -Force
Copy-Item -LiteralPath $IconIcoPath -Destination $StagedIco -Force
Invoke-CodeSign $StagedExe

$BuiltFiles = @()

# ==============================================================================
# 1. BUILD WINDOWS INSTALLER (.MSI)
# ==============================================================================
if ($Format -eq "Both" -or $Format -eq "Msi") {
    Write-Host "`n=== [1/2] Building x64 Windows Installer (.msi) v$MsiVersion ===" -ForegroundColor Cyan

    if ([string]::IsNullOrWhiteSpace($WixBinDir)) {
        $candleCmd = Get-Command "candle.exe" -ErrorAction SilentlyContinue
        if ($candleCmd) {
            $WixBinDir = Split-Path -Parent $candleCmd.Source
        } else {
            $LocalWixDir = Join-Path $InstallerDir ".tools\wix314"
            if (-not (Test-Path -LiteralPath (Join-Path $LocalWixDir "candle.exe"))) {
                Write-Host "Downloading portable WiX Toolset 3.14.1 binaries into $LocalWixDir ..."
                New-Item -ItemType Directory -Force -Path $LocalWixDir | Out-Null
                $wixZip = Join-Path $TempWorkRoot "wix314-binaries.zip"
                & curl.exe -sL "https://github.com/wixtoolset/wix3/releases/download/wix3141rtm/wix314-binaries.zip" -o $wixZip
                if ($LASTEXITCODE -ne 0) { throw "Failed to download wix314-binaries.zip" }
                Expand-Archive -Force -Path $wixZip -DestinationPath $LocalWixDir
            }
            $WixBinDir = $LocalWixDir
        }
    }

    $CandleExe = Join-Path $WixBinDir "candle.exe"
    $LightExe  = Join-Path $WixBinDir "light.exe"
    if (-not (Test-Path -LiteralPath $CandleExe)) { throw "candle.exe not found at $CandleExe" }
    if (-not (Test-Path -LiteralPath $LightExe))  { throw "light.exe not found at $LightExe" }

    $WxsSource = Join-Path $InstallerDir "Fanfare.wxs"
    $StagedWxs = Join-Path $TempWorkRoot "Fanfare.wxs"
    $WixObj    = Join-Path $TempWorkRoot "Fanfare.wixobj"
    $TempMsi   = Join-Path $TempWorkRoot "Fanfare-$MsiVersion-x64.msi"
    $FinalMsi  = Join-Path $OutDir "Fanfare-$MsiVersion-x64.msi"

    Copy-Item -LiteralPath $WxsSource -Destination $StagedWxs -Force

    & $CandleExe -nologo -arch x64 `
        "-dProductVersion=$MsiVersion" `
        "-dDisplayVersion=$DisplayVersion" `
        "-dExePath=$StagedExe" `
        "-dIconPath=$StagedIco" `
        -out $WixObj $StagedWxs
    if ($LASTEXITCODE -ne 0) { throw "WiX candle.exe failed (exit $LASTEXITCODE)" }

    & $LightExe -nologo -out $TempMsi $WixObj
    if ($LASTEXITCODE -ne 0) { throw "WiX light.exe failed (exit $LASTEXITCODE)" }

    Invoke-CodeSign $TempMsi
    Copy-Item -LiteralPath $TempMsi -Destination $FinalMsi -Force
    $BuiltFiles += $FinalMsi

    $msiHash = (Get-FileHash -LiteralPath $FinalMsi -Algorithm SHA256).Hash
    $msiSize = (Get-Item -LiteralPath $FinalMsi).Length
    Write-Host "Built MSI: $FinalMsi ($msiSize bytes)" -ForegroundColor Green
    Write-Host "  SHA256 : $msiHash"
    Write-Host "  Silent : msiexec /i Fanfare-$MsiVersion-x64.msi /quiet /norestart"

    if ($PublishToDocs) {
        $DocsDownloads = Join-Path $RepoRoot "docs\downloads"
        New-Item -ItemType Directory -Force -Path $DocsDownloads | Out-Null
        $DocsMsi = Join-Path $DocsDownloads "Fanfare-$MsiVersion-x64.msi"
        Copy-Item -LiteralPath $FinalMsi -Destination $DocsMsi -Force
        Write-Host "Staged for GitHub Pages (HTTP 200 non-redirect URL):" -ForegroundColor Green
        Write-Host "  Local  : $DocsMsi"
        Write-Host "  Direct : https://openrig-host.github.io/fanfare/downloads/Fanfare-$MsiVersion-x64.msi"
    }
}

# ==============================================================================
# 2. BUILD MICROSOFT STORE PACKAGE (.MSIX)
# ==============================================================================
if ($Format -eq "Both" -or $Format -eq "Msix") {
    Write-Host "`n=== [2/2] Building Microsoft Store Package (.msix) v$MsixVersion ===" -ForegroundColor Cyan

    $MakeAppx = Find-WindowsSdkTool "makeappx.exe"
    if (-not $MakeAppx) {
        throw "makeappx.exe not found. Install Windows 10/11 SDK to build .msix packages."
    }

    $MsixStage = Join-Path $TempWorkRoot "msix_stage"
    $AssetsDir = Join-Path $MsixStage "Assets"
    New-Item -ItemType Directory -Force -Path $MsixStage, $AssetsDir | Out-Null

    Copy-Item -LiteralPath $StagedExe -Destination (Join-Path $MsixStage "Fanfare.exe") -Force

    # Generate exact Store PNG visual assets from Source/Resources/Fanfare_Icon.png
    Add-Type -AssemblyName System.Drawing
    function New-ResizedTile([string]$SrcPath, [string]$DstPath, [int]$CanvasW, [int]$CanvasH) {
        $srcImg = [System.Drawing.Image]::FromFile($SrcPath)
        try {
            $bmp = New-Object System.Drawing.Bitmap($CanvasW, $CanvasH, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
            try {
                $gfx = [System.Drawing.Graphics]::FromImage($bmp)
                try {
                    $gfx.Clear([System.Drawing.Color]::Transparent)
                    $gfx.InterpolationMode  = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
                    $gfx.SmoothingMode      = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
                    $gfx.PixelOffsetMode    = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
                    $gfx.CompositingQuality = [System.Drawing.Drawing2D.CompositingQuality]::HighQuality

                    $iconSide = [Math]::Min($CanvasW, $CanvasH)
                    $drawX = [int](($CanvasW - $iconSide) / 2)
                    $drawY = [int](($CanvasH - $iconSide) / 2)
                    $gfx.DrawImage($srcImg, $drawX, $drawY, $iconSide, $iconSide)
                } finally {
                    $gfx.Dispose()
                }
                $bmp.Save($DstPath, [System.Drawing.Imaging.ImageFormat]::Png)
            } finally {
                $bmp.Dispose()
            }
        } finally {
            $srcImg.Dispose()
        }
    }

    New-ResizedTile $IconPngPath (Join-Path $AssetsDir "StoreLogo.png")        50  50
    New-ResizedTile $IconPngPath (Join-Path $AssetsDir "Square44x44Logo.png")  44  44
    New-ResizedTile $IconPngPath (Join-Path $AssetsDir "Square150x150Logo.png") 150 150
    New-ResizedTile $IconPngPath (Join-Path $AssetsDir "Wide310x150Logo.png")  310 150

    # Render AppxManifest.xml
    $ManifestTpl = Get-Content -LiteralPath (Join-Path $InstallerDir "AppxManifest.template.xml") -Raw -Encoding UTF8
    $ManifestXml = $ManifestTpl.
        Replace("__IDENTITY_NAME__",          [System.Security.SecurityElement]::Escape($StoreIdentityName)).
        Replace("__PUBLISHER__",              [System.Security.SecurityElement]::Escape($StorePublisher)).
        Replace("__MSIX_VERSION__",           [System.Security.SecurityElement]::Escape($MsixVersion)).
        Replace("__DISPLAY_NAME__",           [System.Security.SecurityElement]::Escape($StoreDisplayName)).
        Replace("__PUBLISHER_DISPLAY_NAME__", [System.Security.SecurityElement]::Escape($StorePublisherDisplayName))

    $Utf8NoBom = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText((Join-Path $MsixStage "AppxManifest.xml"), $ManifestXml, $Utf8NoBom)

    $TempMsix  = Join-Path $TempWorkRoot "Fanfare-$MsiVersion-x64.msix"
    $FinalMsix = Join-Path $OutDir "Fanfare-$MsiVersion-x64.msix"

    & $MakeAppx pack /d $MsixStage /p $TempMsix /o | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "makeappx.exe failed (exit $LASTEXITCODE)" }

    Invoke-CodeSign $TempMsix
    Copy-Item -LiteralPath $TempMsix -Destination $FinalMsix -Force
    $BuiltFiles += $FinalMsix

    $msixHash = (Get-FileHash -LiteralPath $FinalMsix -Algorithm SHA256).Hash
    $msixSize = (Get-Item -LiteralPath $FinalMsix).Length
    Write-Host "Built MSIX: $FinalMsix ($msixSize bytes)" -ForegroundColor Green
    Write-Host "  SHA256   : $msixHash"
    Write-Host "  Identity : Name='$StoreIdentityName' | Publisher='$StorePublisher'"
    if ($StoreIdentityName -eq "OpenRigHost.Fanfare") {
        Write-Host "  NOTE     : Before uploading .msix to Partner Center, re-run with your exact Partner Center" -ForegroundColor Yellow
        Write-Host "             Product Identity (-StoreIdentityName and -StorePublisher)." -ForegroundColor Yellow
    }
}

# Clean up temp folder
Remove-Item -LiteralPath $TempWorkRoot -Recurse -Force -ErrorAction SilentlyContinue

Write-Host "`nDone! Generated $($BuiltFiles.Count) package(s)." -ForegroundColor Green
