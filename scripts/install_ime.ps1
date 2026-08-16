[CmdletBinding()]
param(
    [ValidateSet('Install', 'HealthCheck', 'Rollback', 'Cleanup')]
    [string]$Action = 'Install',
    [string]$DllPath = '',
    [string]$PackagePath = '',
    [string]$SettingsPath = '',
    [string]$Version = '',
    [string]$InstallRoot = '',
    [string]$DataRoot = '',
    [string]$StartMenuRoot = '',
    [string]$HealthCheckExe = '',
    [ValidateSet('Off', 'IfPresent', 'Required')]
    [string]$SigningPolicy = 'IfPresent',
    [ValidateSet('', 'AfterCopy', 'AfterManifest', 'AfterRegister', 'AfterPointer', 'HealthCheck')]
    [string]$InjectFailure = '',
    [switch]$NoRegister,
    [switch]$NoShortcut
)

$ErrorActionPreference = 'Stop'
$script:ExitCode = 1
$RepositoryRoot = Split-Path -Parent $PSScriptRoot
$SettingsFiles = @(
    'ShuruSettings.exe',
    'ShuruSettings.dll',
    'ShuruSettings.deps.json',
    'ShuruSettings.runtimeconfig.json'
)
$ApplicationResourceDirectory = 'data\skins'
$SettingsShortcutName = -join @(
    [char]0x8D22, [char]0x795E, [char]0x8F93, [char]0x5165, [char]0x6CD5,
    [char]0x8BBE, [char]0x7F6E
)

if (-not $InstallRoot) { $InstallRoot = Join-Path $env:ProgramFiles 'CaishenPinyin' }
if (-not $DataRoot) { $DataRoot = Join-Path $env:ProgramData 'CaishenPinyin\data\lexicon' }
if (-not $StartMenuRoot) {
    $StartMenuRoot = [Environment]::GetFolderPath([Environment+SpecialFolder]::CommonPrograms)
}

$LogDirectory = Join-Path $InstallRoot 'logs'
New-Item -ItemType Directory -Force -Path $LogDirectory | Out-Null
$LogPath = Join-Path $LogDirectory ('deploy-{0:yyyyMMdd-HHmmss}.log' -f (Get-Date))

