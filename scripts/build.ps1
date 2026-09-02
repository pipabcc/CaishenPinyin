[CmdletBinding()]
param(
  [ValidateSet('Debug','Release')][string]$Config='Release',
  [string]$BuildDir='build-release',
  [string]$X86BuildDir='build-release-x86',
  [string]$OutputDir='artifacts\release',
  [string]$GrammarPath='',
  [ValidateSet('Off','IfPresent','Required')][string]$SigningPolicy='Off',
  [switch]$NoPackage
)
$ErrorActionPreference='Stop'
$Root=Split-Path -Parent $PSScriptRoot;$ToolsRoot=Join-Path $Root 'tools';Set-Location $Root
Stop-Process -Name ShuruSettings -Force -ErrorAction SilentlyContinue
$localEnv=Join-Path $ToolsRoot 'env.ps1'
if(Test-Path -LiteralPath $localEnv){. $localEnv}
if(-not(Get-Command cmake -ErrorAction SilentlyContinue)){throw 'cmake not found'}
if(-not(Get-Command ctest -ErrorAction SilentlyContinue)){throw 'ctest not found'}
if(-not(Get-Command dotnet -ErrorAction SilentlyContinue)){throw 'dotnet not found'}
if(-not(Get-Command python -ErrorAction SilentlyContinue)){throw 'python not found'}
$dev=Join-Path $ToolsRoot 'vs2022\Common7\Tools\VsDevCmd.bat'
if(-not(Test-Path -LiteralPath $dev)){
  $vswhere=Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
  if(Test-Path -LiteralPath $vswhere){
    $vsPath=& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if($vsPath){$dev=Join-Path $vsPath 'Common7\Tools\VsDevCmd.bat'}
  }
}
if(-not(Test-Path -LiteralPath $dev) -and $env:VSINSTALLDIR){
  $candidate=Join-Path $env:VSINSTALLDIR 'Common7\Tools\VsDevCmd.bat'
  if(Test-Path -LiteralPath $candidate){$dev=$candidate}
}
if(-not(Test-Path -LiteralPath $dev)){throw 'VS C++ toolchain missing (VsDevCmd.bat not found)'}
$build=Join-Path $Root $BuildDir;$x86Build=Join-Path $Root $X86BuildDir;$out=Join-Path $Root $OutputDir
$grammarTarget=Join-Path $Root 'data\lexicon\rime-moqi-zh.gram'
$grammarHash='35993085E9CE5D9722050BD548B807572EDCDD784ABF8079152091F8CD9BC731'
if($GrammarPath){
 $GrammarPath=(Resolve-Path -LiteralPath $GrammarPath).Path
 if((Get-FileHash -LiteralPath $GrammarPath -Algorithm SHA256).Hash-ne$grammarHash){throw 'full Moqi Grammar SHA-256 mismatch'}
 if([IO.Path]::GetFullPath($GrammarPath)-ne[IO.Path]::GetFullPath($grammarTarget)){
  Copy-Item -LiteralPath $GrammarPath -Destination $grammarTarget -Force
 }
}
python (Join-Path $PSScriptRoot 'lexicon_manifest.py') validate --dir (Join-Path $Root 'data\lexicon') --manifest (Join-Path $Root 'data\lexicon\manifest.json')
if($LASTEXITCODE-ne 0){throw 'lexicon manifest validation failed'}
$versionHeader=Get-Content -LiteralPath (Join-Path $Root 'src\common\version.h') -Raw
if($versionHeader-notmatch '#define\s+SHURU_VERSION_STRING\s+"([^"]+)"'){throw 'version string missing'}
$productVersion=$Matches[1]
$ninja=Get-Command ninja -ErrorAction SilentlyContinue
if($ninja){
 $configure="cmake -S `"$Root`" -B `"$build`" -G Ninja -DCMAKE_BUILD_TYPE=$Config -DBUILD_TESTING=ON -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl"
 $compile="cmake --build `"$build`" --config $Config --clean-first"
 $x86Configure="cmake -S `"$Root`" -B `"$x86Build`" -G Ninja -DCMAKE_BUILD_TYPE=$Config -DBUILD_TESTING=ON -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl"
 $x86Compile="cmake --build `"$x86Build`" --config $Config --clean-first"
 $dll=Join-Path $build 'ShuruIme.dll'
 $x86Dll=Join-Path $x86Build 'ShuruIme32.dll'
}else{
 $configure="cmake -S `"$Root`" -B `"$build`" -G `"Visual Studio 17 2022`" -A x64 -DBUILD_TESTING=ON"
 $compile="cmake --build `"$build`" --config $Config --clean-first"
 $x86Configure="cmake -S `"$Root`" -B `"$x86Build`" -G `"Visual Studio 17 2022`" -A Win32 -DBUILD_TESTING=ON"
 $x86Compile="cmake --build `"$x86Build`" --config $Config --clean-first"
 $dll=Join-Path $build "$Config\ShuruIme.dll"
 $x86Dll=Join-Path $x86Build "$Config\ShuruIme32.dll"
}
$settingsProject=Join-Path $Root 'settings\ShuruSettings.csproj'
$settingsOutput=Join-Path $Root "settings\bin\$Config\net8.0-windows\win-x64\publish"
if(Test-Path -LiteralPath $settingsOutput){Remove-Item -LiteralPath $settingsOutput -Recurse -Force}
$ctestFilters = @()
$nonInteractiveBuild = (
    $env:ANTIGRAVITY_AGENT -eq '1' -or
    -not [System.Environment]::UserInteractive
)
if ($env:CAISHEN_SKIP_INTERACTIVE_TESTS -eq '1' -or $nonInteractiveBuild) {
    $ctestFilters += '-LE interactive-tsf'
}
if ($nonInteractiveBuild) {
    $ctestFilters += '-E p1_engine'
}
$excludeTests = $ctestFilters -join ' '
$bat=Join-Path $env:TEMP 'facai-release-build.cmd';@"
@echo on
call "$dev" -arch=amd64 -host_arch=amd64 || exit /b 1
dotnet build "$settingsProject" --configuration $Config -p:Version=$productVersion -p:FileVersion=$productVersion -p:AssemblyVersion=$productVersion.0 || exit /b 1
$configure || exit /b 1
$compile || exit /b 1
ctest --test-dir "$build" -C $Config $excludeTests --output-on-failure || exit /b 1
call "$dev" -arch=x86 -host_arch=amd64 || exit /b 1
$x86Configure || exit /b 1
$x86Compile || exit /b 1
ctest --test-dir "$x86Build" -C $Config -R "input_policy|engine_snapshot|release_health|tsf_e2e_core" --output-on-failure || exit /b 1
rem settings_ui_smoke uses dotnet run and rebuilds with the project defaults.
rem Publish the final settings application with its own Windows desktop runtime.
call "$dev" -arch=amd64 -host_arch=amd64 || exit /b 1
dotnet publish "$settingsProject" --configuration $Config --runtime win-x64 --self-contained true --output "$settingsOutput" -p:PublishSingleFile=false -p:PublishTrimmed=false -p:DebugType=None -p:DebugSymbols=false -p:Version=$productVersion -p:FileVersion=$productVersion -p:AssemblyVersion=$productVersion.0 || exit /b 1
"@ | Set-Content -LiteralPath $bat -Encoding ASCII
cmd /c "`"$bat`"";if($LASTEXITCODE-ne 0){throw "configure/build/full CTest failed: $LASTEXITCODE"}
if(-not(Test-Path $dll)){throw "expected x64 DLL missing: $dll"}
if(-not(Test-Path $x86Dll)){throw "expected x86 DLL missing: $x86Dll"}
if($ninja){$snapshotTool=Join-Path $build 'engine_snapshot_build_tool.exe'}else{$snapshotTool=Join-Path $build "$Config\engine_snapshot_build_tool.exe"}
if(-not(Test-Path -LiteralPath $snapshotTool)){throw "expected snapshot build tool missing: $snapshotTool"}
$builtVersion=(Get-Item -LiteralPath $dll).VersionInfo.FileVersion
if($builtVersion-ne$productVersion){throw "DLL file version $builtVersion does not match source version $productVersion"}
$x86BuiltVersion=(Get-Item -LiteralPath $x86Dll).VersionInfo.FileVersion
if($x86BuiltVersion-ne$productVersion){throw "x86 DLL file version $x86BuiltVersion does not match source version $productVersion"}
function Get-PeMachine([string]$Path){
 $bytes=[IO.File]::ReadAllBytes($Path)
 if($bytes.Length-lt 0x40 -or [BitConverter]::ToUInt16($bytes,0)-ne 0x5A4D){throw "invalid PE DOS signature: $Path"}
 $peOffset=[BitConverter]::ToInt32($bytes,0x3c)
 if($peOffset-lt 0 -or $peOffset+6-gt $bytes.Length){throw "invalid PE header offset: $Path"}
 if([BitConverter]::ToUInt32($bytes,$peOffset)-ne 0x00004550){throw "invalid PE signature: $Path"}
 return [BitConverter]::ToUInt16($bytes,$peOffset+4)
}
if((Get-PeMachine $dll)-ne 0x8664){throw "ShuruIme.dll is not x64"}
if((Get-PeMachine $x86Dll)-ne 0x014c){throw "ShuruIme32.dll is not x86"}
$settingsRequiredFiles=@('ShuruSettings.exe','ShuruSettings.dll','ShuruSettings.deps.json','ShuruSettings.runtimeconfig.json')
foreach($name in $settingsRequiredFiles){if(-not(Test-Path -LiteralPath (Join-Path $settingsOutput $name) -PathType Leaf)){throw "expected settings file missing: $name"}}
$settingsVersion=(Get-Item -LiteralPath (Join-Path $settingsOutput 'ShuruSettings.dll')).VersionInfo.FileVersion
if(([Version]$settingsVersion).ToString(3)-ne$productVersion){throw "settings file version $settingsVersion does not match source version $productVersion"}
if(-not $NoPackage){
 if(Test-Path $out){Remove-Item $out -Recurse -Force};New-Item -ItemType Directory -Force -Path (Join-Path $out 'data\lexicon')|Out-Null
 Copy-Item $dll (Join-Path $out 'ShuruIme.dll')
 Copy-Item $x86Dll (Join-Path $out 'ShuruIme32.dll')
 Copy-Item -LiteralPath $snapshotTool (Join-Path $out 'engine_snapshot_build_tool.exe')
 Get-ChildItem -LiteralPath $settingsOutput -Recurse -File | ForEach-Object {
  $relative=$_.FullName.Substring($settingsOutput.Length).TrimStart('\')
  $destination=Join-Path $out $relative
  New-Item -ItemType Directory -Force -Path (Split-Path -Parent $destination)|Out-Null
  Copy-Item -LiteralPath $_.FullName -Destination $destination -Force
 }
 $lexiconRoot=Join-Path $Root 'data\lexicon';$lexiconOut=Join-Path $out 'data\lexicon';$lexiconManifest=Get-Content -LiteralPath (Join-Path $lexiconRoot 'manifest.json') -Raw|ConvertFrom-Json
 foreach($file in $lexiconManifest.files){
  if($file.PSObject.Properties.Name -contains 'runtimeOptional' -and $file.runtimeOptional -eq $true){continue}
  Copy-Item -LiteralPath (Join-Path $lexiconRoot $file.path) -Destination (Join-Path $lexiconOut $file.path) -Force
 }
 Copy-Item -LiteralPath (Join-Path $lexiconRoot 'manifest.json') -Destination (Join-Path $lexiconOut 'manifest.json') -Force
 Copy-Item -LiteralPath (Join-Path $Root 'THIRD_PARTY_NOTICES.md') -Destination $out
 Copy-Item -LiteralPath (Join-Path $Root 'licenses') -Destination (Join-Path $out 'licenses') -Recurse -Force
 $dotnetRoot=Split-Path -Parent (Get-Command dotnet).Source
 foreach($notice in @('LICENSE.txt','ThirdPartyNotices.txt')){
  $source=Join-Path $dotnetRoot $notice
  if(Test-Path -LiteralPath $source -PathType Leaf){
   Copy-Item -LiteralPath $source -Destination (Join-Path $out "licenses\dotnet-$notice") -Force
  }
 }
 $skinSource=Join-Path $Root 'data\skins';$skinOutput=Join-Path $out 'data\skins'
 if(Test-Path -LiteralPath $skinSource -PathType Container){
  New-Item -ItemType Directory -Force -Path $skinOutput|Out-Null
  Get-ChildItem -LiteralPath $skinSource -Force|ForEach-Object{
   Copy-Item -LiteralPath $_.FullName -Destination $skinOutput -Recurse -Force
  }
 }
 & (Join-Path $PSScriptRoot 'new_release_manifest.ps1') -PackageRoot $out -Version $productVersion -SigningPolicy $SigningPolicy
}
Write-Host "[OK] formal $Config x64/x86 build + CTest; x64=$dll x86=$x86Dll package=$out" -ForegroundColor Green
