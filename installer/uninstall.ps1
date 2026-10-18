<#
.SYNOPSIS
  Unregisters NovaChangJie and removes its files. Run elevated.
  Your learning data and user words in %APPDATA%\NovaChangJie are kept
  unless -RemoveUserData is given.
#>
[CmdletBinding()]
param(
    [string]$Target = (Join-Path $env:ProgramFiles 'NovaChangJie'),
    [switch]$RemoveUserData
)
$ErrorActionPreference = 'Stop'

$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Please run uninstall.ps1 from an elevated (Administrator) PowerShell.'
}

$regsvr = @{
    x64   = Join-Path $env:WINDIR 'System32\regsvr32.exe'
    arm64 = Join-Path $env:WINDIR 'System32\regsvr32.exe'
    x86   = Join-Path $env:WINDIR 'SysWOW64\regsvr32.exe'
}
foreach ($a in 'x64', 'x86', 'arm64') {
    $dll = Join-Path $Target "$a\NovaChangJie.dll"
    if (Test-Path $dll) {
        Start-Process -FilePath $regsvr[$a] -ArgumentList '/s', '/u', "`"$dll`"" -Wait | Out-Null
    }
}

if (Test-Path $Target) {
    try {
        Remove-Item $Target -Recurse -Force
    } catch {
        Write-Warning "Some files are still in use. Sign out (or restart), then delete $Target."
    }
}
if ($RemoveUserData) {
    $data = Join-Path $env:APPDATA 'NovaChangJie'
    if (Test-Path $data) { Remove-Item $data -Recurse -Force }
    Remove-Item 'HKCU:\Software\NovaChangJie' -Recurse -Force -ErrorAction SilentlyContinue
}
Write-Host 'NovaChangJie has been unregistered.'
