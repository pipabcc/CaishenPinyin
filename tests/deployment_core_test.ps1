$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$temporaryRoot = Join-Path $env:TEMP ('facai-deploy-test-' + [guid]::NewGuid())

try {
    New-Item -ItemType Directory -Force -Path $temporaryRoot | Out-Null
    $installRoot = Join-Path $temporaryRoot 'app'
    $dataRoot = Join-Path $temporaryRoot 'data'
    $startMenuRoot = Join-Path $temporaryRoot 'start-menu'
    $dummyDll = Join-Path $temporaryRoot 'ShuruIme.dll'
    [IO.File]::WriteAllBytes($dummyDll, [byte[]](1, 2, 3, 4))

    $settingsRoot = Join-Path $temporaryRoot 'settings'
    New-Item -ItemType Directory -Force -Path $settingsRoot | Out-Null
    foreach ($name in @(
        'ShuruSettings.exe',
        'ShuruSettings.dll',
        'ShuruSettings.deps.json',
        'ShuruSettings.runtimeconfig.json')) {
        Set-Content -LiteralPath (Join-Path $settingsRoot $name) -Value $name
    }

    $commonArguments = @{
        DllPath = $dummyDll
        SettingsPath = $settingsRoot
        PackagePath = (Join-Path $root 'data\lexicon')
        InstallRoot = $installRoot
        DataRoot = $dataRoot
        StartMenuRoot = $startMenuRoot
        SigningPolicy = 'Off'
        NoRegister = $true
    }
    & (Join-Path $root 'scripts\install_ime.ps1') -Action Install -Version test-1 @commonArguments
    if ($LASTEXITCODE -ne 0) { throw 'non-admin install failed' }

    & (Join-Path $root 'scripts\install_ime.ps1') -Action HealthCheck `
        -InstallRoot $installRoot -DataRoot $dataRoot -StartMenuRoot $startMenuRoot `
        -SigningPolicy Off -NoRegister
    if ($LASTEXITCODE -ne 0) { throw 'health check failed' }

    $installedSettings = Join-Path $installRoot 'versions\test-1\ShuruSettings.exe'
    if (-not (Test-Path -LiteralPath $installedSettings -PathType Leaf)) {
        throw 'settings application was not deployed'
    }
    $shortcutName = -join @(
        [char]0x53D1, [char]0x8D22, [char]0x62FC, [char]0x97F3,
        [char]0x8BBE, [char]0x7F6E
    )
    $shortcutPath = Join-Path $startMenuRoot ($shortcutName + '.lnk')
    if (-not (Test-Path -LiteralPath $shortcutPath -PathType Leaf)) {
        throw 'settings shortcut missing'
    }
    $shell = New-Object -ComObject WScript.Shell
    $shortcutTarget = $shell.CreateShortcut($shortcutPath).TargetPath
    if ([IO.Path]::GetFullPath($shortcutTarget) -ne [IO.Path]::GetFullPath($installedSettings)) {
        throw "settings shortcut target mismatch: actual=$shortcutTarget expected=$installedSettings"
    }

    $userDictionary = Join-Path $temporaryRoot 'user_dict.txt'
    Set-Content -LiteralPath $userDictionary -Value 'keep-me'
    $installedLexicon = Join-Path $dataRoot 'versions\1.2.0'
    $sentinel = Join-Path $installedLexicon 'reuse-sentinel.txt'
    Set-Content -LiteralPath $sentinel -Value 'keep-version-directory'
    $lock = [IO.File]::Open(
        (Join-Path $installedLexicon 'base_dict.txt'),
        [IO.FileMode]::Open,
        [IO.FileAccess]::Read,
        [IO.FileShare]::Read)
    try {
        & (Join-Path $root 'scripts\install_ime.ps1') -Action Install -Version test-2 @commonArguments
    } finally {
        $lock.Dispose()
    }
    if ($LASTEXITCODE -ne 0) { throw 'upgrade failed' }
    if ((Get-Content -LiteralPath $sentinel -Raw).Trim() -ne 'keep-version-directory') {
        throw 'matching lexicon version was replaced instead of reused'
    }

    $previousBeforeReinstall = (Get-Content (Join-Path $installRoot 'previous') -Raw).Trim()
    & (Join-Path $root 'scripts\install_ime.ps1') -Action Install -Version test-2 @commonArguments
    if ($LASTEXITCODE -ne 0) { throw 'idempotent reinstall failed' }
    if ((Get-Content (Join-Path $installRoot 'previous') -Raw).Trim() -ne $previousBeforeReinstall) {
        throw 'idempotent reinstall replaced the rollback pointer'
    }

    $conflictDirectory = Join-Path $installRoot 'versions\conflict'
    Copy-Item -LiteralPath (Join-Path $installRoot 'versions\test-2') `
        -Destination $conflictDirectory -Recurse
    Set-Content -LiteralPath (Join-Path $conflictDirectory 'ShuruSettings.dll') -Value 'conflict'
    & (Join-Path $root 'scripts\install_ime.ps1') -Action Install -Version conflict @commonArguments
    if ($LASTEXITCODE -eq 0) { throw 'conflicting immutable version unexpectedly succeeded' }
    if ((Get-Content (Join-Path $installRoot 'current') -Raw).Trim() -ne 'test-2' -or
        (Get-Content (Join-Path $conflictDirectory 'ShuruSettings.dll') -Raw).Trim() -ne 'conflict') {
        throw 'conflicting immutable version was modified or changed current pointer'
    }

    & (Join-Path $root 'scripts\install_ime.ps1') -Action Install -Version test-3 `
        -InjectFailure AfterPointer @commonArguments
    if ($LASTEXITCODE -eq 0) { throw 'failure injection unexpectedly succeeded' }
    if ((Get-Content (Join-Path $installRoot 'current') -Raw).Trim() -ne 'test-2') {
        throw 'injected failure did not restore current DLL pointer'
    }
    $shortcutTarget = $shell.CreateShortcut($shortcutPath).TargetPath
    $expectedTarget = Join-Path $installRoot 'versions\test-2\ShuruSettings.exe'
    if ([IO.Path]::GetFullPath($shortcutTarget) -ne [IO.Path]::GetFullPath($expectedTarget)) {
        throw "failure rollback shortcut target failed: actual=$shortcutTarget expected=$expectedTarget"
    }

    & (Join-Path $root 'scripts\install_ime.ps1') -Action Rollback `
        -InstallRoot $installRoot -DataRoot $dataRoot -StartMenuRoot $startMenuRoot `
        -SigningPolicy Off -NoRegister
    if ((Get-Content (Join-Path $installRoot 'current') -Raw).Trim() -ne 'test-1') {
        throw 'rollback pointer failed'
    }
    $shortcutTarget = $shell.CreateShortcut($shortcutPath).TargetPath
    if ([IO.Path]::GetFullPath($shortcutTarget) -ne [IO.Path]::GetFullPath($installedSettings)) {
        throw "rollback shortcut target failed: actual=$shortcutTarget expected=$installedSettings"
    }
    if ((Get-Content $userDictionary -Raw).Trim() -ne 'keep-me') {
        throw 'user dictionary overwritten'
    }

    $firstInstallRoot = Join-Path $temporaryRoot 'first-install-app'
    $firstDataRoot = Join-Path $temporaryRoot 'first-install-data'
    $firstStartMenuRoot = Join-Path $temporaryRoot 'first-install-start-menu'
    & (Join-Path $root 'scripts\install_ime.ps1') -Action Install `
        -DllPath $dummyDll -SettingsPath $settingsRoot `
        -PackagePath (Join-Path $root 'data\lexicon') -Version first-failure `
        -InstallRoot $firstInstallRoot -DataRoot $firstDataRoot `
        -StartMenuRoot $firstStartMenuRoot -SigningPolicy Off -NoRegister `
        -InjectFailure AfterPointer
    if ($LASTEXITCODE -eq 0) { throw 'first-install failure injection unexpectedly succeeded' }
    if (Test-Path -LiteralPath (Join-Path $firstInstallRoot 'current') -PathType Leaf) {
        throw 'failed first install left an application current pointer'
    }
    if (Test-Path -LiteralPath (Join-Path $firstDataRoot 'current') -PathType Leaf) {
        throw 'failed first install left a lexicon current pointer'
    }
    if (Get-ChildItem -LiteralPath $firstStartMenuRoot -Filter '*.lnk' -ErrorAction SilentlyContinue) {
        throw 'failed first install left a settings shortcut'
    }
    Write-Host 'deployment core tests passed'
} finally {
    Remove-Item -LiteralPath $temporaryRoot -Recurse -Force -ErrorAction SilentlyContinue
}
