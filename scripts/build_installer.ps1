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

function Assert-ReleasePackage([string]$PackageRoot, [string]$ProductVersion) {
    $manifestPath = Join-Path $PackageRoot 'release-manifest.json'
    if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
        throw "release manifest missing: $manifestPath"
    }
    try { $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json }
    catch { throw "release manifest invalid: $($_.Exception.Message)" }
    if ([string]$manifest.schemaVersion -ne '2') { throw 'release manifest schema must be 2' }
    if ([string]$manifest.version -ne $ProductVersion) {
        throw "release manifest version $($manifest.version) does not match $ProductVersion"
    }
    if ([string]$manifest.signingPolicy -ne 'Off') {
        throw "release manifest signingPolicy must be Off, actual=$($manifest.signingPolicy)"
    }

    $manifestPaths = @{}
    foreach ($entry in $manifest.files) {
        $relativePath = ([string]$entry.path).Replace('/', '\')
        if (-not $relativePath -or [IO.Path]::IsPathRooted($relativePath) -or
            $relativePath.Split('\') -contains '..') {
            throw "unsafe release manifest path: $relativePath"
        }
        $manifestPaths[$relativePath.ToLowerInvariant()] = $true
        $filePath = Join-Path $PackageRoot $relativePath
        if (-not (Test-Path -LiteralPath $filePath -PathType Leaf)) {
            throw "release file missing: $relativePath"
        }
        $file = Get-Item -LiteralPath $filePath
        if ($file.Length -ne [int64]$entry.size) {
            throw "release size mismatch: $relativePath"
        }
        $hash = (Get-FileHash -LiteralPath $filePath -Algorithm SHA256).Hash
        if ($hash.ToLowerInvariant() -ne ([string]$entry.sha256).ToLowerInvariant()) {
            throw "release hash mismatch: $relativePath"
        }
    }

    foreach ($file in Get-ChildItem -LiteralPath $PackageRoot -Recurse -File) {
        $relativePath = $file.FullName.Substring($PackageRoot.Length).TrimStart('\')
        if ($relativePath -eq 'release-manifest.json') { continue }
        if (-not $manifestPaths.ContainsKey($relativePath.ToLowerInvariant())) {
            throw "release file is not covered by manifest: $relativePath"
        }
    }

    foreach ($required in @(
        'ShuruIme.dll',
        'ShuruSettings.exe',
        'ShuruSettings.dll',
        'ShuruSettings.runtimeconfig.json',
        'coreclr.dll',
        'hostfxr.dll',
        'PresentationFramework.dll',
        'licenses\dotnet-LICENSE.txt',
        'licenses\dotnet-ThirdPartyNotices.txt')) {
        if (-not (Test-Path -LiteralPath (Join-Path $PackageRoot $required) -PathType Leaf)) {
            throw "required self-contained release file missing: $required"
        }
    }

    foreach ($forbidden in @('rime-moqi-zh.gram', 'zh-moqi.gram', 'user_dict.txt')) {
        if (Get-ChildItem -LiteralPath $PackageRoot -Recurse -File -Filter $forbidden) {
            throw "forbidden user-managed file found in installer payload: $forbidden"
        }
    }

    $runtimeConfig = Get-Content -LiteralPath `
        (Join-Path $PackageRoot 'ShuruSettings.runtimeconfig.json') -Raw | ConvertFrom-Json
    $frameworkNames = @($runtimeConfig.runtimeOptions.includedFrameworks | ForEach-Object {
        [string]$_.name
    })
    if ($frameworkNames -notcontains 'Microsoft.WindowsDesktop.App') {
        throw 'settings application is not a self-contained Windows desktop publish'
    }
    return $manifest
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
[void](Assert-ReleasePackage $packageRoot $productVersion)

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
