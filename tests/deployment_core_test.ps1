$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot; $tmp=Join-Path $env:TEMP ('facai-deploy-test-'+[guid]::NewGuid())
try {
  New-Item -ItemType Directory -Force -Path $tmp | Out-Null
  $install=Join-Path $tmp 'app'; $data=Join-Path $tmp 'data'; $dummy=Join-Path $tmp 'ShuruIme.dll'; [IO.File]::WriteAllBytes($dummy,[byte[]](1,2,3,4))
  & (Join-Path $root 'scripts\install_ime.ps1') -Action Install -DllPath $dummy -PackagePath (Join-Path $root 'data\lexicon') -Version test-1 -InstallRoot $install -DataRoot $data -SigningPolicy Off -NoRegister
  if($LASTEXITCODE -ne 0){throw 'non-admin install failed'}
  & (Join-Path $root 'scripts\install_ime.ps1') -Action HealthCheck -InstallRoot $install -DataRoot $data -SigningPolicy Off -NoRegister
  if($LASTEXITCODE -ne 0){throw 'health check failed'}
  $user=Join-Path $tmp 'user_dict.txt'; Set-Content $user 'keep-me';
  $installedLexicon=Join-Path $data 'versions\1.1.0'; $sentinel=Join-Path $installedLexicon 'reuse-sentinel.txt'; Set-Content $sentinel 'keep-version-directory'
  $lock=[IO.File]::Open((Join-Path $installedLexicon 'base_dict.txt'),[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read)
  try {
    & (Join-Path $root 'scripts\install_ime.ps1') -Action Install -DllPath $dummy -PackagePath (Join-Path $root 'data\lexicon') -Version test-2 -InstallRoot $install -DataRoot $data -SigningPolicy Off -NoRegister
  } finally { $lock.Dispose() }
  if($LASTEXITCODE -ne 0){throw 'upgrade failed'}
  if((Get-Content $sentinel -Raw).Trim() -ne 'keep-version-directory'){throw 'matching lexicon version was replaced instead of reused'}
  & (Join-Path $root 'scripts\install_ime.ps1') -Action Install -DllPath $dummy -PackagePath (Join-Path $root 'data\lexicon') -Version test-3 -InstallRoot $install -DataRoot $data -SigningPolicy Off -InjectFailure AfterPointer -NoRegister
  if($LASTEXITCODE -eq 0){throw 'failure injection unexpectedly succeeded'}
  if((Get-Content (Join-Path $install 'current') -Raw).Trim() -ne 'test-2'){throw 'injected failure did not restore current DLL pointer'}
  & (Join-Path $root 'scripts\install_ime.ps1') -Action Rollback -InstallRoot $install -DataRoot $data -SigningPolicy Off -NoRegister
  if((Get-Content (Join-Path $install 'current') -Raw).Trim() -ne 'test-1'){throw 'rollback pointer failed'}
  if((Get-Content $user -Raw).Trim() -ne 'keep-me'){throw 'user dictionary overwritten'}
  Write-Host 'deployment core tests passed'
} finally { Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue }
