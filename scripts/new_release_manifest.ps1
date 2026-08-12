[CmdletBinding()]
param(
  [Parameter(Mandatory=$true)][string]$PackageRoot,
  [Parameter(Mandatory=$true)][string]$Version,
  [ValidateSet('Off','IfPresent','Required')][string]$SigningPolicy='IfPresent'
)
$ErrorActionPreference='Stop'
$PackageRoot=(Resolve-Path -LiteralPath $PackageRoot).Path
$dll=Join-Path $PackageRoot 'ShuruIme.dll'
if(-not(Test-Path -LiteralPath $dll -PathType Leaf)){throw 'ShuruIme.dll missing'}
$files=@()
Get-ChildItem -LiteralPath $PackageRoot -Recurse -File | Where-Object {$_.Name -ne 'release-manifest.json' -and $_.Name -ne 'user_dict.txt'} | Sort-Object FullName | ForEach-Object {
  $relative=$_.FullName.Substring($PackageRoot.Length).TrimStart('\').Replace('\','/')
  $item=[ordered]@{path=$relative;sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant();size=[int64]$_.Length}
  if($relative -eq 'ShuruIme.dll'){
    $vi=$_.VersionInfo
    $item.fileVersion=[string]$vi.FileVersion
    $item.productVersion=[string]$vi.ProductVersion
    $sig=Get-AuthenticodeSignature -LiteralPath $_.FullName
    $item.signatureStatus=[string]$sig.Status
    if($sig.SignerCertificate){$item.signerThumbprint=[string]$sig.SignerCertificate.Thumbprint}
    if($SigningPolicy -eq 'Required' -and $sig.Status -ne 'Valid'){throw "Authenticode signature required but status is $($sig.Status)"}
    if($SigningPolicy -eq 'IfPresent' -and $sig.Status -notin @('Valid','NotSigned')){throw "invalid Authenticode signature: $($sig.Status)"}
  }
  $files += [pscustomobject]$item
}
$manifest=[ordered]@{schemaVersion='1';product='Facai Pinyin';version=$Version;signingPolicy=$SigningPolicy;createdUtc=(Get-Date).ToUniversalTime().ToString('O');files=$files}
$manifest | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $PackageRoot 'release-manifest.json') -Encoding UTF8
Write-Host "release manifest created: $PackageRoot\release-manifest.json"
