<#
.SYNOPSIS
  Installs NovaChangJie from this folder (a package staged by build.ps1).

.DESCRIPTION
  Run from an elevated PowerShell:
      Set-ExecutionPolicy -Scope Process Bypass
      .\install.ps1

  Copies the files to %ProgramFiles%\NovaChangJie and registers the text
  service for every architecture present (x64, x86, arm64). Afterwards add
  the keyboard in Settings > Time & language > Language & region >
  中文(台灣) or 中文(香港特別行政區) > Language options > Add a keyboard.
#>
[CmdletBinding()]
param(
    [string]$Target = (Join-Path $env:ProgramFiles 'NovaChangJie')
)
$ErrorActionPreference = 'Stop'

$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Please run install.ps1 from an elevated (Administrator) PowerShell.'
}

$source = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not (Test-Path (Join-Path $source 'nova.dict'))) { throw "nova.dict not found in $source" }

$regsvr = @{
    x64   = Join-Path $env:WINDIR 'System32\regsvr32.exe'
    arm64 = Join-Path $env:WINDIR 'System32\regsvr32.exe'
    x86   = Join-Path $env:WINDIR 'SysWOW64\regsvr32.exe'
}
$nativeArch = if ($env:PROCESSOR_ARCHITECTURE -eq 'ARM64') { 'arm64' } else { 'x64' }

# Unregister an older copy first so its DLLs can be replaced.
foreach ($a in 'x64', 'x86', 'arm64') {
    $old = Join-Path $Target "$a\NovaChangJie.dll"
    if (Test-Path $old) { & $regsvr[$a] /s /u $old | Out-Null }
}

New-Item -ItemType Directory -Force -Path $Target | Out-Null
foreach ($item in Get-ChildItem $source) {
    try {
        Copy-Item $item.FullName -Destination $Target -Recurse -Force
    } catch {
        throw "Could not copy $($item.Name). If NovaChangJie is in use, sign out and in again, then retry. $_"
    }
}

$registered = @()
foreach ($a in 'x64', 'x86', 'arm64') {
    $dll = Join-Path $Target "$a\NovaChangJie.dll"
    if (-not (Test-Path $dll)) { continue }
    if ($a -eq 'x64' -and $nativeArch -eq 'arm64') {
        # x64 DLL on Windows on ARM serves emulated x64 apps.
        $exe = Join-Path $env:WINDIR 'System32\regsvr32.exe'
    } elseif ($a -eq 'arm64' -and $nativeArch -ne 'arm64') {
        continue
    } else {
        $exe = $regsvr[$a]
    }
    $p = Start-Process -FilePath $exe -ArgumentList '/s', "`"$dll`"" -Wait -PassThru
    if ($p.ExitCode -ne 0) { throw "regsvr32 failed for $dll (exit code $($p.ExitCode))" }
    $registered += $a
}
if (-not $registered) { throw 'No NovaChangJie.dll was found to register.' }

Write-Host "NovaChangJie installed to $Target (registered: $($registered -join ', '))."
Write-Host 'Add it under Settings > Time & language > Language & region > 中文 > Language options > Add a keyboard.'