function Write-DeployLog([string]$Message) {
    ('[{0:O}] {1}' -f (Get-Date), $Message) |
        Tee-Object -FilePath $LogPath -Append
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

function Assert-ReusableApplication(
    [string]$SourceDll,
    [string]$SettingsDirectory,
    [object[]]$ResourceFiles,
    [string]$Installed) {
    Test-InstalledApplication $Installed
    $pairs = @(
        [pscustomobject]@{ Source = $SourceDll; Installed = (Join-Path $Installed 'ShuruIme.dll') }
    )
    foreach ($name in $SettingsFiles) {
        $pairs += [pscustomobject]@{
            Source = (Join-Path $SettingsDirectory $name)
            Installed = (Join-Path $Installed $name)
        }
    }
    foreach ($resource in $ResourceFiles) {
        $pairs += [pscustomobject]@{
            Source = $resource.Source
            Installed = (Join-Path $Installed $resource.RelativePath.Replace('/', '\'))
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
    $installedResourcePaths = @()
    if ($componentManifest.PSObject.Properties.Name -contains 'resourceFiles') {
        $installedResourcePaths = @($componentManifest.resourceFiles | ForEach-Object {
            ([string]$_.path).Replace('\', '/')
        })
    }
    $sourceResourcePaths = @($ResourceFiles | ForEach-Object { $_.RelativePath })
    if (@(Compare-Object $installedResourcePaths $sourceResourcePaths).Count -ne 0) {
        Stop-Deployment 33 'installed application resources conflict with release files'
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
        foreach ($name in $SettingsFiles) {
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
    foreach ($name in $SettingsFiles) {
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
    if ($components.PSObject.Properties.Name -contains 'resourceFiles') {
        foreach ($resource in $components.resourceFiles) {
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
}

function Register-Dll([string]$Path) {
    if ($NoRegister) { Write-DeployLog 'registration skipped'; return }
    $process = Start-Process -FilePath "$env:SystemRoot\System32\regsvr32.exe" `
        -ArgumentList @('/s', ('"{0}"' -f $Path)) -Wait -PassThru
    if ($process.ExitCode -ne 0) { Stop-Deployment 40 "registration failed $($process.ExitCode)" }
}

function Unregister-Dll([string]$Path) {
    if ($NoRegister -or -not (Test-Path -LiteralPath $Path -PathType Leaf)) { return }
    $process = Start-Process -FilePath "$env:SystemRoot\System32\regsvr32.exe" `
        -ArgumentList @('/s', '/u', ('"{0}"' -f $Path)) -Wait -PassThru
    if ($process.ExitCode -ne 0) {
        Write-DeployLog "rollback warning: unregister failed $($process.ExitCode)"
    }
}

function Get-ShortcutPath {
    if ($NoShortcut -or -not $StartMenuRoot) { return '' }
    return Join-Path $StartMenuRoot ($SettingsShortcutName + '.lnk')
}

function Sync-SettingsShortcut([string]$VersionDirectory) {
    $shortcutPath = Get-ShortcutPath
    if (-not $shortcutPath) { return }
    $settingsExecutable = Join-Path $VersionDirectory 'ShuruSettings.exe'
    if (-not (Test-Path -LiteralPath $settingsExecutable -PathType Leaf)) {
        Remove-Item -LiteralPath $shortcutPath -Force -ErrorAction SilentlyContinue
        return
    }
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $shortcutPath) | Out-Null
    $temporary = "$shortcutPath.tmp-$PID.lnk"
    try {
        $shell = New-Object -ComObject WScript.Shell
        $shortcut = $shell.CreateShortcut($temporary)
        $shortcut.TargetPath = $settingsExecutable
        $shortcut.WorkingDirectory = $VersionDirectory
        $shortcut.Description = $SettingsShortcutName
        $shortcut.IconLocation = "$settingsExecutable,0"
        $shortcut.Save()
        Move-Item -LiteralPath $temporary -Destination $shortcutPath -Force
    } finally {
        Remove-Item -LiteralPath $temporary -Force -ErrorAction SilentlyContinue
    }
}

function Test-SettingsShortcut([string]$VersionDirectory) {
    $shortcutPath = Get-ShortcutPath
    if (-not $shortcutPath) { return }
    if (-not (Test-Path -LiteralPath $shortcutPath -PathType Leaf)) {
        Stop-Deployment 31 'settings shortcut missing'
    }
    $shell = New-Object -ComObject WScript.Shell
    $target = $shell.CreateShortcut($shortcutPath).TargetPath
    $expected = Join-Path $VersionDirectory 'ShuruSettings.exe'
    if ([IO.Path]::GetFullPath($target) -ne [IO.Path]::GetFullPath($expected)) {
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
        Test-SettingsDirectory $SettingsPath $releaseManifest
        $packageRoot = Split-Path -Parent $DllPath
        $applicationResources = @(Test-ApplicationResources $packageRoot $releaseManifest)
        Write-DeployLog "application resources=$($applicationResources.Count) root=$packageRoot"

        $oldInstallVersion = Read-Pointer (Join-Path $InstallRoot 'current')
        $oldDataVersion = Read-Pointer (Join-Path $DataRoot 'current')
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
            Assert-ReusableApplication $DllPath $SettingsPath $applicationResources $target
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
                foreach ($name in $SettingsFiles) {
                    Copy-Item -LiteralPath (Join-Path $SettingsPath $name) `
                        -Destination (Join-Path $stage $name)
                }
                $resourceManifest = @()
                foreach ($resource in $applicationResources) {
                    $destination = Join-Path $stage $resource.RelativePath.Replace('/', '\')
                    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $destination) |
                        Out-Null
                    Copy-Item -LiteralPath $resource.Source -Destination $destination
                    $resourceManifest += [pscustomobject][ordered]@{
                        path = $resource.RelativePath
                        sha256 = (Get-FileHash -LiteralPath $resource.Source -Algorithm SHA256).Hash.ToLowerInvariant()
                        size = [int64](Get-Item -LiteralPath $resource.Source).Length
                    }
                }
                [ordered]@{
                    schemaVersion = 2
                    settingsRequired = $true
                    resourceFiles = @($resourceManifest)
                } |
                    ConvertTo-Json | Set-Content `
                    -LiteralPath (Join-Path $stage 'install-components.json') -Encoding UTF8
            }
            if (-not $reuseLexicon) {
                foreach ($file in @($lexiconManifest.files) + @([pscustomobject]@{ path = 'manifest.json' })) {
                    Copy-Item -LiteralPath (Join-Path $PackagePath $file.path) `
                        -Destination (Join-Path $dataStage $file.path) -Force
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

            Register-Dll (Join-Path $target 'ShuruIme.dll')
            Invoke-FailureInjection 'AfterRegister'
            Set-Pointer (Join-Path $InstallRoot 'current') $Version
            Set-Pointer (Join-Path $DataRoot 'current') $dataVersion
            Sync-SettingsShortcut $target
            Invoke-FailureInjection 'AfterPointer'
            Test-CurrentInstallation

            if ($oldInstallVersion -and $oldInstallVersion -ne $Version) {
                Set-Pointer (Join-Path $InstallRoot 'previous') $oldInstallVersion
            }
            if ($oldDataVersion) {
                $rollbackDataVersion = if ($oldDataHealthy) { $oldDataVersion } else { $dataVersion }
                Set-Pointer (Join-Path $DataRoot 'previous') $rollbackDataVersion
            }
            Write-DeployLog "install complete $Version"
            exit 0
        } catch {
            Write-DeployLog "transaction rollback: $($_.Exception.Message)"
            if ($oldInstallVersion) {
                $oldDirectory = Join-Path $InstallRoot "versions\$oldInstallVersion"
                Register-Dll (Join-Path $oldDirectory 'ShuruIme.dll')
                Set-Pointer (Join-Path $InstallRoot 'current') $oldInstallVersion
                Sync-SettingsShortcut $oldDirectory
            } else {
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
