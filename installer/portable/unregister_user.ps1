$ErrorActionPreference = "SilentlyContinue"
$tip = "0804:{7C4E9F2A-1B3D-4A8E-9F6C-2D5E8B1A4C7F}{3A8B5C2E-9D1F-4E6A-B7C8-5D2E9F1A3B6C}"

Stop-Process -Name "ShuruSettings" -Force -ErrorAction SilentlyContinue

try {
    $langs = Get-WinUserLanguageList
    $zh = $langs | Where-Object { $_.LanguageTag -like "zh-Hans*" } | Select-Object -First 1
    if ($zh -and ($zh.InputMethodTips -contains $tip)) {
        [void]$zh.InputMethodTips.Remove($tip)
        Set-WinUserLanguageList -LanguageList $langs -Force
    }
} catch {}

try {
    Remove-ItemProperty -Path "HKCU:\Software\Microsoft\Windows\CurrentVersion\Run" -Name "CaishenSettings" -ErrorAction SilentlyContinue
    Remove-ItemProperty -Path "HKLM:\Software\Microsoft\Windows\CurrentVersion\Run" -Name "CaishenSettings" -ErrorAction SilentlyContinue
} catch {}

try {
    $desktop = [Environment]::GetFolderPath("Desktop")
    $lnk = [IO.Path]::Combine($desktop, "财神输入法设置.lnk")
    if (Test-Path -LiteralPath $lnk) {
        Remove-Item -LiteralPath $lnk -Force
    }
} catch {}