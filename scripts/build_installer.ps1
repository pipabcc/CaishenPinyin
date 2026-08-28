[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Config = 'Release',
    [string]$BuildDir = 'build-release',
    [string]$PackageDir = 'artifacts\release',
    [string]$InstallerOutputDir = 'artifacts\installer',
    [string]$MakeNsisPath = '',
    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'
$RepositoryRoot = Split-Path -Parent $PSScriptRoot

function Resolve-RepositoryPath([string]$Path) {
    if ([IO.Path]::IsPathRooted($Path)) { return [IO.Path]::GetFullPath($Path) }
    return [IO.Path]::GetFullPath((Join-Path $RepositoryRoot $Path))
}

function Read-ProductVersion {
    $versionHeader = Get-Content -LiteralPath `
        (Join-Path $RepositoryRoot 'src\common\version.h') -Raw
    if ($versionHeader -notmatch '#define\s+SHURU_VERSION_STRING\s+"([^"]+)"') {
        throw 'version string missing from src/common/version.h'
    }
    return $Matches[1]
}

if (-not $SkipBuild) {
    & (Join-Path $PSScriptRoot 'build.ps1') -Config $Config -BuildDir $BuildDir `
        -OutputDir $PackageDir -SigningPolicy Off
    if ($LASTEXITCODE -ne 0) { throw "release build failed: $LASTEXITCODE" }
}

$productVersion = Read-ProductVersion
$versionParts = $productVersion.Split('.')
$invalidVersionParts = @($versionParts | Where-Object { $_ -notmatch '^\d+$' })
if ($versionParts.Count -ne 3 -or $invalidVersionParts.Count -ne 0) {
    throw "NSIS requires a numeric major.minor.patch version, actual=$productVersion"
}
$numericVersion = '{0}.{1}.{2}.0' -f $versionParts[0], $versionParts[1], $versionParts[2]
$packageRoot = Resolve-RepositoryPath $PackageDir
if (-not (Test-Path -LiteralPath $packageRoot -PathType Container)) {
    throw "release package directory missing: $packageRoot"
}
& (Join-Path $PSScriptRoot 'test_release_package.ps1') `
    -PackageRoot $packageRoot -ProductVersion $productVersion -ExpectedSigningPolicy Off

if (-not $MakeNsisPath) {
    $MakeNsisPath = Join-Path ${env:ProgramFiles(x86)} 'NSIS\makensis.exe'
}
if (-not (Test-Path -LiteralPath $MakeNsisPath -PathType Leaf)) {
    throw "makensis.exe not found: $MakeNsisPath"
}
$MakeNsisPath = (Resolve-Path -LiteralPath $MakeNsisPath).Path

$outputDirectory = Resolve-RepositoryPath $InstallerOutputDir
New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null
$setupName = "CaishenPinyin-$productVersion-win-x64-Setup.exe"
$setupPath = Join-Path $outputDirectory $setupName
$estimatedSizeKb = [int64][Math]::Ceiling((
    (Get-ChildItem -LiteralPath $packageRoot -Recurse -File |
        Measure-Object -Property Length -Sum).Sum) / 1KB)

$nsiPath = Join-Path $RepositoryRoot 'installer\CaishenPinyin.nsi'
$deployScript = Join-Path $RepositoryRoot 'scripts\install_ime.ps1'
$appIcon = Join-Path $RepositoryRoot 'settings\app.ico'
$arguments = @(
    '/V4',
    "/DPRODUCT_VERSION=$productVersion",
    "/DPRODUCT_VERSION_NUMERIC=$numericVersion",
    "/DPAYLOAD_DIR=$packageRoot",
    "/DDEPLOY_SCRIPT=$deployScript",
    "/DAPP_ICON=$appIcon",
    "/DOUTPUT_FILE=$setupPath",
    "/DESTIMATED_SIZE_KB=$estimatedSizeKb",
    $nsiPath
)
& $MakeNsisPath @arguments
if ($LASTEXITCODE -ne 0) { throw "NSIS build failed: $LASTEXITCODE" }
if (-not (Test-Path -LiteralPath $setupPath -PathType Leaf)) {
    throw "NSIS output missing: $setupPath"
}

$setupVersionInfo = (Get-Item -LiteralPath $setupPath).VersionInfo
$expectedProductName = -join @(
    [char]0x8D22, [char]0x795E, [char]0x8F93, [char]0x5165, [char]0x6CD5)
$expectedFileDescription = $expectedProductName + ' ' + (-join @(
    [char]0x5B89, [char]0x88C5, [char]0x7A0B, [char]0x5E8F))
if ($setupVersionInfo.ProductName -ne $expectedProductName -or
    $setupVersionInfo.FileDescription -ne $expectedFileDescription) {
    throw "installer product metadata is invalid: product=$($setupVersionInfo.ProductName) description=$($setupVersionInfo.FileDescription)"
}

$signature = Get-AuthenticodeSignature -LiteralPath $setupPath
if ($signature.Status -ne 'NotSigned') {
    throw "unsigned installer expected, signature status=$($signature.Status)"
}
$setupHash = (Get-FileHash -LiteralPath $setupPath -Algorithm SHA256).Hash.ToLowerInvariant()
$checksumPath = "$setupPath.sha256"
[IO.File]::WriteAllText(
    $checksumPath,
    "$setupHash  $setupName`r`n",
    (New-Object Text.UTF8Encoding($false)))

Write-Host "[OK] unsigned NSIS installer: $setupPath" -ForegroundColor Green
Write-Host "[OK] SHA-256: $setupHash" -ForegroundColor Green
