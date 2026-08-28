# 激活仓库 tools 目录中的可选便携工具链。
# 用法: . tools\env.ps1

$ErrorActionPreference = 'Stop'

if ($PSScriptRoot) {
    $ToolsRoot = $PSScriptRoot
} elseif ($MyInvocation.MyCommand.Path) {
    $ToolsRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
} else {
    throw 'Unable to determine the repository-local tools directory.'
}

$RepoRoot = Split-Path -Parent $ToolsRoot
$CMakeBin = Join-Path $ToolsRoot 'cmake\cmake-3.31.6-windows-x86_64\bin'
$NinjaBin = Join-Path $ToolsRoot 'ninja'
$VsInstall = Join-Path $ToolsRoot 'vs2022'
$DotnetLocal = Join-Path $ToolsRoot 'dotnet'
$TmpDir = Join-Path $ToolsRoot 'tmp'
$WinsdkRoot = Join-Path $ToolsRoot 'winsdk\Windows Kits\10'

New-Item -ItemType Directory -Force -Path $TmpDir | Out-Null
$env:TEMP = $TmpDir
$env:TMP = $TmpDir
$env:SHURU_TOOLS_ROOT = $ToolsRoot
$env:SHURU_REPO_ROOT = $RepoRoot
$env:SHURU_VSINSTALL = $VsInstall

$pathParts = @()
if (Test-Path -LiteralPath $CMakeBin) { $pathParts += $CMakeBin }
if (Test-Path -LiteralPath $NinjaBin) { $pathParts += $NinjaBin }
if (Test-Path -LiteralPath (Join-Path $DotnetLocal 'dotnet.exe')) {
    $pathParts += $DotnetLocal
    $env:DOTNET_ROOT = $DotnetLocal
}

$vsDevShell = Join-Path $VsInstall 'Common7\Tools\Launch-VsDevShell.ps1'
$vcvars = Join-Path $VsInstall 'VC\Auxiliary\Build\vcvars64.bat'

$loadedVc = $false
if (Test-Path -LiteralPath $vsDevShell) {
    try {
        & $vsDevShell -Arch amd64 -HostArch amd64 -SkipAutomaticLocation | Out-Null
        $loadedVc = $true
    } catch {
        $loadedVc = $false
    }
}

if (-not $loadedVc -and (Test-Path -LiteralPath $vcvars)) {
    $psi = "call `"$vcvars`" >nul && set"
    cmd.exe /c $psi | ForEach-Object {
        if ($_ -match '^(.*?)=(.*)$') {
            $name = $Matches[1]
            $value = $Matches[2]
            if ($name -match '^(PATH|INCLUDE|LIB|LIBPATH|WindowsSdkDir|WindowsSDKVersion|WindowsSDKLibVersion|UniversalCRTSdkDir|UCRTVersion|VCToolsInstallDir|VCToolsVersion|VSCMD_ARG_TGT_ARCH|VSCMD_ARG_HOST_ARCH)$') {
                Set-Item -Path "Env:$name" -Value $value
            }
        }
    }
}

if (Test-Path -LiteralPath $WinsdkRoot) {
    $sdkVerDir = Get-ChildItem -LiteralPath (Join-Path $WinsdkRoot 'Include') -Directory -ErrorAction SilentlyContinue |
        Sort-Object Name -Descending |
        Select-Object -First 1
    if ($sdkVerDir) {
        $ver = $sdkVerDir.Name
        $env:WindowsSdkDir = $(if ($WinsdkRoot.EndsWith('\')) { $WinsdkRoot } else { "$WinsdkRoot\" })
        $env:WindowsSDKVersion = "$ver\"
        $includeExtra = @(
            (Join-Path $WinsdkRoot "Include\$ver\ucrt"),
            (Join-Path $WinsdkRoot "Include\$ver\um"),
            (Join-Path $WinsdkRoot "Include\$ver\shared"),
            (Join-Path $WinsdkRoot "Include\$ver\winrt")
        ) -join ';'
        $libExtra = @(
            (Join-Path $WinsdkRoot "Lib\$ver\ucrt\x64"),
            (Join-Path $WinsdkRoot "Lib\$ver\um\x64")
        ) -join ';'
        if ($env:INCLUDE) { $env:INCLUDE = "$includeExtra;$env:INCLUDE" } else { $env:INCLUDE = $includeExtra }
        if ($env:LIB) { $env:LIB = "$libExtra;$env:LIB" } else { $env:LIB = $libExtra }
    }
}

$prepend = ($pathParts -join ';')
if ($prepend) {
    $env:PATH = "$prepend;$env:PATH"
}

function Write-Tool([string]$name) {
    $c = Get-Command $name -ErrorAction SilentlyContinue
    if ($c) { return $c.Source }
    return '(missing)'
}

Write-Host "Shuru tools activated:" -ForegroundColor Cyan
Write-Host "  repo   = $RepoRoot"
Write-Host "  tools  = $ToolsRoot"
Write-Host "  cmake  = $(Write-Tool cmake)"
Write-Host "  ninja  = $(Write-Tool ninja)"
Write-Host "  cl     = $(Write-Tool cl)"
Write-Host "  TEMP   = $env:TEMP"
