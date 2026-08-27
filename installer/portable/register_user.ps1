$ErrorActionPreference = "SilentlyContinue"
$tip = "0804:{7C4E9F2A-1B3D-4A8E-9F6C-2D5E8B1A4C7F}{3A8B5C2E-9D1F-4E6A-B7C8-5D2E9F1A3B6C}"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path

try {
    $langs = Get-WinUserLanguageList
    $zh = $langs | Where-Object { $_.LanguageTag -like "zh-Hans*" } | Select-Object -First 1
    if ($zh) {
        if (-not ($zh.InputMethodTips -contains $tip)) {
            $zh.InputMethodTips.Add($tip)
            Set-WinUserLanguageList -LanguageList $langs -Force
        }
    }
} catch {}

try {
    $exe = Join-Path $root "ShuruSettings.exe"
    Set-ItemProperty -Path "HKCU:\Software\Microsoft\Windows\CurrentVersion\Run" -Name "CaishenSettings" -Value "`"$exe`" --minimized"
} catch {}

try {
    $ws = New-Object -ComObject WScript.Shell
    $desktop = [Environment]::GetFolderPath("Desktop")
    $lnk = $ws.CreateShortcut([IO.Path]::Combine($desktop, ([System.Text.Encoding]::UTF8.GetString([System.Text.Encoding]::UTF8.GetBytes("财神输入法设置.lnk")))))
    $lnk.TargetPath = Join-Path $root "ShuruSettings.exe"
    $lnk.WorkingDirectory = $root
    $lnk.Description = "财神输入法设置中心"
    $lnk.Save()
} catch {}