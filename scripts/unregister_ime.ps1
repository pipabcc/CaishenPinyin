param(
    [string]$DllPath = ""
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot

if ([string]::IsNullOrWhiteSpace($DllPath)) {
    $candidates = @()
    $distDir = Join-Path $Root "dist"
    if (Test-Path $distDir) {
        $candidates += Get-ChildItem -Path $distDir -Filter "ShuruIme*.dll" -File |
            Sort-Object {
                if ($_.BaseName -match '^ShuruIme(\d+)$') { [int]$Matches[1] }
                elseif ($_.BaseName -eq 'ShuruIme') { 0 }
                else { -1 }
            } -Descending |
            ForEach-Object { $_.FullName }
    }
    $candidates += @(
        (Join-Path $Root "build\Release\ShuruIme.dll"),
        (Join-Path $Root "build\Debug\ShuruIme.dll"),
        (Join-Path $Root "build\ShuruIme.dll")
    )
    foreach ($c in $candidates) {
        if (Test-Path $c) { $DllPath = $c; break }
    }
}

if (-not (Test-Path $DllPath)) {
    Write-Host "未找到 ShuruIme.dll" -ForegroundColor Yellow
    exit 0
}

$DllPath = (Resolve-Path $DllPath).Path
$regsvr = Join-Path $env:SystemRoot "System32\regsvr32.exe"
& $regsvr /u /s $DllPath
if ($LASTEXITCODE -ne 0) {
    Write-Error "卸载注册失败，退出码: $LASTEXITCODE"
    exit $LASTEXITCODE
}
Write-Host "已卸载注册: $DllPath" -ForegroundColor Green
