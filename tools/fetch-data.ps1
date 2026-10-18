<#
.SYNOPSIS
  Downloads the open data NovaChangJie compiles into nova.dict.

.DESCRIPTION
  Files are pinned to exact upstream commits and verified by SHA-256, then
  saved to data/sources. Nothing from Microsoft is used.

    table.txt  libcangjie character table (Cangjie 3 + 5 codes, Big5/HKSCS
               flags, simplified forms). Public domain.
    essay.txt  rime-essay word frequency list (words for 詞語輸入 / 聯想,
               character frequencies). LGPL-3.0.

  -Neural also downloads ONNX Runtime (MIT) to data/onnxruntime, needed to
  build the NovaChangJieLM helper. The training text and the model come from
  tools/prepare-corpus.py and tools/train-lm.py (see README).
#>
[CmdletBinding()]
param(
    [string]$Destination,
    [switch]$Neural
)
$ErrorActionPreference = 'Stop'
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $Destination) { $Destination = Join-Path $scriptDir '..\data\sources' }

$sources = @(
    @{
        Name    = 'table.txt'
        Url     = 'https://gitlab.freedesktop.org/cangjie/libcangjie/-/raw/1f95d1d56ffb328779d46baaab7431fa31349f1e/data/table.txt'
        Sha256  = '86118f2a50591b837e3317a3dab87f96e7188c54046e8ed20dd4a0933877d3ea'
        License = 'Public domain (see libcangjie data/README.table.md)'
        Home    = 'https://gitlab.freedesktop.org/cangjie/libcangjie'
    },
    @{
        Name    = 'essay.txt'
        Url     = 'https://raw.githubusercontent.com/rime/rime-essay/054920de4f54c9e5994276a96a4fc2a35cb51aa3/essay.txt'
        Sha256  = '3e8512ebaa6657e35961b1f5762060717c647e3bf471d5e3b86e783b4e934ffb'
        License = 'LGPL-3.0 (https://github.com/rime/rime-essay/blob/master/LICENSE)'
        Home    = 'https://github.com/rime/rime-essay'
    }
)

New-Item -ItemType Directory -Force -Path $Destination | Out-Null
$Destination = (Resolve-Path $Destination).Path

$notes = @('# NovaChangJie data sources', '', 'nova.dict is compiled from the following files:', '')
foreach ($s in $sources) {
    $target = Join-Path $Destination $s.Name
    $ok = (Test-Path $target) -and ((Get-FileHash $target -Algorithm SHA256).Hash -eq $s.Sha256)
    if (-not $ok) {
        Write-Host "Downloading $($s.Name) ..."
        $tmp = "$target.download"
        # curl.exe ships with Windows 10+. Some hosts answer PowerShell's own
        # web client with a bot-check page; the checksum below catches that.
        $curl = Get-Command curl.exe -ErrorAction SilentlyContinue
        if ($curl) {
            & $curl.Source -fsSL --retry 3 -o $tmp $s.Url
            if ($LASTEXITCODE) { throw "Download failed for $($s.Name) (curl exit code $LASTEXITCODE)" }
        } else {
            Invoke-WebRequest -Uri $s.Url -OutFile $tmp -UseBasicParsing -UserAgent 'curl/8.0'
        }
        $hash = (Get-FileHash $tmp -Algorithm SHA256).Hash
        if ($hash -ne $s.Sha256) {
            Remove-Item $tmp -Force
            throw "Checksum mismatch for $($s.Name): expected $($s.Sha256), got $hash"
        }
        Move-Item $tmp $target -Force
    } else {
        Write-Host "$($s.Name) is up to date."
    }
    $notes += "- **$($s.Name)** - $($s.Home)"
    $notes += "  - URL: $($s.Url)"
    $notes += "  - SHA-256: ``$($s.Sha256)``"
    $notes += "  - License: $($s.License)"
}
$notes += ''
$notes += 'If you redistribute nova.dict, include this file and comply with the licenses above.'
Set-Content -Path (Join-Path $Destination 'SOURCES.md') -Value $notes -Encoding UTF8
Write-Host "Data ready in $Destination"

if ($Neural) {
    $ort = @{
        Name   = 'onnxruntime-win-x64-1.30.0'
        Url    = 'https://github.com/microsoft/onnxruntime/releases/download/v1.30.0/onnxruntime-win-x64-1.30.0.zip'
        Sha256 = 'c6ba983baf5681af108599675d2a89c2d145512d02de28aed0bff177cd0ba949'
    }
    $ortRoot = Join-Path $scriptDir '..\data\onnxruntime'
    New-Item -ItemType Directory -Force -Path $ortRoot | Out-Null
    $ortRoot = (Resolve-Path $ortRoot).Path
    if (Test-Path (Join-Path $ortRoot "$($ort.Name)\include\onnxruntime_cxx_api.h")) {
        Write-Host "$($ort.Name) is up to date."
    } else {
        $zip = Join-Path $ortRoot "$($ort.Name).zip"
        if (-not ((Test-Path $zip) -and ((Get-FileHash $zip -Algorithm SHA256).Hash -eq $ort.Sha256))) {
            Write-Host "Downloading $($ort.Name) ..."
            & curl.exe -fsSL --retry 3 -o $zip $ort.Url
            if ($LASTEXITCODE) { throw "Download failed for $($ort.Name)" }
            if ((Get-FileHash $zip -Algorithm SHA256).Hash -ne $ort.Sha256) {
                Remove-Item $zip -Force
                throw "Checksum mismatch for $($ort.Name)"
            }
        }
        Expand-Archive -Path $zip -DestinationPath $ortRoot -Force
        # Debug symbols are not needed (and are 420 MB).
        Get-ChildItem (Join-Path $ortRoot $ort.Name) -Recurse -Filter *.pdb | Remove-Item -Force
    }
    Write-Host "ONNX Runtime ready in $ortRoot"
}
