[CmdletBinding()]
param([string]$BuildDir='build-release',[switch]$RequireRegisteredProfile)
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$exe=Join-Path $root "$BuildDir\tsf_e2e_host.exe";$dll=Join-Path $root "$BuildDir\ShuruIme.dll"
if(-not [Environment]::UserInteractive){Write-Host 'SKIP: TSF local E2E requires an interactive Windows desktop';exit 77}
if(-not(Test-Path $exe)-or-not(Test-Path $dll)){throw 'Build artifacts missing; run scripts\build.ps1 first'}
if($RequireRegisteredProfile){
 $key='Registry::HKEY_CLASSES_ROOT\CLSID\{7C4E9F2A-1B3D-4A8E-9F6C-2D5E8B1A4C7F}\InprocServer32'
 if(-not(Test-Path $key)){Write-Host 'SKIP: profile/COM registration intentionally not present';exit 77}
 $registeredDll=(Get-ItemProperty -LiteralPath $key).'(default)'
 if(-not(Test-Path -LiteralPath $registeredDll -PathType Leaf)){throw "Registered DLL missing: $registeredDll"}
 $dll=$registeredDll
 $lexicon=Join-Path $root 'data\lexicon'
 $dataCurrent=Join-Path $env:ProgramData 'CaishenPinyin\data\lexicon\current'
 if(Test-Path -LiteralPath $dataCurrent){
  $dataVersion=(Get-Content -LiteralPath $dataCurrent -Raw).Trim()
  $deployedLexicon=Join-Path $env:ProgramData "CaishenPinyin\data\lexicon\versions\$dataVersion"
  if(Test-Path -LiteralPath $deployedLexicon -PathType Container){$lexicon=$deployedLexicon}
 }
 & (Join-Path $root "$BuildDir\release_health_check.exe") $registeredDll $lexicon --registered
 if($LASTEXITCODE-ne 0){exit $LASTEXITCODE}
}
& $exe $dll
exit $LASTEXITCODE
