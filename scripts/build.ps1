[CmdletBinding()]
param(
  [ValidateSet('Debug','Release')][string]$Config='Release',
  [string]$BuildDir='build-release',
  [string]$OutputDir='artifacts\release',
  [ValidateSet('Off','IfPresent','Required')][string]$SigningPolicy='IfPresent',
  [switch]$NoPackage
)
$ErrorActionPreference='Stop'
$Root=Split-Path -Parent $PSScriptRoot;$ToolsRoot=Join-Path $Root 'tools';Set-Location $Root
. (Join-Path $ToolsRoot 'env.ps1')
if(-not(Get-Command cmake -ErrorAction SilentlyContinue)){throw 'cmake not found'}
if(-not(Get-Command ctest -ErrorAction SilentlyContinue)){throw 'ctest not found'}
if(-not(Get-Command dotnet -ErrorAction SilentlyContinue)){throw 'dotnet not found'}
$dev=Join-Path $ToolsRoot 'vs2022\Common7\Tools\VsDevCmd.bat';if(-not(Test-Path $dev)){throw 'VS C++ toolchain missing'}
$build=Join-Path $Root $BuildDir;$out=Join-Path $Root $OutputDir
$versionHeader=Get-Content -LiteralPath (Join-Path $Root 'src\common\version.h') -Raw
if($versionHeader-notmatch '#define\s+SHURU_VERSION_STRING\s+"([^"]+)"'){throw 'version string missing'}
$productVersion=$Matches[1]
$ninja=Get-Command ninja -ErrorAction SilentlyContinue
if($ninja){$configure="cmake -S `"$Root`" -B `"$build`" -G Ninja -DCMAKE_BUILD_TYPE=$Config -DBUILD_TESTING=ON -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl";$compile="cmake --build `"$build`" --config $Config";$dll=Join-Path $build 'ShuruIme.dll'}
else{$configure="cmake -S `"$Root`" -B `"$build`" -G `"Visual Studio 17 2022`" -A x64 -DBUILD_TESTING=ON";$compile="cmake --build `"$build`" --config $Config";$dll=Join-Path $build "$Config\ShuruIme.dll"}
$settingsProject=Join-Path $Root 'settings\ShuruSettings.csproj'
$settingsOutput=Join-Path $Root "settings\bin\$Config\net8.0-windows"
$bat=Join-Path $env:TEMP 'facai-release-build.cmd';@"
@echo on
call "$dev" -arch=amd64 -host_arch=amd64 || exit /b 1
dotnet build "$settingsProject" --configuration $Config || exit /b 1
$configure || exit /b 1
$compile || exit /b 1
ctest --test-dir "$build" -C $Config --output-on-failure || exit /b 1
"@ | Set-Content -LiteralPath $bat -Encoding ASCII
cmd /c "`"$bat`"";if($LASTEXITCODE-ne 0){throw "configure/build/full CTest failed: $LASTEXITCODE"}
if(-not(Test-Path $dll)){throw "expected DLL missing: $dll"}
$builtVersion=(Get-Item -LiteralPath $dll).VersionInfo.FileVersion
if($builtVersion-ne$productVersion){throw "DLL file version $builtVersion does not match source version $productVersion"}
$settingsFiles=@('ShuruSettings.exe','ShuruSettings.dll','ShuruSettings.deps.json','ShuruSettings.runtimeconfig.json')
foreach($name in $settingsFiles){if(-not(Test-Path -LiteralPath (Join-Path $settingsOutput $name) -PathType Leaf)){throw "expected settings file missing: $name"}}
if(-not $NoPackage){
 if(Test-Path $out){Remove-Item $out -Recurse -Force};New-Item -ItemType Directory -Force -Path (Join-Path $out 'data\lexicon')|Out-Null
 Copy-Item $dll (Join-Path $out 'ShuruIme.dll');foreach($name in $settingsFiles){Copy-Item (Join-Path $settingsOutput $name) (Join-Path $out $name)};Copy-Item (Join-Path $Root 'data\lexicon\*') (Join-Path $out 'data\lexicon') -Recurse -Force
 Remove-Item (Join-Path $out 'data\lexicon\user_dict.txt') -Force -ErrorAction SilentlyContinue
 & (Join-Path $PSScriptRoot 'new_release_manifest.ps1') -PackageRoot $out -Version $productVersion -SigningPolicy $SigningPolicy
 if($LASTEXITCODE-ne 0){throw 'release manifest generation failed'}
}
Write-Host "[OK] formal $Config build + full CTest; DLL=$dll package=$out" -ForegroundColor Green
