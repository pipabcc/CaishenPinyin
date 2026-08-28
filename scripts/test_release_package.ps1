[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$PackageRoot,
    [Parameter(Mandatory = $true)]
    [string]$ProductVersion,
    [ValidateSet('Off', 'IfPresent', 'Required')]
    [string]$ExpectedSigningPolicy = 'Off'
)

$ErrorActionPreference = 'Stop'
$package = [IO.Path]::GetFullPath($PackageRoot)
if (-not (Test-Path -LiteralPath $package -PathType Container)) {
    throw "release package directory missing: $package"
}
$packagePrefix = $package.TrimEnd('\') + '\'

$manifestPath = Join-Path $package 'release-manifest.json'
if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
    throw "release manifest missing: $manifestPath"
}
try {
    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
} catch {
    throw "release manifest invalid: $($_.Exception.Message)"
}
if ([string]$manifest.schemaVersion -ne '2') {
    throw 'release manifest schema must be 2'
}
if ([string]$manifest.version -ne $ProductVersion) {
    throw "release manifest version $($manifest.version) does not match $ProductVersion"
}
if ([string]$manifest.signingPolicy -ne $ExpectedSigningPolicy) {
    throw "release manifest signingPolicy must be $ExpectedSigningPolicy, actual=$($manifest.signingPolicy)"
}

$manifestPaths = @{}
foreach ($entry in @($manifest.files)) {
    $relativePath = ([string]$entry.path).Replace('/', '\')
    if (-not $relativePath -or [IO.Path]::IsPathRooted($relativePath) -or
        $relativePath.Split('\') -contains '..') {
        throw "unsafe release manifest path: $relativePath"
    }
    $key = $relativePath.ToLowerInvariant()
    if ($manifestPaths.ContainsKey($key)) {
        throw "duplicate release manifest path: $relativePath"
    }
    $manifestPaths[$key] = $true
    $filePath = [IO.Path]::GetFullPath((Join-Path $package $relativePath))
    if (-not $filePath.StartsWith(
            $packagePrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "release manifest path escapes package root: $relativePath"
    }
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

foreach ($file in Get-ChildItem -LiteralPath $package -Recurse -File) {
    $relativePath = $file.FullName.Substring($package.Length).TrimStart('\')
    if ($relativePath -eq 'release-manifest.json') { continue }
    if (-not $manifestPaths.ContainsKey($relativePath.ToLowerInvariant())) {
        throw "release file is not covered by manifest: $relativePath"
    }
}

$requiredFiles = @(
    'ShuruIme.dll',
    'ShuruSettings.exe',
    'ShuruSettings.dll',
    'ShuruSettings.runtimeconfig.json',
    'engine_snapshot_build_tool.exe',
    'coreclr.dll',
    'hostfxr.dll',
    'PresentationFramework.dll',
    'data\lexicon\manifest.json',
    'THIRD_PARTY_NOTICES.md',
    'licenses\GPL-3.0.txt',
    'licenses\dotnet-LICENSE.txt',
    'licenses\dotnet-ThirdPartyNotices.txt'
)
foreach ($required in $requiredFiles) {
    if (-not (Test-Path -LiteralPath (Join-Path $package $required) -PathType Leaf)) {
        throw "required release file missing: $required"
    }
}

foreach ($forbidden in @('rime-moqi-zh.gram', 'zh-moqi.gram', 'user_dict.txt')) {
    if (Get-ChildItem -LiteralPath $package -Recurse -File -Filter $forbidden) {
        throw "forbidden user-managed file found in release package: $forbidden"
    }
}

$dllVersion = (Get-Item -LiteralPath (Join-Path $package 'ShuruIme.dll')).VersionInfo.FileVersion
if ($dllVersion -ne $ProductVersion) {
    throw "ShuruIme.dll version $dllVersion does not match $ProductVersion"
}
$settingsVersion = (Get-Item -LiteralPath (Join-Path $package 'ShuruSettings.dll')).VersionInfo.FileVersion
if (([Version]$settingsVersion).ToString(3) -ne $ProductVersion) {
    throw "ShuruSettings.dll version $settingsVersion does not match $ProductVersion"
}

$runtimeConfig = Get-Content -LiteralPath `
    (Join-Path $package 'ShuruSettings.runtimeconfig.json') -Raw | ConvertFrom-Json
$frameworkNames = @($runtimeConfig.runtimeOptions.includedFrameworks | ForEach-Object {
    [string]$_.name
})
if ($frameworkNames -notcontains 'Microsoft.WindowsDesktop.App') {
    throw 'settings application is not a self-contained Windows desktop publish'
}

Write-Host "[OK] release package verified: $package" -ForegroundColor Green
