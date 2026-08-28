[CmdletBinding()]
param()

$ErrorActionPreference = 'Continue'
$tip = '0804:{7C4E9F2A-1B3D-4A8E-9F6C-2D5E8B1A4C7F}{3A8B5C2E-9D1F-4E6A-B7C8-5D2E9F1A3B6C}'
$hadFailure = $false
$shortcutName = -join @(
    [char]0x8D22, [char]0x795E, [char]0x8F93, [char]0x5165, [char]0x6CD5,
    [char]0x8BBE, [char]0x7F6E)

Stop-Process -Name 'ShuruSettings' -Force -ErrorAction SilentlyContinue

try {
    $languages = Get-WinUserLanguageList -ErrorAction Stop
    $changed = $false
    foreach ($language in $languages) {
        if ($language.InputMethodTips -contains $tip) {
            [void]$language.InputMethodTips.Remove($tip)
            $changed = $true
        }
    }
    if ($changed) {
        Set-WinUserLanguageList -LanguageList $languages -Force -ErrorAction Stop
    }
} catch {
    $hadFailure = $true
    Write-Warning "Unable to remove the input method from the user language list: $($_.Exception.Message)"
}

try {
    $runKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
    if (Test-Path -LiteralPath $runKey -PathType Container) {
        $runProperties = Get-ItemProperty -LiteralPath $runKey -ErrorAction Stop
        if ($null -ne $runProperties.PSObject.Properties['CaishenSettings']) {
            Remove-ItemProperty -LiteralPath $runKey `
                -Name 'CaishenSettings' -ErrorAction Stop
        }
    }
} catch {
    $hadFailure = $true
    Write-Warning "Unable to remove the startup entry: $($_.Exception.Message)"
}

try {
    $desktop = [Environment]::GetFolderPath('Desktop')
    $shortcut = if ($desktop) {
        Join-Path $desktop ($shortcutName + '.lnk')
    } else {
        ''
    }
    if ($shortcut -and (Test-Path -LiteralPath $shortcut -PathType Leaf)) {
        Remove-Item -LiteralPath $shortcut -Force -ErrorAction Stop
    }
} catch {
    $hadFailure = $true
    Write-Warning "Unable to remove the desktop shortcut: $($_.Exception.Message)"
}

if ($hadFailure) { exit 1 }
exit 0
