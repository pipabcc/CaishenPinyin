$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$temporaryRoot = Join-Path $env:TEMP ('facai-deploy-test-' + [guid]::NewGuid())

function New-TestLexiconPackage([string]$Path, [string]$Version, [string]$Content) {
    New-Item -ItemType Directory -Force -Path $Path | Out-Null
    $dictionaryPath = Join-Path $Path 'test_dict.txt'
    [IO.File]::WriteAllText($dictionaryPath, $Content, [Text.UTF8Encoding]::new($false))
    $file = Get-Item -LiteralPath $dictionaryPath
    [ordered]@{
        schemaVersion = '2'
        version = $Version
        files = @([ordered]@{
            path = 'test_dict.txt'
            sha256 = (Get-FileHash -LiteralPath $dictionaryPath -Algorithm SHA256).Hash.ToLowerInvariant()
            size = [int64]$file.Length
        })
    } | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath `
        (Join-Path $Path 'manifest.json') -Encoding UTF8
}

try {
    New-Item -ItemType Directory -Force -Path $temporaryRoot | Out-Null
    $installRoot = Join-Path $temporaryRoot 'app'
    $dataRoot = Join-Path $temporaryRoot 'data'
    $startMenuRoot = Join-Path $temporaryRoot 'start-menu'
    $userDataRoot = Join-Path $temporaryRoot 'user-data'
    $packageRoot = Join-Path $temporaryRoot 'package'
    $skinSource = Join-Path $packageRoot 'data\skins\classic_blue'
    New-Item -ItemType Directory -Force -Path $skinSource | Out-Null
    Set-Content -LiteralPath (Join-Path $skinSource 'skin.ini') -Value '[General]'
    [IO.File]::WriteAllBytes((Join-Path $skinSource 'cand_bg.png'), [byte[]](5, 6, 7, 8))
    $dummyDll = Join-Path $packageRoot 'ShuruIme.dll'
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
    Set-Content -LiteralPath (Join-Path $settingsRoot 'coreclr.dll') -Value 'self-contained-runtime'
    $nativeRuntimeRoot = Join-Path $settingsRoot 'runtimes\win-x64\native'
    New-Item -ItemType Directory -Force -Path $nativeRuntimeRoot | Out-Null
    Set-Content -LiteralPath (Join-Path $nativeRuntimeRoot 'runtime-helper.dll') `
        -Value 'nested-runtime'

    $commonArguments = @{
        DllPath = $dummyDll
        SettingsPath = $settingsRoot
        PackagePath = (Join-Path $root 'data\lexicon')
        InstallRoot = $installRoot
        DataRoot = $dataRoot
        StartMenuRoot = $startMenuRoot
        UserDataRoot = $userDataRoot
        SigningPolicy = 'Off'
        NoRegister = $true
    }
    $lexiconVersion = [string](
        Get-Content -LiteralPath (Join-Path $root 'data\lexicon\manifest.json') -Raw |
            ConvertFrom-Json).version
    $legacyLexicon = Join-Path $dataRoot "versions\$lexiconVersion"
    New-Item -ItemType Directory -Force -Path $legacyLexicon | Out-Null
    Set-Content -LiteralPath (Join-Path $legacyLexicon 'incomplete.txt') -Value 'keep-incomplete'
    & (Join-Path $root 'scripts\install_ime.ps1') -Action Install -Version test-1 @commonArguments
    if ($LASTEXITCODE -ne 0) { throw 'non-admin install failed' }

    & (Join-Path $root 'scripts\install_ime.ps1') -Action HealthCheck `
        -InstallRoot $installRoot -DataRoot $dataRoot -StartMenuRoot $startMenuRoot `
        -SigningPolicy Off -NoRegister
    if ($LASTEXITCODE -ne 0) { throw 'health check failed' }

    $installedDataVersion = (Get-Content -LiteralPath (Join-Path $dataRoot 'current') -Raw).Trim()
    if ($installedDataVersion -eq $lexiconVersion -or
        -not $installedDataVersion.StartsWith("$lexiconVersion-")) {
        throw "lexicon pointer is not content-addressed: $installedDataVersion"
    }
    if ((Get-Content -LiteralPath (Join-Path $legacyLexicon 'incomplete.txt') -Raw).Trim() -ne
        'keep-incomplete') {
        throw 'incomplete legacy lexicon directory was modified'
    }
    $installedLexicon = Join-Path $dataRoot "versions\$installedDataVersion"
    if (Test-Path -LiteralPath (Join-Path $installedLexicon 'rime-moqi-zh.gram')) {
        throw 'runtime-optional Grammar model was unexpectedly installed'
    }
    & (Join-Path $root 'scripts\install_ime.ps1') -Action HealthCheck `
        -InstallRoot $installRoot -DataRoot $dataRoot -StartMenuRoot $startMenuRoot `
        -SigningPolicy Off -NoRegister
    if ($LASTEXITCODE -ne 0) {
        throw 'health check rejected an absent runtime-optional Grammar model'
    }

    $installedSettings = Join-Path $installRoot 'versions\test-1\ShuruSettings.exe'
    if (-not (Test-Path -LiteralPath $installedSettings -PathType Leaf)) {
        throw 'settings application was not deployed'
    }
    if (-not (Test-Path -LiteralPath `
        (Join-Path $installRoot 'versions\test-1\coreclr.dll') -PathType Leaf)) {
        throw 'self-contained runtime file was not deployed'
    }
    if (-not (Test-Path -LiteralPath `
        (Join-Path $installRoot 'versions\test-1\runtimes\win-x64\native\runtime-helper.dll') `
        -PathType Leaf)) {
        throw 'nested self-contained runtime file was not deployed'
    }
    $installedSkin = Join-Path $installRoot 'versions\test-1\data\skins\classic_blue\skin.ini'
    if (-not (Test-Path -LiteralPath $installedSkin -PathType Leaf)) {
        $installedFiles = @(Get-ChildItem -LiteralPath (Join-Path $installRoot 'versions\test-1') `
            -Recurse -File | ForEach-Object { $_.FullName }) -join ', '
        throw "built-in skin resources were not deployed; installed=$installedFiles"
    }
    Remove-Item -LiteralPath $installedSkin -Force
    & (Join-Path $root 'scripts\install_ime.ps1') -Action HealthCheck `
        -InstallRoot $installRoot -DataRoot $dataRoot -StartMenuRoot $startMenuRoot `
        -SigningPolicy Off -NoRegister
    if ($LASTEXITCODE -eq 0) { throw 'health check accepted a missing built-in skin resource' }
    Copy-Item -LiteralPath (Join-Path $skinSource 'skin.ini') -Destination $installedSkin
    & (Join-Path $root 'scripts\install_ime.ps1') -Action HealthCheck `
        -InstallRoot $installRoot -DataRoot $dataRoot -StartMenuRoot $startMenuRoot `
        -SigningPolicy Off -NoRegister
    if ($LASTEXITCODE -ne 0) { throw 'health check failed after restoring built-in skin resource' }
    $shortcutName = -join @(
        [char]0x8D22, [char]0x795E, [char]0x8F93, [char]0x5165, [char]0x6CD5,
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

    $grammarPackage1 = Join-Path $temporaryRoot 'grammar-package-1'
    $grammarPackage2 = Join-Path $temporaryRoot 'grammar-package-2'
    New-TestLexiconPackage $grammarPackage1 'grammar-1' 'first lexicon'
    New-TestLexiconPackage $grammarPackage2 'grammar-2' 'second lexicon'
    $grammarInstallRoot = Join-Path $temporaryRoot 'grammar-app'
    $grammarDataRoot = Join-Path $temporaryRoot 'grammar-data'
    $grammarStartMenuRoot = Join-Path $temporaryRoot 'grammar-start-menu'
    $grammarUserDataRoot = Join-Path $temporaryRoot 'grammar-user-data'
    $grammarArguments = @{
        DllPath = $dummyDll
        SettingsPath = $settingsRoot
        InstallRoot = $grammarInstallRoot
        DataRoot = $grammarDataRoot
        StartMenuRoot = $grammarStartMenuRoot
        UserDataRoot = $grammarUserDataRoot
        SigningPolicy = 'Off'
        NoRegister = $true
        NoShortcut = $true
    }
    & (Join-Path $root 'scripts\install_ime.ps1') -Action Install `
        -Version grammar-app-1 -PackagePath $grammarPackage1 @grammarArguments
    if ($LASTEXITCODE -ne 0) { throw 'optional Grammar test first install failed' }
    $grammarDataVersion1 = (Get-Content -LiteralPath `
        (Join-Path $grammarDataRoot 'current') -Raw).Trim()
    $grammarFile1 = Join-Path $grammarDataRoot "versions\$grammarDataVersion1\rime-moqi-zh.gram"
    Set-Content -LiteralPath $grammarFile1 -Value 'user-managed-grammar'
    & (Join-Path $root 'scripts\install_ime.ps1') -Action Install `
        -Version grammar-app-2 -PackagePath $grammarPackage2 @grammarArguments
    if ($LASTEXITCODE -ne 0) { throw 'optional Grammar test upgrade failed' }
    $grammarDataVersion2 = (Get-Content -LiteralPath `
        (Join-Path $grammarDataRoot 'current') -Raw).Trim()
    $grammarFile2 = Join-Path $grammarDataRoot "versions\$grammarDataVersion2\rime-moqi-zh.gram"
    if (-not (Test-Path -LiteralPath $grammarFile2 -PathType Leaf) -or
        (Get-Content -LiteralPath $grammarFile2 -Raw).Trim() -ne 'user-managed-grammar') {
        throw 'user-managed Grammar was not preserved across lexicon upgrade'
    }
    & (Join-Path $root 'scripts\install_ime.ps1') -Action Uninstall `
        -InstallRoot $grammarInstallRoot -DataRoot $grammarDataRoot `
        -StartMenuRoot $grammarStartMenuRoot -UserDataRoot $grammarUserDataRoot `
        -SigningPolicy Off -NoRegister -NoShortcut -DeleteUserData
    if ($LASTEXITCODE -ne 0) { throw 'optional Grammar test cleanup failed' }

    $activeDataVersion = (Get-Content -LiteralPath (Join-Path $dataRoot 'current') -Raw).Trim()
    $preservedGrammar = Join-Path $dataRoot "versions\$activeDataVersion\rime-moqi-zh.gram"
    Set-Content -LiteralPath $preservedGrammar -Value 'preserve-on-uninstall'
    New-Item -ItemType Directory -Force -Path $userDataRoot | Out-Null
    $userDataSentinel = Join-Path $userDataRoot 'settings.ini'
    Set-Content -LiteralPath $userDataSentinel -Value 'preserve-user-data'
    & (Join-Path $root 'scripts\install_ime.ps1') -Action Uninstall `
        -InstallRoot $installRoot -DataRoot $dataRoot -StartMenuRoot $startMenuRoot `
        -UserDataRoot $userDataRoot -SigningPolicy Off -NoRegister
    if ($LASTEXITCODE -ne 0) { throw 'uninstall with retained data failed' }
    if ((Test-Path -LiteralPath (Join-Path $installRoot 'current') -PathType Leaf) -or
        (Test-Path -LiteralPath (Join-Path $installRoot 'versions') -PathType Container)) {
        throw 'uninstall left application pointers or version directories'
    }
    if (Test-Path -LiteralPath $shortcutPath -PathType Leaf) {
        throw 'uninstall left the settings shortcut'
    }
    if (-not (Test-Path -LiteralPath $userDataSentinel -PathType Leaf) -or
        -not (Test-Path -LiteralPath $preservedGrammar -PathType Leaf)) {
        throw 'default uninstall removed retained user data or Grammar'
    }
    & (Join-Path $root 'scripts\install_ime.ps1') -Action Uninstall `
        -InstallRoot $installRoot -DataRoot $dataRoot -StartMenuRoot $startMenuRoot `
        -UserDataRoot $userDataRoot -SigningPolicy Off -NoRegister -DeleteUserData
    if ($LASTEXITCODE -ne 0) { throw 'uninstall user-data cleanup failed' }
    if ((Test-Path -LiteralPath $dataRoot) -or (Test-Path -LiteralPath $userDataRoot)) {
        throw 'delete-user-data uninstall retained data directories'
    }
    Write-Host 'deployment core tests passed'
} finally {
    Remove-Item -LiteralPath $temporaryRoot -Recurse -Force -ErrorAction SilentlyContinue
}
