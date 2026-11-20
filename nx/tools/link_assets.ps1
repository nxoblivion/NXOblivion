# Makes nx/app/assets a junction to the extracted game (the folder holding default.xex, Data, Data2, Oblivion.ini).
# The game files never go into git (nx/.gitignore). Default: the repository's assets/ folder.
#   powershell -File nx/tools/link_assets.ps1 [-Game <folder>]
param([string]$Game = "")
$nx = Split-Path -Parent $PSScriptRoot
if (-not $Game) { $Game = Join-Path (Split-Path -Parent $nx) "assets" }
$Game = (Resolve-Path $Game).Path
if (-not (Test-Path (Join-Path $Game "default.xex"))) { throw "No default.xex in $Game" }
$link = Join-Path $nx "app\assets"
if (Test-Path $link) {
    $item = Get-Item $link -Force
    if ($item.LinkType -eq "Junction") { Write-Host "$link -> $($item.Target)"; exit 0 }
    throw "$link exists and is not a junction"
}
New-Item -ItemType Junction -Path $link -Target $Game | Out-Null
Write-Host "$link -> $Game"
