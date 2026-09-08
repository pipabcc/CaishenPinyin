param([Parameter(Mandatory = $true)][string]$ProbeExe)
$ErrorActionPreference = 'Stop'
$repositoryRoot = Split-Path -Parent $PSScriptRoot
$temporaryBase = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\')
$temporaryRoot = Join-Path $temporaryBase ('caishen-installer-acl-' + [guid]::NewGuid().ToString('N'))
$application = Join-Path $temporaryRoot 'app'
$publicParent = Join-Path $temporaryRoot 'shared'
$publicRoot = Join-Path $publicParent 'lexicon'
$privateRoot = Join-Path $temporaryRoot 'personal'
$publicFile = Join-Path $publicRoot 'base_dict.txt'
$privateFile = Join-Path $privateRoot 'data\user_dict.txt'
$deployScript = Join-Path $repositoryRoot 'scripts\install_ime.ps1'
$packageSids = @('S-1-15-2-1', 'S-1-15-2-2')
$readRights = [Security.AccessControl.FileSystemRights]'ReadAndExecute, Synchronize'

function Read-OtherRules([string]$Path) {
    $security = Get-Acl -LiteralPath $Path
    return @($security.GetAccessRules($true, $true, [Security.Principal.SecurityIdentifier]) |
        Where-Object { $_.IdentityReference.Value -notin $packageSids } |
        ForEach-Object {
            '{0}|{1}|{2}|{3}|{4}' -f $_.IdentityReference.Value, $_.FileSystemRights,
                $_.AccessControlType, $_.InheritanceFlags, $_.PropagationFlags
        } | Sort-Object)
}

try {
    New-Item -ItemType Directory -Path $temporaryRoot | Out-Null
    $rootSecurity = New-Object Security.AccessControl.DirectorySecurity
    $rootSecurity.SetAccessRuleProtection($true, $false)
    foreach ($sid in @([Security.Principal.WindowsIdentity]::GetCurrent().User.Value,
                       'S-1-5-18', 'S-1-5-32-544')) {
        $rootSecurity.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new(
            [Security.Principal.SecurityIdentifier]::new($sid), 'FullControl',
            'ContainerInherit, ObjectInherit', 'None', 'Allow'))
    }
    Set-Acl -LiteralPath $temporaryRoot -AclObject $rootSecurity
    New-Item -ItemType Directory -Force -Path $application, $publicRoot,
        (Split-Path -Parent $privateFile) | Out-Null
    [IO.File]::WriteAllText($publicFile, 'public lexicon sentinel')
    [IO.File]::WriteAllText($privateFile, 'private learning sentinel')

    $legacy = Get-Acl -LiteralPath $publicParent
    foreach ($sid in $packageSids) {
        $identity = [Security.Principal.SecurityIdentifier]::new($sid)
        foreach ($access in @('Allow', 'Deny')) {
            $legacy.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new(
                $identity, 'FullControl', 'ContainerInherit, ObjectInherit', 'None', $access))
        }
    }
    Set-Acl -LiteralPath $publicParent -AclObject $legacy
    $legacyFile = Get-Acl -LiteralPath $publicFile
    foreach ($sid in $packageSids) {
        $legacyFile.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new(
            [Security.Principal.SecurityIdentifier]::new($sid), 'FullControl', 'Deny'))
    }
    Set-Acl -LiteralPath $publicFile -AclObject $legacyFile
    $before = (Get-Acl -LiteralPath $publicFile).Sddl
    $otherBefore = @(Read-OtherRules $publicFile)

    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $deployScript `
        -Action RepairPermissions -InstallRoot $application -DataRoot $publicRoot `
        -UserDataRoot $publicParent -NoShortcut -SigningPolicy Off | Out-Null
    if ($LASTEXITCODE -ne 54) { throw 'Overlapping private/public roots were not rejected.' }
    if ((Get-Acl -LiteralPath $publicFile).Sddl -ne $before) {
        throw 'Rejected scope changed the public file ACL.'
    }

    $repairOutput = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $deployScript `
        -Action RepairPermissions -InstallRoot $application -DataRoot $publicRoot `
        -UserDataRoot $privateRoot -NoShortcut -SigningPolicy Off
    if ($LASTEXITCODE -ne 0) { $repairOutput | Write-Output; throw 'Permission repair failed.' }
    foreach ($path in @($publicRoot, $publicFile)) {
        $rules = (Get-Acl -LiteralPath $path).GetAccessRules(
            $true, $true, [Security.Principal.SecurityIdentifier])
        foreach ($sid in $packageSids) {
            $entries = @($rules | Where-Object { $_.IdentityReference.Value -eq $sid })
            if ($entries.Count -eq 0 -or @($entries | Where-Object {
                $_.AccessControlType -ne 'Allow' -or
                ([int64]$_.FileSystemRights -band (-bnot [int64]$readRights)) -ne 0 -or
                ($_.FileSystemRights -band $readRights) -ne $readRights
            }).Count -ne 0) {
                $entries | Select-Object AccessControlType, FileSystemRights, IsInherited |
                    ConvertTo-Json | Write-Output
                throw 'Public package access was not restored to read-only.'
            }
        }
    }
    if (@(Compare-Object $otherBefore @(Read-OtherRules $publicFile)).Count -ne 0) {
        throw 'Repair changed non-package access rules.'
    }
    if ([IO.File]::ReadAllText($publicFile) -ne 'public lexicon sentinel' -or
        [IO.File]::ReadAllText($privateFile) -ne 'private learning sentinel') {
        throw 'Permission repair changed file content.'
    }
    & $ProbeExe --verify-paths $privateFile $publicFile
    if ($LASTEXITCODE -ne 0) {
        $privateSecurity = Get-Acl -LiteralPath $privateFile
        Write-Output $privateSecurity.Sddl
        Write-Output ('Private ACL canonical: ' + $privateSecurity.AreAccessRulesCanonical)
        throw 'Real AppContainer access probe failed.'
    }

    $repaired = (Get-Acl -LiteralPath $publicFile).Sddl
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $deployScript `
        -Action RepairPermissions -InstallRoot $application -DataRoot $publicRoot `
        -UserDataRoot $privateRoot -NoShortcut -SigningPolicy Off | Out-Null
    if ($LASTEXITCODE -ne 0 -or (Get-Acl -LiteralPath $publicFile).Sddl -ne $repaired) {
        throw 'Permission repair is not idempotent.'
    }
    Write-Output 'installer ACL scope, migration and real AppContainer checks passed'
} finally {
    $resolvedRoot = [IO.Path]::GetFullPath($temporaryRoot)
    if (-not $resolvedRoot.StartsWith($temporaryBase + '\caishen-installer-acl-',
            [StringComparison]::OrdinalIgnoreCase)) { throw 'Unsafe test cleanup path.' }
    if (Test-Path -LiteralPath $resolvedRoot) {
        Remove-Item -LiteralPath $resolvedRoot -Recurse -Force
    }
}
