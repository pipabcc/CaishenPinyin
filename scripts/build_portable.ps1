[CmdletBinding()]
param(
    [string]$PackageDir = 'artifacts\release',
    [string]$OutputDir = 'artifacts\installer',
    [ValidateSet('Off', 'IfPresent', 'Required')]
    [string]$SigningPolicy = 'Off'
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
& (Join-Path $PSScriptRoot 'test_release_package.ps1') `
    -PackageRoot $packageRoot -ProductVersion $productVersion `
    -ExpectedSigningPolicy $SigningPolicy

$tempDir = Join-Path ([IO.Path]::GetTempPath()) `
    ('caishen-portable-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $tempDir | Out-Null
try {
    Copy-Item -Path "$packageRoot\*" -Destination $tempDir -Recurse -Force

    $portableSrc = Join-Path $RepositoryRoot 'installer\portable'
    $installLabel = -join @([char]0x5B89, [char]0x88C5)
    $uninstallLabel = -join @([char]0x5378, [char]0x8F7D)
    $asAdministrator = -join @(
        [char]0x4EE5, [char]0x7BA1, [char]0x7406, [char]0x5458,
        [char]0x8EAB, [char]0x4EFD, [char]0x8FD0, [char]0x884C)
    $portableReadme = -join @(
        [char]0x4FBF, [char]0x643A, [char]0x7248, [char]0x4F7F,
        [char]0x7528, [char]0x8BF4, [char]0x660E)
    Copy-Item -LiteralPath (Join-Path $portableSrc 'install.bat') `
        -Destination (Join-Path $tempDir "$installLabel($asAdministrator).bat") -Force
    Copy-Item -LiteralPath (Join-Path $portableSrc 'install.bat') `
        -Destination (Join-Path $tempDir "$installLabel.bat") -Force
    Copy-Item -LiteralPath (Join-Path $portableSrc 'uninstall.bat') `
        -Destination (Join-Path $tempDir "$uninstallLabel($asAdministrator).bat") -Force
    Copy-Item -LiteralPath (Join-Path $portableSrc 'uninstall.bat') `
        -Destination (Join-Path $tempDir "$uninstallLabel.bat") -Force
    Copy-Item -LiteralPath (Join-Path $portableSrc 'register_user.ps1') `
        -Destination (Join-Path $tempDir 'register_user.ps1') -Force
    Copy-Item -LiteralPath (Join-Path $portableSrc 'unregister_user.ps1') `
        -Destination (Join-Path $tempDir 'unregister_user.ps1') -Force
    Copy-Item -LiteralPath (Join-Path $portableSrc 'README.txt') `
        -Destination (Join-Path $tempDir "$portableReadme.txt") -Force

    New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null
    $zipName = "CaishenPinyin-$productVersion-win-x64-Portable.zip"
    $zipPath = Join-Path $outputDirectory $zipName
    if (Test-Path -LiteralPath $zipPath) {
        Remove-Item -LiteralPath $zipPath -Force
    }

    Write-Host "Compressing to $zipPath..."
    Compress-Archive -Path "$tempDir\*" -DestinationPath $zipPath -Force
} finally {
    if (Test-Path -LiteralPath $tempDir) {
        Remove-Item -Recurse -Force -LiteralPath $tempDir
    }
}

# Calculate hash
$zipHash = (Get-FileHash -LiteralPath $zipPath -Algorithm SHA256).Hash.ToLowerInvariant()
$checksumPath = "$zipPath.sha256"
[IO.File]::WriteAllText(
    $checksumPath,
    "$zipHash  $zipName`r`n",
    (New-Object Text.UTF8Encoding($false)))

Write-Host "[OK] Portable package created: $zipPath" -ForegroundColor Green
Write-Host "[OK] SHA-256: $zipHash" -ForegroundColor Green
