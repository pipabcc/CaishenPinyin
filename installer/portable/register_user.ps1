[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$tip = '0804:{7C4E9F2A-1B3D-4A8E-9F6C-2D5E8B1A4C7F}{3A8B5C2E-9D1F-4E6A-B7C8-5D2E9F1A3B6C}'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$settings = Join-Path $root 'ShuruSettings.exe'
$runKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
$runName = 'CaishenSettings'
$shortcutName = -join @(
    [char]0x8D22, [char]0x795E, [char]0x8F93, [char]0x5165, [char]0x6CD5,
    [char]0x8BBE, [char]0x7F6E)
$shortcutDescription = $shortcutName + (-join @([char]0x4E2D, [char]0x5FC3))
$runChanged = $false
$runPreviouslyPresent = $false
$runPreviousValue = ''
$shortcutPath = ''
$shortcutBackup = ''
$shortcutTemporary = ''
$shortcutWritten = $false
$languageTipAdded = $false
$languageAdded = $false

function Release-ComObject($Value) {
    if ($null -ne $Value -and [Runtime.InteropServices.Marshal]::IsComObject($Value)) {
        [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($Value)
    }
}

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
    $shortcutPath = Join-Path $desktop ($shortcutName + '.lnk')
    if (Test-Path -LiteralPath $shortcutPath -PathType Leaf) {
        $shortcutBackup = "$shortcutPath.backup-$PID"
        Move-Item -LiteralPath $shortcutPath -Destination $shortcutBackup -Force -ErrorAction Stop
    }
    $shortcutTemporary = Join-Path $desktop (
        '.CaishenSettings-{0}.lnk' -f [guid]::NewGuid().ToString('N'))
    $shell = $null
    $shortcut = $null
    try {
        $shell = New-Object -ComObject WScript.Shell
        $shortcut = $shell.CreateShortcut($shortcutTemporary)
        $shortcut.TargetPath = $settings
        $shortcut.WorkingDirectory = $root
        $shortcut.Description = $shortcutDescription
        $shortcut.Save()
    } finally {
        Release-ComObject $shortcut
        Release-ComObject $shell
    }
    Move-Item -LiteralPath $shortcutTemporary -Destination $shortcutPath `
        -Force -ErrorAction Stop
    $shortcutTemporary = ''
    $shortcutWritten = $true

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
    if ($shortcutTemporary) {
        Remove-Item -LiteralPath $shortcutTemporary -Force -ErrorAction SilentlyContinue
    }
    Restore-UserState
    Write-Error "Portable per-user configuration failed: $failure"
    exit 1
}

exit 0
