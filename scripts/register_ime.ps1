# 注册发财拼音输入法（建议以管理员身份运行）
param(
    [string]$DllPath = ""
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot

function Resolve-TargetDll {
    param([string]$RequestedPath)

    if (-not [string]::IsNullOrWhiteSpace($RequestedPath)) {
        if (-not (Test-Path -LiteralPath $RequestedPath -PathType Leaf)) {
            throw "找不到指定 DLL: $RequestedPath"
        }
        return (Resolve-Path -LiteralPath $RequestedPath).Path
    }

    $candidates = @()
    $distDir = Join-Path $Root "dist"
    if (Test-Path -LiteralPath $distDir -PathType Container) {
        $candidates += Get-ChildItem -LiteralPath $distDir -Filter "ShuruIme*.dll" -File |
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

    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }
    throw "找不到 ShuruIme.dll，请先完成构建。"
}

try {
    $DllPath = Resolve-TargetDll -RequestedPath $DllPath
    Write-Host "注册 DLL: $DllPath"

    # 这里只负责显式 DLL 注册。版本化词库和 side-by-side 部署请使用 install_ime.ps1；
    # 不再向 DLL 目录复制系统词库或 user_dict.txt，避免升级覆盖用户数据。
    $regsvr = Join-Path $env:SystemRoot "System32\regsvr32.exe"
    & $regsvr /s $DllPath
    if ($LASTEXITCODE -ne 0) {
        throw "regsvr32 注册失败，退出码: $LASTEXITCODE。请确认已使用管理员权限。"
    }

    # 将 TIP 加入当前用户的中文语言列表；语言/profile 激活也是注册事务的一部分。
    $tip = '0804:{7C4E9F2A-1B3D-4A8E-9F6C-2D5E8B1A4C7F}{3A8B5C2E-9D1F-4E6A-B7C8-5D2E9F1A3B6C}'
    try {
        $langs = Get-WinUserLanguageList
        $zh = $langs | Where-Object { $_.LanguageTag -like 'zh-Hans*' } | Select-Object -First 1
        if ($zh) {
            # 已存在的 TIP 也先移除再添加，强制刷新 Windows 输入切换器缓存的旧名称。
            if ($zh.InputMethodTips -contains $tip) {
                [void]$zh.InputMethodTips.Remove($tip)
            }
            [void]$zh.InputMethodTips.Add($tip)
            Set-WinUserLanguageList -LanguageList $langs -Force
        }
        Set-WinDefaultInputMethodOverride -InputTip $tip
        Write-Host "已设置默认输入法: Facai Pinyin / 发财拼音"
    } catch {
        & $regsvr /u /s $DllPath
        throw "语言列表/profile 激活失败，已撤销 DLL 注册: $($_.Exception.Message)"
    }

    $health = Join-Path (Split-Path $DllPath) 'release_health_check.exe'
    if (Test-Path -LiteralPath $health) {
        & $health $DllPath (Join-Path $Root 'data\lexicon') --registered
        if ($LASTEXITCODE -ne 0) {
            & $regsvr /u /s $DllPath
            throw "注册后健康检查失败，已撤销 DLL 注册"
        }
    }

    Write-Host "注册完成，可使用 Win+Space 切换输入法。" -ForegroundColor Green
    exit 0
} catch {
    Write-Error $_.Exception.Message
    exit 1
}
