param(
    [string]$DllPath = "",
    [string]$X86DllPath = ""
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
$X86DllPath = if ([string]::IsNullOrWhiteSpace($X86DllPath)) {
    Join-Path (Split-Path -Parent $DllPath) 'ShuruIme32.dll'
} else {
    $X86DllPath
}
$x86Regsvr = Join-Path $env:SystemRoot "SysWOW64\regsvr32.exe"
if (Test-Path -LiteralPath $X86DllPath -PathType Leaf) {
    $x86Unregistration = Start-Process -FilePath $x86Regsvr `
        -ArgumentList @('/u', '/s', "`"$X86DllPath`"") `
        -Wait -PassThru -WindowStyle Hidden
    if ($x86Unregistration.ExitCode -ne 0) {
        Write-Error "32 位 DLL 注销失败，退出码: $($x86Unregistration.ExitCode)"
        exit $x86Unregistration.ExitCode
    }
}
$regsvr = Join-Path $env:SystemRoot "System32\regsvr32.exe"
$unregistration = Start-Process -FilePath $regsvr `
    -ArgumentList @('/u', '/s', "`"$DllPath`"") `
    -Wait -PassThru -WindowStyle Hidden
if ($unregistration.ExitCode -ne 0) {
    Write-Error "64 位 DLL 注销失败，退出码: $($unregistration.ExitCode)"
    exit $unregistration.ExitCode
}
Write-Host "已卸载注册: x64=$DllPath；x86=$X86DllPath" -ForegroundColor Green
