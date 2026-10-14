# Build the release package: dist\DoubleDash\ (doubledash.exe + README.txt) and dist\DoubleDash-<commit>.zip.
#   powershell -File tools\package.ps1 [-NoBuild] [-Pdb]
# -Pdb also packs doubledash.pdb (function names in crash logs and for the profiler).
# The package contains no game data; the user supplies the disc image (see tools\dist\README.txt).
param([switch]$NoBuild, [switch]$Pdb)
$ErrorActionPreference = "Stop"
Set-Location (Split-Path $PSScriptRoot -Parent)
if (-not $NoBuild) {
    cmake -S . -B out -G "Visual Studio 17 2022" -A x64 | Out-Null
    cmake --build out --config Release -- /m /v:minimal 2>&1 | Out-File -Encoding utf8 out\build.log
    $errs = Select-String -Path out\build.log -Pattern ": error" | Select-Object -First 30
    if ($errs) { $errs | ForEach-Object { $_.Line }; exit 1 }
}
$dir = "dist\DoubleDash"
if (Test-Path $dir) { Remove-Item -Recurse -Force $dir }
New-Item -ItemType Directory -Force $dir | Out-Null
Copy-Item out\Release\doubledash.exe $dir
if ($Pdb) { Copy-Item out\Release\doubledash.pdb $dir }
Copy-Item tools\dist\README.txt $dir
$rev = (git rev-parse --short HEAD).Trim()
if (git status --porcelain --untracked-files=no) { $rev += "-dirty" }
$zip = "dist\DoubleDash-$rev.zip"
if (Test-Path $zip) { Remove-Item $zip }
Compress-Archive -Path $dir -DestinationPath $zip
"$zip  ($([math]::Round((Get-Item $zip).Length / 1MB, 1)) MB)"
Get-ChildItem $dir | ForEach-Object { "  $($_.Name)  $([math]::Round($_.Length / 1MB, 1)) MB" }
