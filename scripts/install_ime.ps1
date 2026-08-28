[CmdletBinding()]
param(
    [ValidateSet('Install', 'HealthCheck', 'Rollback', 'Cleanup', 'Uninstall')]
    [string]$Action = 'Install',
    [string]$DllPath = '',
    [string]$PackagePath = '',
    [string]$SettingsPath = '',
    [string]$Version = '',
    [string]$InstallRoot = '',
    [string]$DataRoot = '',
    [string]$StartMenuRoot = '',
    [string]$HealthCheckExe = '',
    [string]$UserDataRoot = '',
    [ValidateSet('Off', 'IfPresent', 'Required')]
    [string]$SigningPolicy = 'IfPresent',
    [ValidateSet('', 'AfterCopy', 'AfterManifest', 'AfterRegister', 'AfterPointer', 'HealthCheck')]
    [string]$InjectFailure = '',
    [switch]$NoRegister,
    [switch]$NoShortcut,
    [switch]$SetDefaultInputMethod,
    [switch]$DeleteUserData
)

$ErrorActionPreference = 'Stop'

# A 32-bit NSIS process can inherit a PowerShell 7 PSModulePath before it starts
# 64-bit Windows PowerShell through Sysnative. Load the inbox modules explicitly
# so the 7.x manifests cannot shadow the Windows PowerShell implementations.
if ($PSVersionTable.PSEdition -eq 'Desktop') {
    foreach ($moduleName in @('Microsoft.PowerShell.Utility', 'Microsoft.PowerShell.Security')) {
        $moduleManifest = Join-Path $PSHOME "Modules\$moduleName\$moduleName.psd1"
        Import-Module -Name $moduleManifest -Force -ErrorAction Stop
    }
}

$script:ExitCode = 1
$RepositoryRoot = Split-Path -Parent $PSScriptRoot
$SettingsRequiredFiles = @(
    'ShuruSettings.exe',
    'ShuruSettings.dll',
    'ShuruSettings.deps.json',
    'ShuruSettings.runtimeconfig.json'
)
$ApplicationResourceDirectory = 'data\skins'
$RuntimeOptionalLexiconFiles = @('rime-moqi-zh.gram')
$DefaultInputMethodTip = '0804:{7C4E9F2A-1B3D-4A8E-9F6C-2D5E8B1A4C7F}{3A8B5C2E-9D1F-4E6A-B7C8-5D2E9F1A3B6C}'
$DefaultInputMethodRegistryPath = 'Registry::HKEY_CURRENT_USER\Control Panel\International\User Profile'
$SettingsShortcutName = -join @(
    [char]0x8D22, [char]0x795E, [char]0x8F93, [char]0x5165, [char]0x6CD5,
    [char]0x8BBE, [char]0x7F6E
)

if (-not $InstallRoot) { $InstallRoot = Join-Path $env:ProgramFiles 'CaishenPinyin' }
if (-not $DataRoot) { $DataRoot = Join-Path $env:ProgramData 'CaishenPinyin\data\lexicon' }
if (-not $UserDataRoot) { $UserDataRoot = Join-Path $env:LOCALAPPDATA 'CaishenPinyin' }
if (-not $StartMenuRoot) {
    $StartMenuRoot = [Environment]::GetFolderPath([Environment+SpecialFolder]::CommonPrograms)
}

if ($Action -eq 'Uninstall' -and -not (Test-Path -LiteralPath $InstallRoot -PathType Container)) {
    $LogBase = Join-Path $env:TEMP 'CaishenPinyin'
} else {
    $LogBase = Join-Path $InstallRoot 'logs'
}
$LogDirectory = $LogBase
New-Item -ItemType Directory -Force -Path $LogDirectory | Out-Null
$LogPath = Join-Path $LogDirectory ('deploy-{0:yyyyMMdd-HHmmss}.log' -f (Get-Date))
if ($Action -eq 'Uninstall' -and -not (Test-Path -LiteralPath $InstallRoot -PathType Container)) {
    $ProgressPath = Join-Path $env:TEMP 'CaishenPinyin\deploy-progress.json'
} else {
    $ProgressPath = Join-Path $InstallRoot 'deploy-progress.json'
}

function Write-DeployLog([string]$Message) {
    ('[{0:O}] {1}' -f (Get-Date), $Message) |
        Tee-Object -FilePath $LogPath -Append
}

function Write-DeployProgress([string]$Phase, [string]$State, [int]$Percent, [string]$Message) {
    if ($null -eq $Message) { $Message = '' }
    $payload = [ordered]@{
        schemaVersion = 1
        phase = $Phase
        state = $State
        percent = [Math]::Max(0, [Math]::Min(100, $Percent))
        message = $Message
        updatedUtc = (Get-Date).ToUniversalTime().ToString('O')
    }
    try {
        $parent = Split-Path -Parent $ProgressPath
        New-Item -ItemType Directory -Force -Path $parent | Out-Null
        $temporary = "$ProgressPath.tmp.$PID"
        $payload | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $temporary -Encoding UTF8
        Move-Item -LiteralPath $temporary -Destination $ProgressPath -Force
    } catch {
        # Progress reporting is non-critical and must not break deployment.
    }
    Write-DeployLog "progress phase=$Phase state=$State percent=$Percent message=$Message"
}

function Stop-Deployment([int]$Code, [string]$Message) {
    $script:ExitCode = $Code
    Write-DeployLog "ERROR[$Code] $Message"
    throw "ERROR[$Code] $Message"
}

function Read-Pointer([string]$Path) {
    if (Test-Path -LiteralPath $Path -PathType Leaf) {
        return (Get-Content -LiteralPath $Path -Raw).Trim()
    }
    return ''
}

function Set-Pointer([string]$Path, [string]$Value) {
    $temporary = "$Path.tmp.$PID"
    [IO.File]::WriteAllText($temporary, $Value, [Text.Encoding]::ASCII)
    Move-Item -LiteralPath $temporary -Destination $Path -Force
}

function Invoke-FailureInjection([string]$Point) {
    if ($InjectFailure -eq $Point) { Stop-Deployment 90 "injected failure: $Point" }
}

function Read-RegistryStringState([string]$Path, [string]$Name) {
    try {
        $properties = Get-ItemProperty -LiteralPath $Path -ErrorAction Stop
        $property = $properties.PSObject.Properties[$Name]
        if ($null -ne $property) {
            return [pscustomobject]@{ Present = $true; Value = [string]$property.Value }
        }
    } catch {
        # A missing registry value means no explicit override is configured.
    }
    return [pscustomobject]@{ Present = $false; Value = '' }
}

function Read-DefaultInputMethodState {
    return Read-RegistryStringState $DefaultInputMethodRegistryPath 'InputMethodOverride'
}

