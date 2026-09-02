@echo off
setlocal
title Caishen IME Portable - Uninstall

cd /d "%~dp0"

echo ========================================================
echo   [Caishen IME Portable] Uninstaller
echo ========================================================
echo.

echo Checking administrator privileges...
fltmc >nul 2>&1
if %errorlevel% neq 0 (
    echo.
    echo [ERROR] Administrator privileges are required!
    echo [INFO] Please right-click this file and select "Run as administrator".
    echo.
    pause
    exit /b 1
)

echo [1/2] Removing user shortcuts and language profiles...
if exist "%~dp0unregister_user.ps1" (
    powershell.exe -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "%~dp0unregister_user.ps1"
    if errorlevel 1 (
        echo [WARNING] Some per-user settings could not be removed. See the messages above.
    )
)
echo [OK] User profiles cleaned.

echo [2/2] Unregistering input method COM/TSF component...
if exist "%~dp0ShuruIme32.dll" (
    "%SystemRoot%\SysWOW64\regsvr32.exe" /u /s "%~dp0ShuruIme32.dll"
    if errorlevel 1 (
        echo [ERROR] x86 regsvr32 failed to unregister ShuruIme32.dll.
        pause
        exit /b 2
    )
)
if exist "%~dp0ShuruIme.dll" (
    "%SystemRoot%\System32\regsvr32.exe" /u /s "%~dp0ShuruIme.dll"
    if errorlevel 1 (
        echo [ERROR] regsvr32 failed to unregister ShuruIme.dll.
        pause
        exit /b 2
    )
)
echo [OK] COM/TSF component unregistered successfully.

echo.
echo ========================================================
echo   [SUCCESS] Caishen IME Portable uninstalled successfully!
echo ========================================================
echo.
pause
exit /b 0
