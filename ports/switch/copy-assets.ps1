<#
.SYNOPSIS
  Copies the Oblivion game files (the repo's assets/ folder) to <target>/oblivion/assets/ for the Switch port.

.PARAMETER Source
  Folder holding default.xex, Oblivion.ini, data, Data2 (default: the repo's assets/ folder).

.PARAMETER Target
  SD card drive letter ("E" or "E:") or any folder that will be the SD card root.

.EXAMPLE
  .\copy-assets.ps1 E:

.NOTES
  ~5.9 GB, 166 files (largest 1.3 GB, so FAT32 is fine). Folder/file name case is preserved from the source
  (default.xex, Oblivion.ini, data, Data2). Re-running only copies what changed.
#>
param([Parameter(Mandatory = $true)][string]$Target, [string]$Source = '')

$ErrorActionPreference = 'Stop'
if (-not $Source) { $Source = Join-Path $PSScriptRoot '..\..\assets' }  # repo layout
$src = Resolve-Path $Source | Select-Object -ExpandProperty Path

if ($Target -match '^[A-Za-z]:?$') { $Target = $Target.Substring(0, 1) + ':\' }
if (-not (Test-Path $Target)) { throw "Target '$Target' does not exist" }
if (-not (Test-Path (Join-Path $src 'default.xex'))) { throw "default.xex not found in $src" }

$dst = Join-Path $Target 'oblivion\assets'
New-Item -ItemType Directory -Force -Path $dst | Out-Null
Write-Host "Copying $src -> $dst (about 5.9 GB)"

# /E subfolders, /R:2 /W:2 limited retries, /J unbuffered for big files is skipped (slower on SD readers).
robocopy $src $dst /E /R:2 /W:2 /NP /NJH /XD .git /XF .gitattributes .gitignore
# robocopy exit codes 0-7 mean success (bit 8+ = failure)
if ($LASTEXITCODE -ge 8) { throw "robocopy failed with exit code $LASTEXITCODE" }

$n = (Get-ChildItem -Recurse -File $dst | Measure-Object).Count
Write-Host "Done: $n files in $dst (expected 166)."
exit 0
