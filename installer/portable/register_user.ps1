[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$tip = '0804:{7C4E9F2A-1B3D-4A8E-9F6C-2D5E8B1A4C7F}{3A8B5C2E-9D1F-4E6A-B7C8-5D2E9F1A3B6C}'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$settings = Join-Path $root 'ShuruSettings.exe'
$runKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
$runName = 'CaishenSettings'
$runChanged = $false
$runPreviouslyPresent = $false
$runPreviousValue = ''
$shortcutPath = ''
$shortcutBackup = ''
$shortcutWritten = $false
$languageTipAdded = $false
$languageAdded = $false

function Restore-UserState {
    if ($languageTipAdded) {
        try {
            $languages = Get-WinUserLanguageList -ErrorAction Stop
            $language = $languages |
                Where-Object { $_.LanguageTag -like 'zh-Hans*' } |
                Select-Object -First 1
            if ($language -and $language.InputMethodTips -contains $tip) {
                [void]$language.InputMethodTips.Remove($tip)
                if ($languageAdded -and $language.InputMethodTips.Count -eq 0) {
                    [void]$languages.Remove($language)
                }
                Set-WinUserLanguageList -LanguageList $languages -Force -ErrorAction Stop
            }
        } catch {
            Write-Warning "Unable to roll back the user language list: $($_.Exception.Message)"
        }
    }

    if ($shortcutWritten -and $shortcutPath) {
        Remove-Item -LiteralPath $shortcutPath -Force -ErrorAction SilentlyContinue
    }
    if ($shortcutBackup -and (Test-Path -LiteralPath $shortcutBackup -PathType Leaf)) {
        try {
            Move-Item -LiteralPath $shortcutBackup -Destination $shortcutPath -Force -ErrorAction Stop
        } catch {
            Write-Warning "Unable to restore the previous desktop shortcut: $($_.Exception.Message)"
        }
    }

    if ($runChanged) {
        try {
            if ($runPreviouslyPresent) {
                Set-ItemProperty -LiteralPath $runKey -Name $runName `
                    -Value $runPreviousValue -ErrorAction Stop
            } else {
                Remove-ItemProperty -LiteralPath $runKey -Name $runName `
                    -ErrorAction SilentlyContinue
            }
        } catch {
            Write-Warning "Unable to restore the previous startup entry: $($_.Exception.Message)"
        }
    }
}

try {
    if (-not (Test-Path -LiteralPath $settings -PathType Leaf)) {
        throw "ShuruSettings.exe not found: $settings"
    }

    if (-not (Test-Path -LiteralPath $runKey -PathType Container)) {
        New-Item -Path $runKey -Force -ErrorAction Stop | Out-Null
    }
    $runProperties = Get-ItemProperty -LiteralPath $runKey -ErrorAction Stop
    $runProperty = $runProperties.PSObject.Properties[$runName]
    if ($null -ne $runProperty) {
        $runPreviouslyPresent = $true
        $runPreviousValue = [string]$runProperty.Value
    }
    Set-ItemProperty -LiteralPath $runKey -Name $runName `
        -Value "`"$settings`" --minimized" -ErrorAction Stop
    $runChanged = $true

    $desktop = [Environment]::GetFolderPath('Desktop')
    if (-not $desktop) { throw 'Desktop directory is unavailable.' }
    $shortcutPath = Join-Path $desktop '财神输入法设置.lnk'
    if (Test-Path -LiteralPath $shortcutPath -PathType Leaf) {
        $shortcutBackup = "$shortcutPath.backup-$PID"
        Move-Item -LiteralPath $shortcutPath -Destination $shortcutBackup -Force -ErrorAction Stop
    }
    $shell = New-Object -ComObject WScript.Shell
    $shortcut = $shell.CreateShortcut($shortcutPath)
    $shortcut.TargetPath = $settings
    $shortcut.WorkingDirectory = $root
    $shortcut.Description = '财神输入法设置中心'
    $shortcutWritten = $true
    $shortcut.Save()

    $languages = Get-WinUserLanguageList -ErrorAction Stop
    $simplifiedChinese = $languages |
        Where-Object { $_.LanguageTag -like 'zh-Hans*' } |
        Select-Object -First 1
    if (-not $simplifiedChinese) {
        $simplifiedChinese = (New-WinUserLanguageList 'zh-Hans-CN')[0]
        $simplifiedChinese.InputMethodTips.Clear()
        [void]$languages.Add($simplifiedChinese)
        $languageAdded = $true
    }
    if ($simplifiedChinese.InputMethodTips -notcontains $tip) {
        [void]$simplifiedChinese.InputMethodTips.Add($tip)
        $languageTipAdded = $true
    }
    if ($languageTipAdded) {
        Set-WinUserLanguageList -LanguageList $languages -Force -ErrorAction Stop
    }

    if ($shortcutBackup) {
        Remove-Item -LiteralPath $shortcutBackup -Force -ErrorAction SilentlyContinue
    }
} catch {
    $failure = $_.Exception.Message
    Restore-UserState
    Write-Error "Portable per-user configuration failed: $failure"
    exit 1
}

exit 0
