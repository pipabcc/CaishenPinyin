# Check the Caishen IME build toolchain; prefer an optional repo-local tools directory.
$ErrorActionPreference = "Continue"
$Root = Split-Path -Parent $PSScriptRoot
$ToolsRoot = Join-Path $Root "tools"
$EnvScript = Join-Path $ToolsRoot "env.ps1"

Write-Host "=== Caishen IME env check ===" -ForegroundColor Cyan
Write-Host "repo = $Root"

if (Test-Path $EnvScript) {
    Write-Host "loading $EnvScript"
    . $EnvScript
}

$ok = $true
function Check-Cmd([string]$name) {
    $c = Get-Command $name -ErrorAction SilentlyContinue
    if ($c) {
        Write-Host "[OK] $name -> $($c.Source)" -ForegroundColor Green
        return $true
    }
    Write-Host "[MISS] $name" -ForegroundColor Yellow
    return $false
}

if (-not (Check-Cmd "cmake")) { $ok = $false }
if (-not (Check-Cmd "dotnet")) { $ok = $false }

$vsInstall = $env:SHURU_VSINSTALL
if (-not $vsInstall) { $vsInstall = Join-Path $ToolsRoot "vs2022" }

$cl = Get-Command cl -ErrorAction SilentlyContinue
$vcvars = Join-Path $vsInstall "VC\Auxiliary\Build\vcvars64.bat"
if ($cl) {
    Write-Host "[OK] MSVC cl -> $($cl.Source)" -ForegroundColor Green
} elseif (Test-Path $vcvars) {
    Write-Host "[OK] VS vcvars64 present: $vcvars" -ForegroundColor Green
    Write-Host "     (run tools\\env.ps1 to put cl on PATH)" -ForegroundColor DarkYellow
} else {
    # fallback vswhere
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $install = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if ($install) {
            Write-Host "[OK] VS C++ tools: $install" -ForegroundColor Green
        } else {
            Write-Host "[MISS] Visual Studio C++ toolset" -ForegroundColor Yellow
            $ok = $false
        }
    } else {
        Write-Host "[MISS] MSVC toolset under $vsInstall" -ForegroundColor Yellow
        $ok = $false
    }
}

$sdkCandidates = @(
    (Join-Path $ToolsRoot "winsdk\Windows Kits\10\Include"),
    "C:\Program Files (x86)\Windows Kits\10\Include"
)
$sdkOk = $false
foreach ($p in $sdkCandidates) {
    if (Test-Path -LiteralPath $p) {
        $versions = Get-ChildItem -LiteralPath $p -Directory -ErrorAction SilentlyContinue
        if ($versions) {
            Write-Host "[OK] Windows SDK Include: $p ($(($versions | Select-Object -ExpandProperty Name) -join ', '))" -ForegroundColor Green
            $sdkOk = $true
            break
        }
    }
}
if (-not $sdkOk) {
    Write-Host "[MISS] Windows SDK C++ headers" -ForegroundColor Yellow
    $ok = $false
}

if ($ok) {
    Write-Host "Environment ready. Run scripts/build.ps1" -ForegroundColor Green
    exit 0
}

Write-Host "Environment incomplete. Need: VS 2022 Build Tools + MSVC + Windows SDK + CMake + .NET 8." -ForegroundColor Red
exit 1
