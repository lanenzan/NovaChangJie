<#
.SYNOPSIS
  Builds NovaChangJie for Windows and stages a package in .\dist.

.EXAMPLE
  .\tools\fetch-data.ps1
  .\build.ps1                      # x64 + x86 (32-bit apps need the x86 DLL)
  .\build.ps1 -Arch arm64,x64,x86  # Windows on ARM: native + emulated apps
  .\build.ps1 -Msi                 # also build dist\NovaChangJie-<ver>-x64.msi

.NOTES
  Needs Visual Studio 2022 or later with "Desktop development with C++" and
  a Windows 11 SDK. -Msi also needs the .NET SDK (WiX Toolset 5 is restored
  from NuGet). Building does not install or register anything.
#>
[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug', 'RelWithDebInfo')]
    [string]$Config = 'Release',
    [ValidateSet('x64', 'x86', 'arm64')]
    [string[]]$Arch = @('x64', 'x86'),
    [string]$Dist,
    [switch]$Msi,
    [switch]$NoModernSettings,  # Skip the WinUI settings app (classic settings only).
    [string]$Version = '0.9.2'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $Dist) { $Dist = Join-Path $root 'dist' }

function Find-CMake {
    $cmd = Get-Command cmake -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path $vswhere) {
        $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if ($vs) {
            $candidate = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
            if (Test-Path $candidate) { return $candidate }
        }
    }
    throw 'CMake not found. Install Visual Studio with C++ CMake tools, or CMake 3.21+.'
}

$cmake = Find-CMake
$platform = @{ x64 = 'x64'; x86 = 'Win32'; arm64 = 'ARM64' }

if (Test-Path $Dist) { Remove-Item $Dist -Recurse -Force }
$first = $true
foreach ($a in $Arch) {
    $buildDir = Join-Path $root "build\$a"
    # The first architecture also builds the dictionary and settings app.
    $shared = if ($first) { 'ON' } else { 'OFF' }
    & $cmake -S $root -B $buildDir -A $platform[$a] `
        "-DNOVA_INSTALL_SHARED_FILES=$shared" "-DNOVA_BUILD_DICT=$shared"
    if ($LASTEXITCODE) { throw "configure failed ($a)" }
    & $cmake --build $buildDir --config $Config --parallel
    if ($LASTEXITCODE) { throw "build failed ($a)" }
    & $cmake --install $buildDir --config $Config --prefix $Dist
    if ($LASTEXITCODE) { throw "install-to-dist failed ($a)" }
    $first = $false
}

# Settings app (WinUI 3, C++/WinRT; x64). It links the x64 CMake libraries
# built above and carries the Windows App SDK runtime (dist\ui).
if ($Arch -contains 'x64' -and -not $NoModernSettings) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    $msbuild = & $vswhere -latest -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\amd64\MSBuild.exe' | Select-Object -First 1
    if (-not $msbuild) { throw 'MSBuild not found.' }
    $uiProject = Join-Path $root 'src\settings-ui\NovaChangJieSettingsUI.vcxproj'
    $uiBuild = Join-Path $root 'build\settings-ui'
    # packages.config restore puts packages in $(SolutionDir)packages.
    & $msbuild $uiProject -t:restore -p:RestorePackagesConfig=true "-p:SolutionDir=$uiBuild\" -v:m -nologo
    if ($LASTEXITCODE) { throw 'settings app: NuGet restore failed' }
    & $msbuild $uiProject -p:Configuration=Release -p:Platform=x64 "-p:NovaVersion=$Version" "-p:NovaLibDir=$root\build\x64\$Config\" -m -v:m -nologo
    if ($LASTEXITCODE) { throw 'settings app: build failed' }
    $uiDist = Join-Path $Dist 'ui'
    New-Item -ItemType Directory -Force -Path $uiDist | Out-Null
    # Only what runs: no debug symbols, import libraries or metadata.
    robocopy (Join-Path $uiBuild 'bin') $uiDist /E /NFL /NDL /NJH /NJS /NP /XF *.pdb *.lib *.exp *.winmd *.ilk | Out-Null
    if ($LASTEXITCODE -ge 8) { throw 'settings app: staging failed' }
    $global:LASTEXITCODE = 0
    # WinUI's own strings: English (the fallback) and Chinese are enough for
    # a Chinese-only app.
    $keep = 'en-us', 'zh-TW', 'zh-HK', 'zh-CN'
    Get-ChildItem $uiDist -Directory |
        Where-Object { $_.Name -match '^[a-z]{2,3}(-[A-Za-z0-9]+)+$' -and $keep -notcontains $_.Name } |
        Remove-Item -Recurse -Force
}

if (-not (Test-Path (Join-Path $Dist 'nova.dict'))) {
    Write-Warning 'dist\nova.dict is missing. Run tools\fetch-data.ps1 and build again.'
}
Write-Host "Package staged in $Dist"

if ($Msi) {
    foreach ($need in 'x64\NovaChangJie.dll', 'x86\NovaChangJie.dll', 'nova.dict', 'NovaChangJieSettings.exe') {
        if (-not (Test-Path (Join-Path $Dist $need))) { throw "-Msi needs dist\$need (build with -Arch x64,x86 after fetch-data)" }
    }
    $wixproj = Join-Path $root 'installer\msi\NovaChangJie.wixproj'
    $msiOut = Join-Path $root 'build\msi'
    & dotnet build $wixproj --no-incremental -c Release "-p:StageDir=$Dist" "-p:ProductVersion=$Version" -o $msiOut
    if ($LASTEXITCODE) {
        # Compressing the cabinet sometimes fails on a file briefly locked by
        # a sync client or virus scanner; one clean retry gets past it.
        Write-Warning 'MSI build failed; retrying once from a clean state.'
        Remove-Item (Join-Path $root 'installer\msi\obj') -Recurse -Force -ErrorAction SilentlyContinue
        & dotnet build $wixproj --no-incremental -c Release "-p:StageDir=$Dist" "-p:ProductVersion=$Version" -o $msiOut
        if ($LASTEXITCODE) { throw 'MSI build failed' }
    }
    Copy-Item (Join-Path $msiOut "NovaChangJie-$Version-x64.msi") $Dist -Force
    Write-Host "Installer: $(Join-Path $Dist "NovaChangJie-$Version-x64.msi")"
}
Write-Host 'To run the unit tests:  ctest --test-dir build\<arch> -C' $Config
