param([string]$DeployScript = '')
$ErrorActionPreference = 'Stop'
$repositoryRoot = Split-Path -Parent $PSScriptRoot
if (-not $DeployScript) { $DeployScript = Join-Path $repositoryRoot 'scripts\install_ime.ps1' }
$temporaryBase = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\')
$temporaryRoot = Join-Path $temporaryBase ('caishen-lexicon-recovery-' + [guid]::NewGuid().ToString('N'))
$packageRoot = Join-Path $temporaryRoot 'application-package'
$lexiconSource = Join-Path $temporaryRoot 'lexicon-package'
$savedFileAcls = @{}
$sourceContent = 'verified-system-lexicon'

function Require([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}

function New-CaseArguments([string]$Name) {
    $caseRoot = Join-Path $temporaryRoot $Name
    $personalRoot = Join-Path $caseRoot 'personal'
    $privateDirectory = Join-Path $personalRoot 'data\lexicon'
    New-Item -ItemType Directory -Force -Path $privateDirectory | Out-Null
    [IO.File]::WriteAllText((Join-Path $privateDirectory 'user_dict.txt'), 'private-sentinel')
    return @{
        DllPath = (Join-Path $packageRoot 'ShuruIme.dll')
        X86DllPath = (Join-Path $packageRoot 'ShuruIme32.dll')
        SettingsPath = $packageRoot
        PackagePath = $lexiconSource
        InstallRoot = (Join-Path $caseRoot 'app')
        DataRoot = (Join-Path $caseRoot 'data')
        UserDataRoot = $personalRoot
        StartMenuRoot = (Join-Path $caseRoot 'start-menu')
        SigningPolicy = 'Off'
        NoRegister = $true
        NoShortcut = $true
    }
}

function Invoke-TestInstall([hashtable]$Arguments, [string]$Version, [int]$ExpectedExit = 0) {
    $output = & $DeployScript -Action Install -Version $Version @Arguments 2>&1
    $code = $LASTEXITCODE
    if ($code -ne $ExpectedExit) {
        $output | Write-Output
        throw "Installation $Version exited $code; expected $ExpectedExit"
    }
    $privatePath = Join-Path $Arguments.UserDataRoot 'data\lexicon\user_dict.txt'
    Require ([IO.File]::ReadAllText($privatePath) -eq 'private-sentinel') 'Personal data changed.'
}

function Read-CurrentDataVersion([hashtable]$Arguments) {
    return [IO.File]::ReadAllText((Join-Path $Arguments.DataRoot 'current')).Trim()
}

function New-LegacyLexicon([hashtable]$Arguments, [string]$CanonicalVersion) {
    $path = Join-Path $Arguments.DataRoot "versions\$CanonicalVersion"
    New-Item -ItemType Directory -Force -Path $path | Out-Null
    Copy-Item -LiteralPath (Join-Path $lexiconSource 'manifest.json'), (Join-Path $lexiconSource 'test_dict.txt') -Destination $path
    [IO.File]::WriteAllText((Join-Path $path 'preserve.txt'), 'leave-old-directory')
    return $path
}

function Block-FileReads([string]$Path) {
    $savedFileAcls[$Path] = [IO.File]::GetAccessControl($Path).GetSecurityDescriptorSddlForm(
        [Security.AccessControl.AccessControlSections]::Access)
    $security = [IO.File]::GetAccessControl($Path)
    $security.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new(
        [Security.Principal.WindowsIdentity]::GetCurrent().User,
        [Security.AccessControl.FileSystemRights]'ReadAttributes, ReadData', 'Deny'))
    [IO.File]::SetAccessControl($Path, $security)
}

function Restore-TestFileAcls {
    foreach ($path in @($savedFileAcls.Keys)) {
        $security = New-Object Security.AccessControl.FileSecurity
        $security.SetSecurityDescriptorSddlForm($savedFileAcls[$path],
            [Security.AccessControl.AccessControlSections]::Access)
        [IO.File]::SetAccessControl($path, $security)
    }
}