function Add-InputMethodToUserLanguageList([string]$InputTip) {
    if ($NoRegister) { return $false }
    try {
        $langs = Get-WinUserLanguageList -ErrorAction Stop
        $zh = $langs | Where-Object { $_.LanguageTag -like 'zh-Hans*' } | Select-Object -First 1
        if (-not $zh) {
            $newZh = (New-WinUserLanguageList 'zh-Hans-CN')[0]
            $newZh.InputMethodTips.Clear()
            [void]$newZh.InputMethodTips.Add($InputTip)
            [void]$langs.Add($newZh)
            Set-WinUserLanguageList -LanguageList $langs -Force -ErrorAction Stop | Out-Null
            Write-DeployLog "added zh-Hans-CN with $InputTip to user language list" | Out-Null
            return $true
        }
        if (-not ($zh.InputMethodTips -contains $InputTip)) {
            [void]$zh.InputMethodTips.Add($InputTip)
            Set-WinUserLanguageList -LanguageList $langs -Force -ErrorAction Stop | Out-Null
            Write-DeployLog "added $InputTip to user language list" | Out-Null
            return $true
        }
    } catch {
        Write-DeployLog "Add-InputMethodToUserLanguageList warning: $($_.Exception.Message)" | Out-Null
    }
    return $false
}

function Remove-InputMethodFromUserLanguageList([string]$InputTip) {
    if ($NoRegister) { return $false }
    try {
        $langs = Get-WinUserLanguageList -ErrorAction Stop
        $changed = $false
        foreach ($language in $langs) {
            if ($language.InputMethodTips -contains $InputTip) {
                [void]$language.InputMethodTips.Remove($InputTip)
                $changed = $true
            }
        }
        if ($changed) {
            Set-WinUserLanguageList -LanguageList $langs -Force -ErrorAction Stop | Out-Null
            Write-DeployLog "removed $InputTip from user language list" | Out-Null
            return $true
        }
    } catch {
        Write-DeployLog "Remove-InputMethodFromUserLanguageList warning: $($_.Exception.Message)" | Out-Null
    }
    return $false
}

function Set-DefaultInputMethodDirect([string]$InputTip) {
    New-Item -Path $DefaultInputMethodRegistryPath -Force | Out-Null
    New-ItemProperty -LiteralPath $DefaultInputMethodRegistryPath `
        -Name 'InputMethodOverride' -Value $InputTip -PropertyType String -Force | Out-Null
}

function Remove-DefaultInputMethodDirect {
    $current = Read-DefaultInputMethodState
    if ($current.Present) {
        Remove-ItemProperty -LiteralPath $DefaultInputMethodRegistryPath `
            -Name 'InputMethodOverride' -Force -ErrorAction Stop
    }
}

function Set-DefaultInputMethod([string]$InputTip) {
    if ($NoRegister) {
        Write-DeployLog 'default input method skipped because registration is disabled'
        return
    }
    try {
        Set-WinDefaultInputMethodOverride -InputTip $InputTip -ErrorAction Stop
    } catch {
        Write-DeployLog "Set-WinDefaultInputMethodOverride failed; using registry fallback: $($_.Exception.Message)"
        Set-DefaultInputMethodDirect $InputTip
    }
    $current = Read-DefaultInputMethodState
    if (-not $current.Present -or $current.Value -ne $InputTip) {
        Set-DefaultInputMethodDirect $InputTip
        $current = Read-DefaultInputMethodState
    }
    if (-not $current.Present -or $current.Value -ne $InputTip) {
        Stop-Deployment 43 'default input method could not be applied'
    }
    Write-DeployLog "default input method set to $InputTip"
}

function Restore-DefaultInputMethod($Snapshot) {
    if ($null -eq $Snapshot) { return }
    if ($Snapshot.Present) {
        try {
            Set-WinDefaultInputMethodOverride -InputTip ([string]$Snapshot.Value) -ErrorAction Stop
        } catch {
            Write-DeployLog "default input method cmdlet restore failed; using registry value: $($_.Exception.Message)"
            Set-DefaultInputMethodDirect ([string]$Snapshot.Value)
        }
        $current = Read-DefaultInputMethodState
        if (-not $current.Present -or $current.Value -ne [string]$Snapshot.Value) {
            Set-DefaultInputMethodDirect ([string]$Snapshot.Value)
        }
    } else {
        Remove-DefaultInputMethodDirect
    }
    $restored = Read-DefaultInputMethodState
    if ([bool]$Snapshot.Present -ne [bool]$restored.Present -or
        ($Snapshot.Present -and [string]$Snapshot.Value -ne [string]$restored.Value)) {
        throw 'default input method state could not be restored'
    }
    Write-DeployLog 'default input method state restored'
}

function Read-InstallState {
    $path = Join-Path $InstallRoot 'install-state.json'
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { return $null }
    try { return Get-Content -LiteralPath $path -Raw | ConvertFrom-Json }
    catch { Stop-Deployment 44 'install state is invalid' }
}

function Write-InstallState($State) {
    $path = Join-Path $InstallRoot 'install-state.json'
    $temporary = "$path.tmp.$PID"
    $State | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $temporary -Encoding UTF8
    Move-Item -LiteralPath $temporary -Destination $path -Force
}

