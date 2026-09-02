[CmdletBinding()]
param(
  [Parameter(Mandatory=$true)][string]$PackageRoot,
  [Parameter(Mandatory=$true)][string]$Version,
  [ValidateSet('Off','IfPresent','Required')][string]$SigningPolicy='Off'
)
$ErrorActionPreference='Stop'
$PackageRoot=(Resolve-Path -LiteralPath $PackageRoot).Path
$dll=Join-Path $PackageRoot 'ShuruIme.dll'
if(-not(Test-Path -LiteralPath $dll -PathType Leaf)){throw 'ShuruIme.dll missing'}
$x86Dll=Join-Path $PackageRoot 'ShuruIme32.dll'
if(-not(Test-Path -LiteralPath $x86Dll -PathType Leaf)){throw 'ShuruIme32.dll missing'}
function Get-PeArchitecture([string]$Path){
  $stream=[IO.File]::OpenRead($Path)
  try{
    $reader=[IO.BinaryReader]::new($stream)
    if($reader.ReadUInt16()-ne 0x5A4D){throw "invalid PE DOS signature: $Path"}
    $stream.Position=0x3c;$peOffset=$reader.ReadInt32()
    if($peOffset-lt 0 -or $peOffset+6-gt $stream.Length){throw "invalid PE header offset: $Path"}
    $stream.Position=$peOffset
    if($reader.ReadUInt32()-ne 0x00004550){throw "invalid PE signature: $Path"}
    $machine=$reader.ReadUInt16()
    if($machine-eq 0x8664){return 'x64'}
    if($machine-eq 0x014c){return 'x86'}
    throw ('unsupported PE machine 0x{0:X4}: {1}' -f $machine,$Path)
  }finally{$stream.Dispose()}
}
$files=@()
Get-ChildItem -LiteralPath $PackageRoot -Recurse -File | Where-Object {$_.Name -ne 'release-manifest.json' -and $_.Name -ne 'user_dict.txt'} | Sort-Object FullName | ForEach-Object {
  $relative=$_.FullName.Substring($PackageRoot.Length).TrimStart('\').Replace('\','/')
  $component = if($relative -in @('ShuruIme.dll','ShuruIme32.dll')){'ime'}
    elseif($relative.StartsWith('data/lexicon/',[StringComparison]::OrdinalIgnoreCase)){'lexicon'}
    elseif($relative.StartsWith('licenses/',[StringComparison]::OrdinalIgnoreCase) -or $relative -eq 'THIRD_PARTY_NOTICES.md'){'legal'}
    else{'application'}
  $item=[ordered]@{path=$relative;component=$component;sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant();size=[int64]$_.Length}
  if($relative -in @('ShuruIme.dll','ShuruIme32.dll')){
    $vi=$_.VersionInfo
    $item.fileVersion=[string]$vi.FileVersion
    $item.productVersion=[string]$vi.ProductVersion
    $sig=Get-AuthenticodeSignature -LiteralPath $_.FullName
    $item.signatureStatus=[string]$sig.Status
    $item.architecture=Get-PeArchitecture $_.FullName
    if($sig.SignerCertificate){$item.signerThumbprint=[string]$sig.SignerCertificate.Thumbprint}
    if($SigningPolicy -eq 'Required' -and $sig.Status -ne 'Valid'){throw "Authenticode signature required but status is $($sig.Status)"}
    if($SigningPolicy -eq 'IfPresent' -and $sig.Status -notin @('Valid','NotSigned')){throw "invalid Authenticode signature: $($sig.Status)"}
  }
  $files += [pscustomobject]$item
}
$manifest=[ordered]@{schemaVersion='2';product='Caishen IME';version=$Version;signingPolicy=$SigningPolicy;createdUtc=(Get-Date).ToUniversalTime().ToString('O');files=$files}
$manifest | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $PackageRoot 'release-manifest.json') -Encoding UTF8
Write-Host "release manifest created: $PackageRoot\release-manifest.json"