function Require-RepairedLexicon([hashtable]$Arguments, [string]$CanonicalVersion) {
    $current = Read-CurrentDataVersion $Arguments
    Require ($current.StartsWith($CanonicalVersion + '-repair-')) 'No separate repair directory was selected.'
    $path = Join-Path $Arguments.DataRoot "versions\$current"
    Require ([IO.File]::ReadAllText((Join-Path $path 'test_dict.txt')) -eq $sourceContent) 'Repair payload differs from the verified source.'
    $expectedHash = (Get-FileHash -LiteralPath (Join-Path $lexiconSource 'manifest.json') -Algorithm SHA256).Hash
    $actualHash = (Get-FileHash -LiteralPath (Join-Path $path 'manifest.json') -Algorithm SHA256).Hash
    Require ($expectedHash -eq $actualHash) 'Repair manifest differs from the verified source.'
    return $current
}

try {
    New-Item -ItemType Directory -Force -Path $packageRoot, $lexiconSource | Out-Null
    foreach ($name in @('ShuruIme.dll','ShuruIme32.dll','ShuruSettings.exe','ShuruSettings.dll',
                         'ShuruSettings.deps.json','ShuruSettings.runtimeconfig.json')) {
        [IO.File]::WriteAllText((Join-Path $packageRoot $name), 'test-component')
    }
    $skin = Join-Path $packageRoot 'data\skins\classic_blue'
    New-Item -ItemType Directory -Force -Path $skin | Out-Null
    [IO.File]::WriteAllText((Join-Path $skin 'skin.ini'), '[General]')
    [IO.File]::WriteAllBytes((Join-Path $skin 'cand_bg.png'), [byte[]](1,2,3,4))
    $dictionary = Join-Path $lexiconSource 'test_dict.txt'
    [IO.File]::WriteAllText($dictionary, $sourceContent)
    $manifest = Join-Path $lexiconSource 'manifest.json'
    [ordered]@{
        schemaVersion = '2'
        version = 'cache-test'
        files = @([ordered]@{
            path = 'test_dict.txt'
            size = [int64](Get-Item -LiteralPath $dictionary).Length
            sha256 = (Get-FileHash -LiteralPath $dictionary -Algorithm SHA256).Hash.ToLowerInvariant()
        })
    } | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $manifest -Encoding UTF8
    $canonicalVersion = 'cache-test-' + (Get-FileHash -LiteralPath $manifest -Algorithm SHA256).Hash.Substring(0,12).ToLowerInvariant()

    $fresh = New-CaseArguments 'fresh'
    Invoke-TestInstall $fresh 'fresh-1'
    Require ((Read-CurrentDataVersion $fresh) -eq $canonicalVersion) 'Fresh install unnecessarily created a repair version.'
    Invoke-TestInstall $fresh 'fresh-2'
    Require ((Read-CurrentDataVersion $fresh) -eq $canonicalVersion) 'Healthy lexicon was not reused.'

    # 无 current 指针，但旧安装残留的同名词库拒绝读取 manifest。
    $blocked = New-CaseArguments 'blocked-manifest'
    $legacy = New-LegacyLexicon $blocked $canonicalVersion
    $blockedManifest = Join-Path $legacy 'manifest.json'
    Block-FileReads $blockedManifest
    Invoke-TestInstall $blocked 'blocked-1'
    $repaired = Require-RepairedLexicon $blocked $canonicalVersion
    $stillDenied = $false
    try { [void][IO.File]::ReadAllText($blockedManifest) }
    catch [UnauthorizedAccessException] { $stillDenied = $true }
    Require $stillDenied 'Repair loosened the original file access rules.'
    Require ([IO.File]::ReadAllText((Join-Path $legacy 'preserve.txt')) -eq 'leave-old-directory') 'Legacy files were changed.'
    Invoke-TestInstall $blocked 'blocked-2'
    Require ((Read-CurrentDataVersion $blocked) -eq $repaired) 'A healthy repaired version was not reused.'
    Invoke-TestInstall $blocked 'blocked-2'
    Require ((Read-CurrentDataVersion $blocked) -eq $repaired) 'Repeated repair was not idempotent.'

    # current 指向不可读旧版本，可选模型也被占用或拒绝读取。
    $optional = Join-Path $legacy 'rime-moqi-zh.gram'
    [IO.File]::WriteAllText($optional, 'user-managed-model')
    Block-FileReads $optional
    [IO.File]::WriteAllText((Join-Path $blocked.DataRoot 'current'), $canonicalVersion)
    $failedRepair = $blocked.Clone()
    $failedRepair.InjectFailure = 'AfterPointer'
    Invoke-TestInstall $failedRepair 'blocked-failure' 90
    Require ((Read-CurrentDataVersion $blocked) -eq $canonicalVersion) 'Failed repair did not restore the previous data pointer.'
    Require ([IO.File]::ReadAllText((Join-Path $blocked.InstallRoot 'current')).Trim() -eq 'blocked-2') 'Failed repair did not restore the previous application pointer.'
    Invoke-TestInstall $blocked 'blocked-3'
    $repairedAgain = Require-RepairedLexicon $blocked $canonicalVersion
    Require ($repairedAgain -ne $repaired) 'Unhealthy current pointer was not replaced.'
    Require ([IO.File]::ReadAllText((Join-Path $blocked.DataRoot 'previous')).Trim() -eq $repairedAgain) 'Rollback pointer retained an unusable lexicon.'

    $damaged = New-CaseArguments 'damaged-content'
    $damagedPath = New-LegacyLexicon $damaged $canonicalVersion
    [IO.File]::WriteAllText((Join-Path $damagedPath 'test_dict.txt'), 'damaged-old-file')
    Invoke-TestInstall $damaged 'damaged-1'
    [void](Require-RepairedLexicon $damaged $canonicalVersion)
    Require ([IO.File]::ReadAllText((Join-Path $damagedPath 'test_dict.txt')) -eq 'damaged-old-file') 'Damaged old file was overwritten.'

    $incomplete = New-CaseArguments 'missing-manifest'
    $incompletePath = New-LegacyLexicon $incomplete $canonicalVersion
    Remove-Item -LiteralPath (Join-Path $incompletePath 'manifest.json')
    Invoke-TestInstall $incomplete 'incomplete-1'
    [void](Require-RepairedLexicon $incomplete $canonicalVersion)

    # 只能修复已安装的缓存；安装包自身损坏仍须在改变指针前失败。
    $currentBefore = Read-CurrentDataVersion $fresh
    [IO.File]::WriteAllText($dictionary, 'tampered-package')
    Invoke-TestInstall $fresh 'invalid-source' 23
    Require ((Read-CurrentDataVersion $fresh) -eq $currentBefore) 'Invalid source changed the lexicon pointer.'
    Require ([IO.File]::ReadAllText((Join-Path $fresh.InstallRoot 'current')).Trim() -eq 'fresh-2') 'Invalid source changed the application pointer.'

    Restore-TestFileAcls
    Require ([IO.File]::ReadAllText($optional) -eq 'user-managed-model') 'Original optional model was not retained.'
    Require ((Get-FileHash -LiteralPath $blockedManifest -Algorithm SHA256).Hash -eq
        (Get-FileHash -LiteralPath $manifest -Algorithm SHA256).Hash) 'Original manifest content changed.'
    Write-Output 'installer lexicon recovery: fresh, denied access, damaged cache, reuse and source integrity passed'
} finally {
    Restore-TestFileAcls
    $resolved = [IO.Path]::GetFullPath($temporaryRoot)
    if (-not $resolved.StartsWith($temporaryBase + '\caishen-lexicon-recovery-', [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Unsafe test cleanup path.'
    }
    if (Test-Path -LiteralPath $resolved) { Remove-Item -LiteralPath $resolved -Recurse -Force }
}
