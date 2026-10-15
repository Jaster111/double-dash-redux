# Build the release package: dist\DoubleDashRedux\ (DoubleDashRedux.exe, README.txt, licences) and
# dist\DoubleDashRedux-<commit>.zip.
#   powershell -File tools\package.ps1 [-NoBuild] [-Pdb]
# -Pdb also packs DoubleDashRedux.pdb (function names in crash logs and for the profiler).
# The package contains no game data; the user supplies the disc image (see tools\dist\README.txt).
param([switch]$NoBuild, [switch]$Pdb)
$ErrorActionPreference = "Stop"
Set-Location (Split-Path $PSScriptRoot -Parent)
if (-not $NoBuild) {
    powershell -ExecutionPolicy Bypass -File tools\build.ps1 | Out-Host
    if ($LASTEXITCODE) { exit 1 }
}
$dir = "dist\DoubleDashRedux"
if (Test-Path $dir) { Remove-Item -Recurse -Force $dir }
New-Item -ItemType Directory -Force $dir | Out-Null
Copy-Item out\Release\DoubleDashRedux.exe $dir
if ($Pdb) { Copy-Item out\Release\DoubleDashRedux.pdb $dir }
Copy-Item tools\dist\README.txt $dir
Copy-Item LICENSE (Join-Path $dir "LICENSE.txt")                           # GPL-2.0-or-later
Copy-Item third_party\zstd\LICENSE (Join-Path $dir "LICENSE-zstd.txt")   # BSD: the notice ships with the binary
$rev = (git rev-parse --short HEAD).Trim()
if (git status --porcelain --untracked-files=no) { $rev += "-dirty" }
$zip = "dist\DoubleDashRedux-$rev.zip"
if (Test-Path $zip) { Remove-Item $zip }
Compress-Archive -Path $dir -DestinationPath $zip
"$zip  ($([math]::Round((Get-Item $zip).Length / 1MB, 1)) MB)"
Get-ChildItem $dir | ForEach-Object { "  $($_.Name)  $([math]::Round($_.Length / 1MB, 1)) MB" }
