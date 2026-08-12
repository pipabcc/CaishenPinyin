[CmdletBinding()]
param(
 [ValidateSet('Install','HealthCheck','Rollback','Cleanup')][string]$Action='Install',
 [string]$DllPath='',[string]$PackagePath='',[string]$Version='',
 [string]$InstallRoot='',[string]$DataRoot='',[string]$HealthCheckExe='',
 [ValidateSet('Off','IfPresent','Required')][string]$SigningPolicy='IfPresent',
 [ValidateSet('','AfterCopy','AfterManifest','AfterRegister','AfterPointer','HealthCheck')][string]$InjectFailure='',
 [switch]$NoRegister
)
$ErrorActionPreference='Stop';$script:ExitCode=1;$Root=Split-Path -Parent $PSScriptRoot
if(!$InstallRoot){$InstallRoot=Join-Path $env:ProgramFiles 'FacaiPinyin'};if(!$DataRoot){$DataRoot=Join-Path $env:ProgramData 'FacaiPinyin\data\lexicon'}
$LogDir=Join-Path $InstallRoot 'logs';New-Item -ItemType Directory -Force -Path $LogDir|Out-Null;$Log=Join-Path $LogDir ('deploy-{0:yyyyMMdd-HHmmss}.log'-f(Get-Date))
function Log($m){('[{0:O}] {1}'-f(Get-Date),$m)|Tee-Object -FilePath $Log -Append};function Fail($c,$m){$script:ExitCode=$c;Log "ERROR[$c] $m";throw "ERROR[$c] $m"}
function Ptr($p){if(Test-Path -LiteralPath $p){(Get-Content -LiteralPath $p -Raw).Trim()}else{''}};function SetPtr($p,$v){$t="$p.tmp.$PID";[IO.File]::WriteAllText($t,$v,[Text.Encoding]::ASCII);Move-Item $t $p -Force}
function Inject($point){if($InjectFailure-eq$point){Fail 90 "injected failure: $point"}}
function TestLexicon($p){$mp=Join-Path $p 'manifest.json';if(!(Test-Path $mp)){Fail 20 'lexicon manifest missing'};try{$m=Get-Content $mp -Raw|ConvertFrom-Json}catch{Fail 21 'lexicon manifest invalid'};foreach($f in $m.files){$x=Join-Path $p $f.path;if(!(Test-Path $x -PathType Leaf)){Fail 22 "missing $($f.path)"};if((Get-FileHash $x -Algorithm SHA256).Hash.ToLowerInvariant()-ne([string]$f.sha256).ToLowerInvariant()){Fail 23 "hash mismatch $($f.path)"};if($f.PSObject.Properties.Name-contains'size'){if((Get-Item $x).Length-ne[int64]$f.size){Fail 24 "size mismatch $($f.path)"}}};$m}
function AssertReusableLexicon($source,$installed){
 [void](TestLexicon $installed)
 $sourceManifest=Join-Path $source 'manifest.json';$installedManifest=Join-Path $installed 'manifest.json'
 $sourceHash=(Get-FileHash $sourceManifest -Algorithm SHA256).Hash
 $installedHash=(Get-FileHash $installedManifest -Algorithm SHA256).Hash
 if($sourceHash-ne$installedHash){Fail 33 'installed lexicon version conflicts with package manifest'}
 Log "reusing verified lexicon $installed"
}
function TestDll($p,$releaseManifest='') {if(!(Test-Path $p -PathType Leaf)){Fail 25 'DLL missing'};if($releaseManifest){$rm=Get-Content $releaseManifest -Raw|ConvertFrom-Json;$f=$rm.files|Where-Object { $_.path -eq 'ShuruIme.dll' }|Select-Object -First 1;if(!$f){Fail 26 'DLL absent from release manifest'};if((Get-FileHash $p -Algorithm SHA256).Hash.ToLowerInvariant()-ne([string]$f.sha256).ToLowerInvariant()){Fail 27 'DLL hash mismatch'};if((Get-Item $p).Length-ne[int64]$f.size){Fail 28 'DLL size mismatch'}};$s=Get-AuthenticodeSignature $p;if($SigningPolicy-eq'Required'-and$s.Status-ne'Valid'){Fail 29 "signature required: $($s.Status)"};if($SigningPolicy-eq'IfPresent'-and$s.Status-notin@('Valid','NotSigned')){Fail 29 "invalid signature: $($s.Status)"}}
function RegisterDll($p){if($NoRegister){Log 'registration skipped';return};$proc=Start-Process -FilePath "$env:SystemRoot\System32\regsvr32.exe" -ArgumentList @('/s',('"{0}"' -f $p)) -Wait -PassThru;if($proc.ExitCode -ne 0){Fail 40 "registration failed $($proc.ExitCode)"}}
function TestCurrent{$iv=Ptr(Join-Path $InstallRoot 'current');$dv=Ptr(Join-Path $DataRoot 'current');if(!$iv-or!$dv){Fail 30 'current pointer missing'};$d=Join-Path "$InstallRoot\versions\$iv" 'ShuruIme.dll';TestDll $d;[void](TestLexicon "$DataRoot\versions\$dv");if($HealthCheckExe){$args=@($d,"$DataRoot\versions\$dv");if(!$NoRegister){$args+='--registered'};& $HealthCheckExe @args;if($LASTEXITCODE-ne0){Fail 32 'real health check failed'}};Inject 'HealthCheck';Log "healthy dll=$iv lexicon=$dv"}
try{
 New-Item -ItemType Directory -Force -Path $InstallRoot,$DataRoot,"$InstallRoot\versions","$DataRoot\versions"|Out-Null
 if($Action-eq'HealthCheck'){TestCurrent;exit 0}
 if($Action-eq'Install'){
  if(!$DllPath){Fail 10 'DllPath required'};if(!$PackagePath){$PackagePath=Join-Path $Root 'data\lexicon'};$DllPath=(Resolve-Path $DllPath).Path;$PackagePath=(Resolve-Path $PackagePath).Path;$lm=TestLexicon $PackagePath;if(!$Version){$Version=[string]$lm.version};if($Version-notmatch'^[0-9A-Za-z._-]+$'){Fail 11 'invalid version'}
  $release=Join-Path(Split-Path $DllPath)'release-manifest.json';TestDll $DllPath $(if(Test-Path $release){$release}else{''})
  $oldI=Ptr(Join-Path $InstallRoot 'current');$oldD=Ptr(Join-Path $DataRoot 'current')
  $stage=Join-Path $InstallRoot ".stage-$Version-$PID";$stageD=Join-Path $DataRoot ".stage-$($lm.version)-$PID"
  $target="$InstallRoot\versions\$Version";$targetD="$DataRoot\versions\$($lm.version)"
  $reuseLexicon=Test-Path -LiteralPath $targetD -PathType Container
  if((Test-Path -LiteralPath $targetD)-and!$reuseLexicon){Fail 33 'lexicon version target is not a directory'}
  if($reuseLexicon){AssertReusableLexicon $PackagePath $targetD}
  try{
   New-Item -ItemType Directory -Force -Path $stage|Out-Null
   if(!$reuseLexicon){New-Item -ItemType Directory -Force -Path $stageD|Out-Null}
   Copy-Item $DllPath(Join-Path $stage 'ShuruIme.dll')
   if(!$reuseLexicon){foreach($f in @($lm.files)+@([pscustomobject]@{path='manifest.json'})){Copy-Item(Join-Path $PackagePath $f.path)(Join-Path $stageD $f.path)-Force}}
   Inject 'AfterCopy'
   TestDll(Join-Path $stage 'ShuruIme.dll')$(if(Test-Path $release){$release}else{''})
   if(!$reuseLexicon){[void](TestLexicon $stageD)}
   Inject 'AfterManifest'
   if(Test-Path $target){Remove-Item $target -Recurse -Force}
   Move-Item $stage $target
   if(!$reuseLexicon){Move-Item $stageD $targetD}
   RegisterDll(Join-Path $target 'ShuruIme.dll');Inject 'AfterRegister'
   SetPtr(Join-Path $InstallRoot 'current')$Version;SetPtr(Join-Path $DataRoot 'current')([string]$lm.version)
   Inject 'AfterPointer';TestCurrent
   if($oldI){SetPtr(Join-Path $InstallRoot 'previous')$oldI};if($oldD){SetPtr(Join-Path $DataRoot 'previous')$oldD}
   Log "install complete $Version";exit 0
  }catch{
   Log "transaction rollback: $($_.Exception.Message)"
   if($oldI){RegisterDll "$InstallRoot\versions\$oldI\ShuruIme.dll";SetPtr(Join-Path $InstallRoot 'current')$oldI}
   if($oldD){SetPtr(Join-Path $DataRoot 'current')$oldD}
   Remove-Item $stage,$stageD -Recurse -Force -ErrorAction SilentlyContinue
   throw
  }
 }
 if($Action-eq'Rollback'){$pi=Ptr(Join-Path $InstallRoot 'previous');$pd=Ptr(Join-Path $DataRoot 'previous');if(!$pi-or!$pd){Fail 50 'previous unavailable'};RegisterDll "$InstallRoot\versions\$pi\ShuruIme.dll";SetPtr(Join-Path $InstallRoot 'current')$pi;SetPtr(Join-Path $DataRoot 'current')$pd;TestCurrent;exit 0}
 if($Action-eq'Cleanup'){$keep=@((Ptr(Join-Path $InstallRoot 'current')),(Ptr(Join-Path $InstallRoot 'previous')));Get-ChildItem "$InstallRoot\versions" -Directory|Where-Object{$keep-notcontains$_.Name}|Remove-Item -Recurse -Force -ErrorAction SilentlyContinue;exit 0}
}catch{if(!$_.Exception.Message.StartsWith('ERROR[')){Log "FAILED $($_.Exception.Message)"};exit $script:ExitCode}
