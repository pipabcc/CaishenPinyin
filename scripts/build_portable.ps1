[CmdletBinding()]
param(
    [string]$PackageDir = 'artifacts\release',
    [string]$OutputDir = 'artifacts\installer'
)

$ErrorActionPreference = 'Stop'
$RepositoryRoot = Split-Path -Parent $PSScriptRoot

function Read-ProductVersion {
    $versionHeader = Get-Content -LiteralPath `
        (Join-Path $RepositoryRoot 'src\common\version.h') -Raw
    if ($versionHeader -notmatch '#define\s+SHURU_VERSION_STRING\s+"([^"]+)"') {
        throw 'version string missing from src/common/version.h'
    }
    return $Matches[1]
}

$productVersion = Read-ProductVersion
$packageRoot = Join-Path $RepositoryRoot $PackageDir
$outputDirectory = Join-Path $RepositoryRoot $OutputDir

if (-not (Test-Path -LiteralPath $packageRoot -PathType Container)) {
    throw "Release package directory missing: $packageRoot. Please build the project first."
}

# Create temp dir for staging
$tempDir = Join-Path $RepositoryRoot 'portable_temp'
if (Test-Path -LiteralPath $tempDir) {
    Remove-Item -Recurse -Force -LiteralPath $tempDir
}
New-Item -ItemType Directory -Force -Path $tempDir | Out-Null

# Copy release files to temp dir
Copy-Item -Path "$packageRoot\*" -Destination $tempDir -Recurse -Force

# Create 安装(以管理员身份运行).bat
$installBatPath = Join-Path $tempDir '安装(以管理员身份运行).bat'
$installBatContent = @'
@echo off
chcp 65001 >nul
:: Check for admin rights
net session >nul 2>&1
if %errorLevel% neq 0 (
    echo 请右键选择“以管理员身份运行”此脚本。
    pause
    exit /b
)

echo 正在安装 财神输入法 便携版...

:: Register IME DLL
regsvr32.exe /s "%~dp0ShuruIme.dll"
if %errorLevel% neq 0 (
    echo 注册 ShuruIme.dll 失败！
    pause
    exit /b
)
echo [OK] 注册输入法组件成功。

:: Write registry key for startup (ShuruSettings)
reg add "HKLM\Software\Microsoft\Windows\CurrentVersion\Run" /v "CaishenSettings" /t REG_SZ /d "\"%~dp0ShuruSettings.exe\" --minimized" /f >nul 2>&1
if %errorLevel% neq 0 (
    :: Fallback to HKCU if HKLM fails
    reg add "HKCU\Software\Microsoft\Windows\CurrentVersion\Run" /v "CaishenSettings" /t REG_SZ /d "\"%~dp0ShuruSettings.exe\" --minimized" /f >nul 2>&1
)
echo [OK] 已写入开机自启。

:: Create desktop shortcut to ShuruSettings.exe
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
    "$WshShell = New-Object -ComObject WScript.Shell; ^
     $Shortcut = $WshShell.CreateShortcut([System.IO.Path]::Combine([Environment]::GetFolderPath('Desktop'), '财神输入法设置.lnk')); ^
     $Shortcut.TargetPath = '%~dp0ShuruSettings.exe'; ^
     $Shortcut.WorkingDirectory = '%~dp0'; ^
     $Shortcut.Description = '财神输入法设置中心'; ^
     $Shortcut.Save()"
echo [OK] 已创建桌面快捷方式。

echo 财神输入法便携版安装完成！请按 Win+Space 切换并体验。
pause
'@

# Save as UTF-8 with BOM so Chinese characters show correctly in PowerShell/CMD with chcp 65001
[System.IO.File]::WriteAllText($installBatPath, $installBatContent, [System.Text.Encoding]::UTF8)

# Create 卸载(以管理员身份运行).bat
$uninstallBatPath = Join-Path $tempDir '卸载(以管理员身份运行).bat'
$uninstallBatContent = @'
@echo off
chcp 65001 >nul
:: Check for admin rights
net session >nul 2>&1
if %errorLevel% neq 0 (
    echo 请右键选择“以管理员身份运行”此脚本。
    pause
    exit /b
)

echo 正在卸载 财神输入法 便携版...

:: Unregister IME DLL
regsvr32.exe /u /s "%~dp0ShuruIme.dll"
echo [OK] 已注销输入法组件。

:: Remove startup registry keys
reg delete "HKLM\Software\Microsoft\Windows\CurrentVersion\Run" /v "CaishenSettings" /f >nul 2>&1
reg delete "HKCU\Software\Microsoft\Windows\CurrentVersion\Run" /v "CaishenSettings" /f >nul 2>&1
echo [OK] 已移除开机自启。

:: Delete desktop shortcut
del /f /q "%USERPROFILE%\Desktop\财神输入法设置.lnk" >nul 2>&1
echo [OK] 已删除桌面快捷方式。

echo 财神输入法便携版卸载完成！
pause
'@

[System.IO.File]::WriteAllText($uninstallBatPath, $uninstallBatContent, [System.Text.Encoding]::UTF8)

# Compress to ZIP
New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null
$zipName = "CaishenPinyin-$productVersion-win-x64-Portable.zip"
$zipPath = Join-Path $outputDirectory $zipName

if (Test-Path -LiteralPath $zipPath) {
    Remove-Item -LiteralPath $zipPath -Force
}

Write-Host "Compressing to $zipPath..."
Compress-Archive -Path "$tempDir\*" -DestinationPath $zipPath -Force

# Clean up temp dir
Remove-Item -Recurse -Force -LiteralPath $tempDir

# Calculate hash
$zipHash = (Get-FileHash -LiteralPath $zipPath -Algorithm SHA256).Hash.ToLowerInvariant()
$checksumPath = "$zipPath.sha256"
[IO.File]::WriteAllText($checksumPath, "$zipHash  $zipName`r`n", [System.Text.Encoding]::UTF8)

Write-Host "[OK] Portable package created: $zipPath" -ForegroundColor Green
Write-Host "[OK] SHA-256: $zipHash" -ForegroundColor Green
