[CmdletBinding()]
param(
    [string]$PackageDir = 'artifacts\release',
    [string]$OutputDir = 'artifacts\installer'
)

$ErrorActionPreference = 'Stop'
$RepositoryRoot = Split-Path -Parent $PSScriptRoot

function Read-ProductVersion {
    $versionHeader = Get-Content -LiteralPath `
        (Join-Path $RepositoryRoot 'src\common\version.h') -Raw
    if ($versionHeader -notmatch '#define\s+SHURU_VERSION_STRING\s+"([^"]+)"') {
        throw 'version string missing from src/common/version.h'
    }
    return $Matches[1]
}

$productVersion = Read-ProductVersion
$packageRoot = Join-Path $RepositoryRoot $PackageDir
$outputDirectory = Join-Path $RepositoryRoot $OutputDir

if (-not (Test-Path -LiteralPath $packageRoot -PathType Container)) {
    throw "Release package directory missing: $packageRoot. Please build the project first."
}

# Create temp dir for staging
$tempDir = Join-Path $RepositoryRoot 'portable_temp'
if (Test-Path -LiteralPath $tempDir) {
    Remove-Item -Recurse -Force -LiteralPath $tempDir
}
New-Item -ItemType Directory -Force -Path $tempDir | Out-Null

# Copy release files to temp dir
Copy-Item -Path "$packageRoot\*" -Destination $tempDir -Recurse -Force

# Copy portable install/uninstall batch scripts and PowerShell helpers
$portableSrc = Join-Path $RepositoryRoot 'installer\portable'
Copy-Item -LiteralPath (Join-Path $portableSrc 'install.bat') -Destination (Join-Path $tempDir '安装(以管理员身份运行).bat') -Force
Copy-Item -LiteralPath (Join-Path $portableSrc 'install.bat') -Destination (Join-Path $tempDir '安装.bat') -Force
Copy-Item -LiteralPath (Join-Path $portableSrc 'uninstall.bat') -Destination (Join-Path $tempDir '卸载(以管理员身份运行).bat') -Force
Copy-Item -LiteralPath (Join-Path $portableSrc 'uninstall.bat') -Destination (Join-Path $tempDir '卸载.bat') -Force
Copy-Item -LiteralPath (Join-Path $portableSrc 'register_user.ps1') -Destination (Join-Path $tempDir 'register_user.ps1') -Force
Copy-Item -LiteralPath (Join-Path $portableSrc 'unregister_user.ps1') -Destination (Join-Path $tempDir 'unregister_user.ps1') -Force

# Compress to ZIP
New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null
$zipName = "CaishenPinyin-$productVersion-win-x64-Portable.zip"
$zipPath = Join-Path $outputDirectory $zipName

if (Test-Path -LiteralPath $zipPath) {
    Remove-Item -LiteralPath $zipPath -Force
}

Write-Host "Compressing to $zipPath..."
Compress-Archive -Path "$tempDir\*" -DestinationPath $zipPath -Force

# Clean up temp dir
Remove-Item -Recurse -Force -LiteralPath $tempDir

# Calculate hash
$zipHash = (Get-FileHash -LiteralPath $zipPath -Algorithm SHA256).Hash.ToLowerInvariant()
$checksumPath = "$zipPath.sha256"
[IO.File]::WriteAllText($checksumPath, "$zipHash  $zipName`r`n", [System.Text.Encoding]::UTF8)

Write-Host "[OK] Portable package created: $zipPath" -ForegroundColor Green
Write-Host "[OK] SHA-256: $zipHash" -ForegroundColor Green