function Remove-InstallState {
    Remove-Item -LiteralPath (Join-Path $InstallRoot 'install-state.json') `
        -Force -ErrorAction SilentlyContinue
}

function New-DefaultInputMethodPlan([bool]$Requested) {
    $existingState = Read-InstallState
    $current = Read-DefaultInputMethodState
    $existingDefault = if ($null -ne $existingState) { $existingState.defaultInputMethod } else { $null }
    if (-not $Requested -or $NoRegister) {
        return [pscustomobject]@{
            Apply = $false
            Previous = $null
            ExistingState = $existingState
            State = $existingDefault
        }
    }
    if ($current.Present -and $current.Value -eq $DefaultInputMethodTip) {
        return [pscustomobject]@{
            Apply = $false
            Previous = $null
            ExistingState = $existingState
            State = $existingDefault
        }
    }
    return [pscustomobject]@{
        Apply = $true
        Previous = $current
        ExistingState = $existingState
        State = [ordered]@{
            managed = $true
            tip = $DefaultInputMethodTip
            previousPresent = [bool]$current.Present
            previousValue = [string]$current.Value
        }
    }
}

function Commit-DefaultInputMethodState($Plan) {
    if ($null -eq $Plan) { return }
    if ($Plan.Apply) {
        Write-InstallState ([ordered]@{
            schemaVersion = 1
            defaultInputMethod = $Plan.State
        })
    } elseif ($null -ne $Plan.ExistingState) {
        Write-InstallState $Plan.ExistingState
    }
}

function Test-Lexicon([string]$Path) {
    $manifestPath = Join-Path $Path 'manifest.json'
    if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
        Stop-Deployment 20 'lexicon manifest missing'
    }
    try { $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json }
    catch { Stop-Deployment 21 'lexicon manifest invalid' }

    foreach ($file in $manifest.files) {
        $filePath = Join-Path $Path $file.path
        if (-not (Test-Path -LiteralPath $filePath -PathType Leaf)) {
            if ($file.PSObject.Properties.Name -contains 'runtimeOptional' -and
                $file.runtimeOptional -eq $true) {
                Write-DeployLog "optional runtime file absent: $($file.path)"
                continue
            }
            Stop-Deployment 22 "missing $($file.path)"
        }
        $actualHash = (Get-FileHash -LiteralPath $filePath -Algorithm SHA256).Hash
        if ($actualHash.ToLowerInvariant() -ne ([string]$file.sha256).ToLowerInvariant()) {
            Stop-Deployment 23 "hash mismatch $($file.path)"
        }
        if ($file.PSObject.Properties.Name -contains 'size' -and
            (Get-Item -LiteralPath $filePath).Length -ne [int64]$file.size) {
            Stop-Deployment 24 "size mismatch $($file.path)"
        }
    }
    return $manifest
}

function Test-LexiconHealthy([string]$Path) {
    $manifestPath = Join-Path $Path 'manifest.json'
    if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) { return $false }
    try { $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json }
    catch { return $false }

    foreach ($file in $manifest.files) {
        $filePath = Join-Path $Path $file.path
        if (-not (Test-Path -LiteralPath $filePath -PathType Leaf)) {
            if ($file.PSObject.Properties.Name -contains 'runtimeOptional' -and
                $file.runtimeOptional -eq $true) {
                continue
            }
            return $false
        }
        try {
            $actualHash = (Get-FileHash -LiteralPath $filePath -Algorithm SHA256).Hash
            if ($actualHash.ToLowerInvariant() -ne ([string]$file.sha256).ToLowerInvariant()) {
                return $false
            }
            if ($file.PSObject.Properties.Name -contains 'size' -and
                (Get-Item -LiteralPath $filePath).Length -ne [int64]$file.size) {
                return $false
            }
        } catch { return $false }
    }
    return $true
}

function Assert-ReusableLexicon([string]$Source, [string]$Installed) {
    [void](Test-Lexicon $Installed)
    $sourceHash = (Get-FileHash (Join-Path $Source 'manifest.json') -Algorithm SHA256).Hash
    $installedHash = (Get-FileHash (Join-Path $Installed 'manifest.json') -Algorithm SHA256).Hash
    if ($sourceHash -ne $installedHash) {
        Stop-Deployment 33 'installed lexicon version conflicts with package manifest'
    }
    Write-DeployLog "reusing verified lexicon $Installed"
}

function Get-ApplicationResourceFiles([string]$Root) {
    $resourceRoot = Join-Path $Root $ApplicationResourceDirectory
    if (-not (Test-Path -LiteralPath $resourceRoot -PathType Container)) { return @() }
    $resourceMarker = "\$ApplicationResourceDirectory\"
    return @(
        Get-ChildItem -LiteralPath $resourceRoot -Recurse -File | ForEach-Object {
            $markerIndex = $_.FullName.LastIndexOf(
                $resourceMarker, [StringComparison]::OrdinalIgnoreCase)
            if ($markerIndex -lt 0) {
                Stop-Deployment 25 "application resource path is invalid: $($_.FullName)"
            }
            [pscustomobject]@{
                Source = $_.FullName
                RelativePath = $_.FullName.Substring($markerIndex + 1).Replace('\', '/')
            }
        }
    )
}

function Test-ApplicationResources([string]$Root, $ReleaseManifest = $null) {
    $resourceFiles = @(Get-ApplicationResourceFiles $Root)
    if ($null -eq $ReleaseManifest) { return $resourceFiles }

    $manifestEntries = @($ReleaseManifest.files | Where-Object {
        ([string]$_.path).Replace('\', '/').StartsWith('data/skins/')
    })
    foreach ($entry in $manifestEntries) {
        $relativePath = ([string]$entry.path).Replace('\', '/')
        Test-ReleaseFile (Join-Path $Root $relativePath.Replace('/', '\')) `
            $relativePath $ReleaseManifest
    }
    foreach ($resource in $resourceFiles) {
        Test-ReleaseFile $resource.Source $resource.RelativePath $ReleaseManifest
    }
    return $resourceFiles
}

