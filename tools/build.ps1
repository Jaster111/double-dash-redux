# Build Double Dash Redux: out\Release\DoubleDashRedux.exe.
#   powershell -ExecutionPolicy Bypass -File tools\build.ps1 [-Disc PATH] [-Recomp]
# The first build recompiles the game: main.dol is read from your disc image (-Disc, or the first *.iso / *.gcm /
# *.rvz in the project folder) into build\main.dol, and tools\recomp\recomp.py turns it into C++ in build\recomp\.
# Later builds only compile; -Recomp (or -Disc) runs the recompiler again (after changing tools\recomp\,
# runtime\hle_functions.txt or runtime\patches\).
# Needs Visual Studio 2022 or newer (Desktop development with C++), CMake 3.20+ and Python 3.8+.
param([string]$Disc, [switch]$Recomp)
$ErrorActionPreference = "Stop"
Set-Location (Split-Path $PSScriptRoot -Parent)

function Configure {
    # the newest Visual Studio on a fresh configure; an existing out\ keeps its generator
    if (Test-Path out\CMakeCache.txt) { cmake -S . -B out | Out-Null } else { cmake -S . -B out -A x64 | Out-Null }
    if ($LASTEXITCODE) { "cmake configure failed (run: cmake -S . -B out)"; exit 1 }
}
function Build([string]$target) {
    New-Item -ItemType Directory -Force out | Out-Null
    cmake --build out --config Release --target $target -- /m /v:minimal 2>&1 | Out-File -Encoding utf8 out\build.log
    $errs = Select-String -Path out\build.log -Pattern ": error|error MSB" | Select-Object -First 30
    if ($errs -or $LASTEXITCODE) { $errs | ForEach-Object { $_.Line }; "build failed, see out\build.log"; exit 1 }
}

if ($Disc -or $Recomp -or -not (Test-Path build\recomp\functable.cpp)) {
    if ($Disc -or -not ((Test-Path build\main.dol) -or (Test-Path orig\sys\main.dol))) {
        if (-not $Disc) {
            $Disc = Get-ChildItem -File -Include *.iso, *.gcm, *.rvz -Path * | Select-Object -First 1 -ExpandProperty FullName
            if (-not $Disc) {
                "No disc image found. Put your Mario Kart: Double Dash!! (Europe, GM4P01) .iso / .gcm / .rvz in the"
                "project folder, or pass it: tools\build.ps1 -Disc PATH"
                exit 1
            }
        }
        Configure
        Build dolextract
        New-Item -ItemType Directory -Force build | Out-Null
        & out\Release\dolextract.exe $Disc build\main.dol
        if ($LASTEXITCODE) { exit 1 }
    }
    "Recompiling main.dol (about a minute)..."
    python tools\recomp\recomp.py | Where-Object { $_ -like "functions:*" }
    if ($LASTEXITCODE) { "recompiler failed"; exit 1 }
}
Configure   # picks up the generated sources
"Compiling (the first build takes a few minutes)..."
Build doubledash
"Built out\Release\DoubleDashRedux.exe"