function Get-DirectoryPackageFiles([string]$Directory, [string]$Prefix = '') {
    $result = @()
    foreach ($file in Get-ChildItem -LiteralPath $Directory -File) {
        $result += [pscustomobject]@{
            Source = $file.FullName
            RelativePath = ($Prefix + $file.Name).Replace('\', '/')
        }
    }
    foreach ($childDirectory in Get-ChildItem -LiteralPath $Directory -Directory) {
        $result += @(Get-DirectoryPackageFiles $childDirectory.FullName `
            ($Prefix + $childDirectory.Name + '/'))
    }
    return $result
}

function Get-ApplicationPackageFiles(
    [string]$PackageRoot,
    [string]$SettingsDirectory,
    $ReleaseManifest = $null) {
    if ($null -ne $ReleaseManifest) {
        $componentEntries = @($ReleaseManifest.files | Where-Object {
            $_.PSObject.Properties.Name -contains 'component' -and
            [string]$_.component -in @('application', 'legal')
        })
        if ($componentEntries.Count -gt 0) {
            return @($componentEntries | ForEach-Object {
                $relativePath = ([string]$_.path).Replace('\', '/')
                [pscustomobject]@{
                    Source = Join-Path $PackageRoot $relativePath.Replace('/', '\')
                    RelativePath = $relativePath
                }
            })
        }
    }

    $files = @(
        Get-DirectoryPackageFiles $SettingsDirectory | ForEach-Object {
            $relativePath = $_.RelativePath
            if ($relativePath -eq 'release-manifest.json' -or
                $relativePath -eq 'ShuruIme.dll' -or
                $relativePath.StartsWith('data/lexicon/') -or
                $relativePath.StartsWith('data/skins/') -or
                $relativePath -eq 'user_dict.txt') {
                return
            }
            [pscustomobject]@{
                Source = $_.Source
                RelativePath = $relativePath
            }
        }
    )
    if ($files.Count -eq 0) {
        $files = @($SettingsRequiredFiles | ForEach-Object {
            [pscustomobject]@{
                Source = Join-Path $SettingsDirectory $_
                RelativePath = $_
            }
        })
    }
    return @($files + @(Test-ApplicationResources $PackageRoot $ReleaseManifest))
}

function Test-ApplicationPackageFiles([object[]]$Files, $ReleaseManifest = $null) {
    foreach ($file in $Files) {
        Test-ReleaseFile $file.Source $file.RelativePath $ReleaseManifest
    }
    return $Files
}

function Assert-ReusableApplication(
    [string]$SourceDll,
    [object[]]$ApplicationFiles,
    [string]$Installed) {
    Test-InstalledApplication $Installed
    $pairs = @(
        [pscustomobject]@{ Source = $SourceDll; Installed = (Join-Path $Installed 'ShuruIme.dll') }
    )
    foreach ($file in $ApplicationFiles) {
        $pairs += [pscustomobject]@{
            Source = $file.Source
            Installed = (Join-Path $Installed $file.RelativePath.Replace('/', '\'))
        }
    }
    foreach ($pair in $pairs) {
        if (-not (Test-Path -LiteralPath $pair.Installed -PathType Leaf) -or
            (Get-FileHash -LiteralPath $pair.Source -Algorithm SHA256).Hash -ne
            (Get-FileHash -LiteralPath $pair.Installed -Algorithm SHA256).Hash) {
            Stop-Deployment 33 'installed application version conflicts with release files'
        }
    }
    $componentManifest = Get-Content -LiteralPath `
        (Join-Path $Installed 'install-components.json') -Raw | ConvertFrom-Json
    $installedApplicationPaths = @()
    if ($componentManifest.PSObject.Properties.Name -contains 'applicationFiles') {
        $installedApplicationPaths = @($componentManifest.applicationFiles | ForEach-Object {
            ([string]$_.path).Replace('\', '/')
        })
    } elseif ($componentManifest.PSObject.Properties.Name -contains 'resourceFiles') {
        $installedApplicationPaths = @($SettingsRequiredFiles + @(
            $componentManifest.resourceFiles | ForEach-Object {
            ([string]$_.path).Replace('\', '/')
        }))
    }
    $sourceApplicationPaths = @($ApplicationFiles | ForEach-Object { $_.RelativePath })
    if (@(Compare-Object $installedApplicationPaths $sourceApplicationPaths).Count -ne 0) {
        Stop-Deployment 33 'installed application components conflict with release files'
    }
    Write-DeployLog "reusing verified application $Installed"
}

function Read-ReleaseManifest([string]$Dll) {
    $path = Join-Path (Split-Path -Parent $Dll) 'release-manifest.json'
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { return $null }
    try { return Get-Content -LiteralPath $path -Raw | ConvertFrom-Json }
    catch { Stop-Deployment 26 'release manifest invalid' }
}

function Test-ReleaseFile([string]$Path, [string]$RelativePath, $ReleaseManifest) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        Stop-Deployment 25 "required file missing: $RelativePath"
    }
    if ($null -eq $ReleaseManifest) { return }
    $entry = $ReleaseManifest.files |
        Where-Object { ([string]$_.path).Replace('\', '/') -eq $RelativePath } |
        Select-Object -First 1
    if ($null -eq $entry) { Stop-Deployment 26 "release manifest missing $RelativePath" }
    $actualHash = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash
    if ($actualHash.ToLowerInvariant() -ne ([string]$entry.sha256).ToLowerInvariant()) {
        Stop-Deployment 27 "release hash mismatch $RelativePath"
    }
    if ((Get-Item -LiteralPath $Path).Length -ne [int64]$entry.size) {
        Stop-Deployment 28 "release size mismatch $RelativePath"
    }
}

function Test-Dll([string]$Path, $ReleaseManifest = $null) {
    Test-ReleaseFile $Path 'ShuruIme.dll' $ReleaseManifest
    $signature = Get-AuthenticodeSignature -LiteralPath $Path
    if ($SigningPolicy -eq 'Required' -and $signature.Status -ne 'Valid') {
        Stop-Deployment 29 "signature required: $($signature.Status)"
    }
    if ($SigningPolicy -eq 'IfPresent' -and $signature.Status -notin @('Valid', 'NotSigned')) {
        Stop-Deployment 29 "invalid signature: $($signature.Status)"
    }
}

function Resolve-SettingsDirectory([string]$RequestedPath, [string]$Dll) {
    $candidates = @()
    if ($RequestedPath) { $candidates += $RequestedPath }
    $candidates += (Split-Path -Parent $Dll)
    $candidates += (Join-Path $RepositoryRoot 'settings\bin\Release\net8.0-windows')
    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            $candidate = Split-Path -Parent $candidate
        }
        if (-not (Test-Path -LiteralPath $candidate -PathType Container)) { continue }
        $complete = $true
        foreach ($name in $SettingsRequiredFiles) {
            if (-not (Test-Path -LiteralPath (Join-Path $candidate $name) -PathType Leaf)) {
                $complete = $false
                break
            }
        }
        if ($complete) { return (Resolve-Path -LiteralPath $candidate).Path }
    }
    Stop-Deployment 25 'settings program files are incomplete'
}

function Test-SettingsDirectory([string]$Directory, $ReleaseManifest = $null) {
    foreach ($name in $SettingsRequiredFiles) {
        Test-ReleaseFile (Join-Path $Directory $name) $name $ReleaseManifest
    }
}

function Test-InstalledApplication([string]$Directory) {
    $componentManifest = Join-Path $Directory 'install-components.json'
    if (-not (Test-Path -LiteralPath $componentManifest -PathType Leaf)) {
        return  # Versions before 0.4.0 only contained the DLL and remain rollback-compatible.
    }
    try { $components = Get-Content -LiteralPath $componentManifest -Raw | ConvertFrom-Json }
    catch { Stop-Deployment 31 'installed component manifest invalid' }
    if ($components.settingsRequired -eq $true) { Test-SettingsDirectory $Directory }
    $installedFiles = if ($components.PSObject.Properties.Name -contains 'applicationFiles') {
        @($components.applicationFiles)
    } elseif ($components.PSObject.Properties.Name -contains 'resourceFiles') {
        @($components.resourceFiles)
    } else {
        @()
    }
    foreach ($resource in $installedFiles) {
        $relativePath = ([string]$resource.path).Replace('/', '\')
        $resourcePath = Join-Path $Directory $relativePath
        if (-not (Test-Path -LiteralPath $resourcePath -PathType Leaf)) {
            Stop-Deployment 31 "installed resource missing: $relativePath"
        }
        if ((Get-FileHash -LiteralPath $resourcePath -Algorithm SHA256).Hash.ToLowerInvariant() -ne
            ([string]$resource.sha256).ToLowerInvariant() -or
            (Get-Item -LiteralPath $resourcePath).Length -ne [int64]$resource.size) {
            Stop-Deployment 31 "installed resource invalid: $relativePath"
        }
    }
}

function Register-Dll([string]$Path) {
    if ($NoRegister) { Write-DeployLog 'registration skipped'; return }
    $process = Start-Process -FilePath "$env:SystemRoot\System32\regsvr32.exe" `
        -ArgumentList @('/s', ('"{0}"' -f $Path)) -Wait -PassThru
    if ($process.ExitCode -ne 0) { Stop-Deployment 40 "registration failed $($process.ExitCode)" }
}

function Unregister-Dll([string]$Path, [switch]$Strict) {
    if ($NoRegister -or -not (Test-Path -LiteralPath $Path -PathType Leaf)) { return }
    $process = Start-Process -FilePath "$env:SystemRoot\System32\regsvr32.exe" `
        -ArgumentList @('/s', '/u', ('"{0}"' -f $Path)) -Wait -PassThru
    if ($process.ExitCode -ne 0) {
        if ($Strict) { Stop-Deployment 41 "unregistration failed $($process.ExitCode)" }
        Write-DeployLog "rollback warning: unregister failed $($process.ExitCode)"
    }
}

function Resolve-InstalledDll {
    $candidates = @()
    $current = Read-Pointer (Join-Path $InstallRoot 'current')
    if ($current) { $candidates += Join-Path $InstallRoot "versions\$current\ShuruIme.dll" }
    try {
        $registered = (Get-ItemProperty -LiteralPath `
            'Registry::HKEY_CLASSES_ROOT\CLSID\{7C4E9F2A-1B3D-4A8E-9F6C-2D5E8B1A4C7F}\InprocServer32' `
            -ErrorAction Stop).'(default)'
        if ($registered) { $candidates += [string]$registered }
    } catch { }
    $versions = Join-Path $InstallRoot 'versions'
    if (Test-Path -LiteralPath $versions -PathType Container) {
        $candidates += @(Get-ChildItem -LiteralPath $versions -Directory |
            Sort-Object LastWriteTime -Descending |
            ForEach-Object { Join-Path $_.FullName 'ShuruIme.dll' })
    }
    foreach ($candidate in ($candidates | Select-Object -Unique)) {
        if (-not (Test-Path -LiteralPath $candidate -PathType Leaf)) { continue }
        try {
            $full = [IO.Path]::GetFullPath((Resolve-Path -LiteralPath $candidate).Path)
            $root = [IO.Path]::GetFullPath($InstallRoot).TrimEnd('\') + '\'
            if ($full.StartsWith($root, [StringComparison]::OrdinalIgnoreCase)) { return $full }
        } catch { }
    }
    return ''
}

function Test-InputMethodRegistration {
    return Test-Path -LiteralPath `
        'Registry::HKEY_CLASSES_ROOT\CLSID\{7C4E9F2A-1B3D-4A8E-9F6C-2D5E8B1A4C7F}\InprocServer32' `
        -PathType Container
}

function Remove-PathBestEffort([string]$Path, [string]$Label) {
    if (-not $Path -or -not (Test-Path -LiteralPath $Path)) { return }
    try {
        Remove-Item -LiteralPath $Path -Recurse -Force -ErrorAction Stop
        Write-DeployLog "removed $Label $Path"
    } catch {
        # NSIS retries with /REBOOTOK so locked files can be removed after restart.
        Write-DeployLog "cleanup deferred for $Label ${Path}: $($_.Exception.Message)"
    }
}

function Invoke-Uninstall {
    Write-DeployProgress 'prepare' 'running' 5 'Checking installed components'
    if (-not (Test-Path -LiteralPath $InstallRoot -PathType Container)) {
        Write-DeployLog 'install root does not exist; uninstall is already complete'
        $orphanedShortcut = Get-ShortcutPath
        if ($orphanedShortcut) {
            Remove-Item -LiteralPath $orphanedShortcut -Force -ErrorAction SilentlyContinue
        }
        if ($DeleteUserData) {
            Remove-PathBestEffort $DataRoot 'lexicon data'
            Remove-PathBestEffort $UserDataRoot 'user data'
        }
        Write-DeployProgress 'complete' 'succeeded' 100 'Uninstall complete'
        return
    }

    $state = Read-InstallState
    $managedDefault = $null
    if (-not $NoRegister -and $null -ne $state -and $null -ne $state.defaultInputMethod -and
        $state.defaultInputMethod.managed -eq $true) {
        $managedDefault = [pscustomobject]@{
            Present = [bool]$state.defaultInputMethod.previousPresent
            Value = [string]$state.defaultInputMethod.previousValue
        }
    }
    $defaultRestored = $false
    $languageTipRemoved = $false
    try {
        if ($null -ne $managedDefault) {
            $currentDefault = Read-DefaultInputMethodState
            if ($currentDefault.Present -and
                $currentDefault.Value -eq $DefaultInputMethodTip) {
                Restore-DefaultInputMethod $managedDefault
                $defaultRestored = $true
            } else {
                Write-DeployLog 'default input method changed by user; leaving it untouched'
            }
        }

        Write-DeployProgress 'unregister' 'running' 35 'Unregistering input method'
        $languageTipRemoved = Remove-InputMethodFromUserLanguageList $DefaultInputMethodTip
        $installedDll = Resolve-InstalledDll
        if ($installedDll) {
            Unregister-Dll $installedDll -Strict
        } elseif (-not $NoRegister -and (Test-InputMethodRegistration)) {
            Stop-Deployment 42 'registered DLL is missing or outside the install root; run repair before uninstall'
        } else {
            Write-DeployLog 'installed DLL not found; registration cleanup skipped'
        }
        Write-DeployProgress 'remove' 'running' 65 'Removing program files'
        Remove-Item -LiteralPath (Join-Path $InstallRoot 'current') `
            -Force -ErrorAction SilentlyContinue
        Remove-Item -LiteralPath (Join-Path $InstallRoot 'previous') `
            -Force -ErrorAction SilentlyContinue
        $shortcutPath = Get-ShortcutPath
        if ($shortcutPath) {
            Remove-Item -LiteralPath $shortcutPath -Force -ErrorAction SilentlyContinue
        }
        Remove-InstallState
        Remove-Item -LiteralPath (Join-Path $InstallRoot 'versions') `
            -Recurse -Force -ErrorAction SilentlyContinue
    } catch {
        if ($languageTipRemoved) {
            [void](Add-InputMethodToUserLanguageList $DefaultInputMethodTip)
        }
        if ($defaultRestored) {
            try { Set-DefaultInputMethod $DefaultInputMethodTip } catch {
                Write-DeployLog "failed to restore managed default after uninstall error: $($_.Exception.Message)"
            }
        }
        throw
    }

    Write-DeployProgress 'data' 'running' 82 `
        $(if ($DeleteUserData) { 'Removing user data' } else { 'Preserving user data' })
    if ($DeleteUserData) {
        Remove-PathBestEffort $DataRoot 'lexicon data'
        Remove-PathBestEffort $UserDataRoot 'user data'
    } else {
        Write-DeployLog "user data preserved: $UserDataRoot"
        Write-DeployLog "lexicon data preserved: $DataRoot (including optional Grammar if present)"
    }
    Write-DeployProgress 'complete' 'succeeded' 100 'Uninstall transaction complete'
}

function Get-ShortcutPath {
    if ($NoShortcut -or -not $StartMenuRoot) { return '' }
    return Join-Path $StartMenuRoot ($SettingsShortcutName + '.lnk')
}

function Release-ComObject($Value) {
    if ($null -ne $Value -and [Runtime.InteropServices.Marshal]::IsComObject($Value)) {
        [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($Value)
    }
}

function Read-ShortcutTarget([string]$ShortcutPath) {
    $shortcutDirectory = Split-Path -Parent $ShortcutPath
    $temporary = Join-Path $shortcutDirectory (
        '.CaishenSettings-read-{0}.lnk' -f [guid]::NewGuid().ToString('N'))
    $shell = $null
    $shortcut = $null
    try {
        Copy-Item -LiteralPath $ShortcutPath -Destination $temporary -Force
        $shell = New-Object -ComObject WScript.Shell
        $shortcut = $shell.CreateShortcut($temporary)
        return [string]$shortcut.TargetPath
    } finally {
        Release-ComObject $shortcut
        Release-ComObject $shell
        Remove-Item -LiteralPath $temporary -Force -ErrorAction SilentlyContinue
    }
}

function Sync-SettingsShortcut([string]$VersionDirectory) {
    $shortcutPath = Get-ShortcutPath
    if (-not $shortcutPath) { return }
    $settingsExecutable = Join-Path $VersionDirectory 'ShuruSettings.exe'
    if (-not (Test-Path -LiteralPath $settingsExecutable -PathType Leaf)) {
        Remove-Item -LiteralPath $shortcutPath -Force -ErrorAction SilentlyContinue
        return
    }
    $shortcutDirectory = Split-Path -Parent $shortcutPath
    New-Item -ItemType Directory -Force -Path $shortcutDirectory | Out-Null
    $temporary = Join-Path $shortcutDirectory (
        '.CaishenSettings-{0}.lnk' -f [guid]::NewGuid().ToString('N'))
    $shell = $null
    $shortcut = $null
    try {
        $shell = New-Object -ComObject WScript.Shell
        $shortcut = $shell.CreateShortcut($temporary)
        $shortcut.TargetPath = $settingsExecutable
        $shortcut.WorkingDirectory = $VersionDirectory
        $shortcut.Description = $SettingsShortcutName
        $shortcut.IconLocation = "$settingsExecutable,0"
        $shortcut.Save()
        Release-ComObject $shortcut
        $shortcut = $null
        Release-ComObject $shell
        $shell = $null
        Move-Item -LiteralPath $temporary -Destination $shortcutPath -Force
    } finally {
        Release-ComObject $shortcut
        Release-ComObject $shell
        Remove-Item -LiteralPath $temporary -Force -ErrorAction SilentlyContinue
    }
}

# The Start menu search host (SearchHost.exe) runs inside an AppContainer, where
# file access must additionally match an S-1-15-2-* ACE or it is denied outright.
# This table mirrors kUserDataGrants in src/common/private_acl.cpp: the clipboard
# directory is deliberately absent because its history may hold passwords and the
# sandboxed host has no use for the v-mode panel.
# NOTE: this script is parsed as ANSI when it has no BOM, so keep it ASCII-only.
$AppContainerSids = @('S-1-15-2-1', 'S-1-15-2-2')
$AppContainerReadRights =
    [System.Security.AccessControl.FileSystemRights]'ReadAndExecute, Synchronize'
$AppContainerWriteRights =
    [System.Security.AccessControl.FileSystemRights]'ReadAndExecute, Synchronize, Write, Delete'
$AppContainerFullInherit =
    [System.Security.AccessControl.InheritanceFlags]'ObjectInherit, ContainerInherit'
$AppContainerFileInherit =
    [System.Security.AccessControl.InheritanceFlags]'ObjectInherit'
$AppContainerNoPropagate =
    [System.Security.AccessControl.PropagationFlags]'NoPropagateInherit'
$AppContainerPropagate = [System.Security.AccessControl.PropagationFlags]'None'

function Grant-PathToAppContainers {
    param(
        [string]$Path,
        [System.Security.AccessControl.InheritanceFlags]$Inheritance,
        [System.Security.AccessControl.PropagationFlags]$Propagation,
        [System.Security.AccessControl.FileSystemRights]$Rights
    )
    # Paths that do not exist yet are left to the IME and the settings app, which
    # run in the right user context; an elevated installer must not create folders
    # under somebody else's LOCALAPPDATA.
    if (-not $Path -or -not (Test-Path -LiteralPath $Path)) { return }
    try {
        $security = Get-Acl -LiteralPath $Path
        $changed = $false
        foreach ($sid in $AppContainerSids) {
            # Explicit entries only: inherited ACEs vanish when the parent DACL changes.
            $rules = $security.GetAccessRules(
                $true, $false, [System.Security.Principal.SecurityIdentifier])
            $matched = $rules | Where-Object {
                $_.AccessControlType -eq 'Allow' -and
                $_.IdentityReference.Value -eq $sid -and
                $_.InheritanceFlags -eq $Inheritance -and
                $_.PropagationFlags -eq $Propagation -and
                ($_.FileSystemRights -band $Rights) -eq $Rights
            }
            if ($matched) { continue }
            $identity = [System.Security.Principal.SecurityIdentifier]::new($sid)
            $rule = [System.Security.AccessControl.FileSystemAccessRule]::new(
                $identity, $Rights, $Inheritance, $Propagation, 'Allow')
            $security.AddAccessRule($rule)
            $changed = $true
        }
        if ($changed) {
            Set-Acl -LiteralPath $Path -AclObject $security
            Write-DeployLog "appcontainer access granted: $Path"
        }
    } catch {
        Write-DeployLog "appcontainer grant failed: $Path : $($_.Exception.Message)"
    }
}

function Deny-PathToAppContainers {
    param([string]$Path)
    # Being absent from the grant table only means "we do not open it". When a
    # parent carries an inheritable AppContainer ACE, access leaks down anyway, so
    # sensitive paths need an explicit deny on top.
    if (-not $Path -or -not (Test-Path -LiteralPath $Path)) { return }
    try {
        $security = Get-Acl -LiteralPath $Path
        $changed = $false
        $full = [System.Security.AccessControl.FileSystemRights]'FullControl'
        foreach ($sid in $AppContainerSids) {
            $rules = $security.GetAccessRules(
                $true, $false, [System.Security.Principal.SecurityIdentifier])
            $matched = $rules | Where-Object {
                $_.AccessControlType -eq 'Deny' -and
                $_.IdentityReference.Value -eq $sid -and
                $_.InheritanceFlags -eq $AppContainerFullInherit -and
                ($_.FileSystemRights -band $full) -eq $full
            }
            if ($matched) { continue }
            $identity = [System.Security.Principal.SecurityIdentifier]::new($sid)
            $rule = [System.Security.AccessControl.FileSystemAccessRule]::new(
                $identity, $full, $AppContainerFullInherit,
                $AppContainerPropagate, 'Deny')
            $security.AddAccessRule($rule)
            $changed = $true
        }
        if ($changed) {
            Set-Acl -LiteralPath $Path -AclObject $security
            Write-DeployLog "appcontainer access denied: $Path"
        }
    } catch {
        Write-DeployLog "appcontainer deny failed: $Path : $($_.Exception.Message)"
    }
}

function Grant-AppContainerAccess {
    # The root only inherits down to direct child files, so settings.ini keeps read
    # access across atomic replacement while the clipboard subdirectory stays closed.
    Grant-PathToAppContainers $UserDataRoot `
        $AppContainerFileInherit $AppContainerNoPropagate $AppContainerReadRights
    Grant-PathToAppContainers (Join-Path $UserDataRoot 'skins') `
        $AppContainerFullInherit $AppContainerPropagate $AppContainerReadRights
    # data\lexicon carries a protected DACL from EnsureCurrentUserOnlyPath, which
    # breaks inheritance, so it has to be listed explicitly.
    foreach ($name in @('data', 'data\lexicon', 'logs', 'paste_requests', 'ui_requests')) {
        Grant-PathToAppContainers (Join-Path $UserDataRoot $name) `
            $AppContainerFullInherit $AppContainerPropagate $AppContainerWriteRights
    }
    Deny-PathToAppContainers (Join-Path $UserDataRoot 'clipboard')
    # Lexicon and program directories only need read access. Both carried this ACE
    # from a manual icacls run that was never committed, so reinstalling dropped it;
    # pin it down here.
    Grant-PathToAppContainers $DataRoot `
        $AppContainerFullInherit $AppContainerPropagate $AppContainerReadRights
    Grant-PathToAppContainers $InstallRoot `
        $AppContainerFullInherit $AppContainerPropagate $AppContainerReadRights
}

function Test-SettingsShortcut([string]$VersionDirectory) {
    $shortcutPath = Get-ShortcutPath
    if (-not $shortcutPath) { return }
    if (-not (Test-Path -LiteralPath $shortcutPath -PathType Leaf)) {
        Stop-Deployment 31 'settings shortcut missing'
    }
    $target = Read-ShortcutTarget $shortcutPath
    $expected = Join-Path $VersionDirectory 'ShuruSettings.exe'
    if (-not $target -or
        [IO.Path]::GetFullPath($target) -ne [IO.Path]::GetFullPath($expected)) {
        Stop-Deployment 31 'settings shortcut target mismatch'
    }
}

function Test-CurrentInstallation {
    $installedVersion = Read-Pointer (Join-Path $InstallRoot 'current')
    $dataVersion = Read-Pointer (Join-Path $DataRoot 'current')
    if (-not $installedVersion -or -not $dataVersion) { Stop-Deployment 30 'current pointer missing' }
    $versionDirectory = Join-Path $InstallRoot "versions\$installedVersion"
    $dll = Join-Path $versionDirectory 'ShuruIme.dll'
    Test-Dll $dll
    Test-InstalledApplication $versionDirectory
    if (Test-Path -LiteralPath (Join-Path $versionDirectory 'install-components.json')) {
        Test-SettingsShortcut $versionDirectory
    }
    [void](Test-Lexicon (Join-Path $DataRoot "versions\$dataVersion"))
    if ($HealthCheckExe) {
        $arguments = @($dll, (Join-Path $DataRoot "versions\$dataVersion"))
        if (-not $NoRegister) { $arguments += '--registered' }
        & $HealthCheckExe @arguments
        if ($LASTEXITCODE -ne 0) { Stop-Deployment 32 'real health check failed' }
    }
    Invoke-FailureInjection 'HealthCheck'
    Write-DeployLog "healthy dll=$installedVersion lexicon=$dataVersion"
}

try {
    if ($Action -eq 'Uninstall') {
        Invoke-Uninstall
        exit 0
    }

    New-Item -ItemType Directory -Force -Path `
        $InstallRoot, $DataRoot, (Join-Path $InstallRoot 'versions'), (Join-Path $DataRoot 'versions') |
        Out-Null

    if ($Action -eq 'HealthCheck') {
        Test-CurrentInstallation
        exit 0
    }

    if ($Action -eq 'Install') {
        if (-not $DllPath) { Stop-Deployment 10 'DllPath required' }
        if (-not $PackagePath) { $PackagePath = Join-Path $RepositoryRoot 'data\lexicon' }
        $DllPath = (Resolve-Path -LiteralPath $DllPath).Path
        $PackagePath = (Resolve-Path -LiteralPath $PackagePath).Path
        $SettingsPath = Resolve-SettingsDirectory $SettingsPath $DllPath
        $lexiconManifest = Test-Lexicon $PackagePath
        if (-not $Version) { $Version = [string]$lexiconManifest.version }
        if ($Version -notmatch '^[0-9A-Za-z._-]+$') { Stop-Deployment 11 'invalid version' }
        $lexiconManifestHash = (Get-FileHash `
            -LiteralPath (Join-Path $PackagePath 'manifest.json') -Algorithm SHA256).Hash
        $dataVersion = '{0}-{1}' -f `
            ([string]$lexiconManifest.version), $lexiconManifestHash.Substring(0, 12).ToLowerInvariant()

        $releaseManifest = Read-ReleaseManifest $DllPath
        Test-Dll $DllPath $releaseManifest
        $packageRoot = Split-Path -Parent $DllPath
        Test-SettingsDirectory $SettingsPath $releaseManifest
        $applicationFiles = @(Get-ApplicationPackageFiles `
            $packageRoot $SettingsPath $releaseManifest)
        [void](Test-ApplicationPackageFiles $applicationFiles $releaseManifest)
        Write-DeployLog "application files=$($applicationFiles.Count) root=$packageRoot"
        if ($applicationFiles.Count -le 20) {
            foreach ($applicationFile in $applicationFiles) {
                Write-DeployLog "application file=$($applicationFile.RelativePath)"
            }
        }

        $oldInstallVersion = Read-Pointer (Join-Path $InstallRoot 'current')
        $oldDataVersion = Read-Pointer (Join-Path $DataRoot 'current')
        $defaultPlan = New-DefaultInputMethodPlan ([bool]$SetDefaultInputMethod)
        $stateCommitted = $false
        $languageTipAdded = $false
        Write-DeployProgress 'prepare' 'running' 8 'Preparing install transaction'
        $oldDataHealthy = $oldDataVersion -and (Test-LexiconHealthy `
            (Join-Path $DataRoot "versions\$oldDataVersion"))
        $stage = Join-Path $InstallRoot ".stage-$Version-$PID"
        $dataStage = Join-Path $DataRoot ".stage-$dataVersion-$PID"
        $target = Join-Path $InstallRoot "versions\$Version"
        $dataTarget = Join-Path $DataRoot "versions\$dataVersion"
        $reuseApplication = Test-Path -LiteralPath $target -PathType Container
        $reuseLexicon = Test-Path -LiteralPath $dataTarget -PathType Container
        if ((Test-Path -LiteralPath $target) -and -not $reuseApplication) {
            Stop-Deployment 33 'application version target is not a directory'
        }
        if ((Test-Path -LiteralPath $dataTarget) -and -not $reuseLexicon) {
            Stop-Deployment 33 'lexicon version target is not a directory'
        }
        if ($reuseApplication) {
            Assert-ReusableApplication $DllPath $applicationFiles $target
        }
        if ($reuseLexicon) { Assert-ReusableLexicon $PackagePath $dataTarget }

        try {
            if (-not $reuseApplication) {
                New-Item -ItemType Directory -Force -Path $stage | Out-Null
            }
            if (-not $reuseLexicon) {
                New-Item -ItemType Directory -Force -Path $dataStage | Out-Null
            }
            if (-not $reuseApplication) {
                Copy-Item -LiteralPath $DllPath -Destination (Join-Path $stage 'ShuruIme.dll')
                $applicationManifest = @()
                foreach ($file in $applicationFiles) {
                    $destination = Join-Path $stage $file.RelativePath.Replace('/', '\')
                    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $destination) |
                        Out-Null
                    Copy-Item -LiteralPath $file.Source -Destination $destination
                    $applicationManifest += [pscustomobject][ordered]@{
                        path = $file.RelativePath
                        sha256 = (Get-FileHash -LiteralPath $file.Source -Algorithm SHA256).Hash.ToLowerInvariant()
                        size = [int64](Get-Item -LiteralPath $file.Source).Length
                    }
                }
                [ordered]@{
                    schemaVersion = 3
                    settingsRequired = $true
                    applicationFiles = @($applicationManifest)
                } |
                    ConvertTo-Json | Set-Content `
                    -LiteralPath (Join-Path $stage 'install-components.json') -Encoding UTF8
            }
            if (-not $reuseLexicon) {
                foreach ($file in @($lexiconManifest.files) + @([pscustomobject]@{ path = 'manifest.json' })) {
                    $source = Join-Path $PackagePath $file.path
                    if ($file.PSObject.Properties.Name -contains 'runtimeOptional' -and
                        $file.runtimeOptional -eq $true) {
                        Write-DeployLog "optional runtime file not installed: $($file.path)"
                        continue
                    }
                    Copy-Item -LiteralPath $source `
                        -Destination (Join-Path $dataStage $file.path) -Force
                }
                if ($oldDataVersion) {
                    $oldDataDirectory = Join-Path $DataRoot "versions\$oldDataVersion"
                    foreach ($optionalPath in $RuntimeOptionalLexiconFiles) {
                        $optionalSource = Join-Path $oldDataDirectory $optionalPath
                        if (Test-Path -LiteralPath $optionalSource -PathType Leaf) {
                            $optionalDestination = Join-Path $dataStage $optionalPath
                            New-Item -ItemType Directory -Force -Path `
                                (Split-Path -Parent $optionalDestination) | Out-Null
                            Copy-Item -LiteralPath $optionalSource -Destination $optionalDestination -Force
                            Write-DeployLog "preserved runtime-optional file: $optionalPath"
                        }
                    }
                }
            }
            Invoke-FailureInjection 'AfterCopy'

            if (-not $reuseApplication) {
                Test-Dll (Join-Path $stage 'ShuruIme.dll')
                Test-InstalledApplication $stage
            }
            if (-not $reuseLexicon) { [void](Test-Lexicon $dataStage) }
            Invoke-FailureInjection 'AfterManifest'

            if (-not $reuseApplication) { Move-Item -LiteralPath $stage -Destination $target }
            if (-not $reuseLexicon) { Move-Item -LiteralPath $dataStage -Destination $dataTarget }

            Write-DeployProgress 'register' 'running' 58 'Registering input method'
            Register-Dll (Join-Path $target 'ShuruIme.dll')
            $languageTipAdded = Add-InputMethodToUserLanguageList $DefaultInputMethodTip
            Invoke-FailureInjection 'AfterRegister'
            if ($defaultPlan.Apply) {
                Write-DeployProgress 'default' 'running' 68 'Setting default input method'
                Set-DefaultInputMethod $DefaultInputMethodTip
            }
            Set-Pointer (Join-Path $InstallRoot 'current') $Version
            Set-Pointer (Join-Path $DataRoot 'current') $dataVersion
            Sync-SettingsShortcut $target
            Grant-AppContainerAccess
            Invoke-FailureInjection 'AfterPointer'
            Write-DeployProgress 'health' 'running' 82 'Running installation health check'
            Test-CurrentInstallation
            Commit-DefaultInputMethodState $defaultPlan
            $stateCommitted = $true

            if ($oldInstallVersion -and $oldInstallVersion -ne $Version) {
                Set-Pointer (Join-Path $InstallRoot 'previous') $oldInstallVersion
            }
            if ($oldDataVersion) {
                $rollbackDataVersion = if ($oldDataHealthy) { $oldDataVersion } else { $dataVersion }
                Set-Pointer (Join-Path $DataRoot 'previous') $rollbackDataVersion
            }
            Write-DeployLog "install complete $Version"
            # 方案 C：部署事务成功后，以隐藏方式调用随包工具按刚部署的
            # 版本化词库目录预生成 EngineSnapshot，写入当前用户缓存，
            # 首次打字即命中快照路径。整段自包含防御式执行：任何失败
            # （含路径解析）只记日志，绝不影响已完成的安装事务；运行时
            # 会在首次传统装载后自行再生快照兜底。不使用续行符，避免
            # 行尾空白破坏解析。
            try {
                $snapshotTool = $null
                if ($PSScriptRoot) {
                    $snapshotTool = Join-Path -Path $PSScriptRoot -ChildPath 'payload\engine_snapshot_build_tool.exe'
                }
                if ($snapshotTool -and (Test-Path -LiteralPath $snapshotTool -PathType Leaf)) {
                    Start-Process -FilePath $snapshotTool -ArgumentList @('--lexicon-dir=' + $dataTarget) -WindowStyle Hidden
                    Write-DeployLog 'snapshot pregeneration started'
                } else {
                    Write-DeployLog 'snapshot pregeneration skipped: tool not packaged'
                }
            } catch {
                Write-DeployLog ('snapshot pregeneration skipped: ' + $_.Exception.Message)
            }
            Write-DeployProgress 'complete' 'succeeded' 100 'Install transaction complete'
            exit 0
        } catch {
            Write-DeployLog "transaction rollback: $($_.Exception.Message)"
            if ($defaultPlan -and $defaultPlan.Apply) {
                try { Restore-DefaultInputMethod $defaultPlan.Previous } catch {
                    Write-DeployLog "default input method rollback failed: $($_.Exception.Message)"
                }
            }
            if ($stateCommitted) {
                if ($null -ne $defaultPlan.ExistingState) {
                    Write-InstallState $defaultPlan.ExistingState
                } else {
                    Remove-InstallState
                }
            }
            if ($oldInstallVersion) {
                $oldDirectory = Join-Path $InstallRoot "versions\$oldInstallVersion"
                Register-Dll (Join-Path $oldDirectory 'ShuruIme.dll')
                Set-Pointer (Join-Path $InstallRoot 'current') $oldInstallVersion
                Sync-SettingsShortcut $oldDirectory
            } else {
                if ($languageTipAdded) {
                    [void](Remove-InputMethodFromUserLanguageList $DefaultInputMethodTip)
                }
                Unregister-Dll (Join-Path $target 'ShuruIme.dll')
                Remove-Item -LiteralPath (Join-Path $InstallRoot 'current') -Force -ErrorAction SilentlyContinue
                $shortcutPath = Get-ShortcutPath
                if ($shortcutPath) {
                    Remove-Item -LiteralPath $shortcutPath -Force -ErrorAction SilentlyContinue
                }
            }
            if ($oldDataVersion) {
                Set-Pointer (Join-Path $DataRoot 'current') $oldDataVersion
            } else {
                Remove-Item -LiteralPath (Join-Path $DataRoot 'current') -Force -ErrorAction SilentlyContinue
            }
            Remove-Item -LiteralPath $stage, $dataStage -Recurse -Force -ErrorAction SilentlyContinue
            Write-DeployProgress 'rollback' 'failed' 100 'Install failed and previous state was restored'
            throw
        }
    }

    if ($Action -eq 'Rollback') {
        $previousInstallVersion = Read-Pointer (Join-Path $InstallRoot 'previous')
        $previousDataVersion = Read-Pointer (Join-Path $DataRoot 'previous')
        if (-not $previousInstallVersion -or -not $previousDataVersion) {
            Stop-Deployment 50 'previous unavailable'
        }
        $previousDirectory = Join-Path $InstallRoot "versions\$previousInstallVersion"
        Register-Dll (Join-Path $previousDirectory 'ShuruIme.dll')
        Set-Pointer (Join-Path $InstallRoot 'current') $previousInstallVersion
        Set-Pointer (Join-Path $DataRoot 'current') $previousDataVersion
        Sync-SettingsShortcut $previousDirectory
        Test-CurrentInstallation
        exit 0
    }

    if ($Action -eq 'Cleanup') {
        $keep = @(
            (Read-Pointer (Join-Path $InstallRoot 'current')),
            (Read-Pointer (Join-Path $InstallRoot 'previous'))
        )
        Get-ChildItem (Join-Path $InstallRoot 'versions') -Directory |
            Where-Object { $keep -notcontains $_.Name } |
            Remove-Item -Recurse -Force -ErrorAction SilentlyContinue
        exit 0
    }
} catch {
    if (-not $_.Exception.Message.StartsWith('ERROR[')) {
        Write-DeployLog "FAILED $($_.Exception.Message)"
    }
    exit $script:ExitCode
}
