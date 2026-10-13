# Build (optionally) and run the game for a limited time, then print the log tail.
#   powershell -File tools\run.ps1 [-Seconds 20] [-NoBuild] [-Lines 80]
#              [-Script tools\scripts\gp_next_race.txt] [-Laps 1] [-HangExit] [-Sym]
# -Script sets DD_SCRIPT (scripted controller input), -Laps sets DD_LAPS (course lap count),
# -HangExit makes the runtime exit after reporting a hang, -Sym runs the log tail through tools\sym.py.
# Other DD_* variables already set in the environment are passed through.
param([int]$Seconds = 20, [switch]$NoBuild, [int]$Lines = 80, [string]$Script, [int]$Laps = -1, [switch]$HangExit,
      [switch]$Sym)
Set-Location (Split-Path $PSScriptRoot -Parent)
if (-not $NoBuild) {
    cmake -S . -B out -G "Visual Studio 17 2022" -A x64 | Out-Null
    cmake --build out --config Release -- /m /v:minimal 2>&1 | Out-File -Encoding utf8 out\build.log
    $errs = Select-String -Path out\build.log -Pattern ": error" | Select-Object -First 30
    if ($errs) { $errs | ForEach-Object { $_.Line }; exit 1 }
}
if ($Script) { $env:DD_SCRIPT = (Resolve-Path $Script).Path }
if ($Laps -ge 0) { $env:DD_LAPS = "$Laps" }
if ($HangExit) { $env:DD_HANG_EXIT = "1" }
Remove-Item doubledash.log -ErrorAction SilentlyContinue
$p = Start-Process -FilePath out\Release\doubledash.exe -PassThru -RedirectStandardError out\stderr.txt -RedirectStandardOutput out\stdout.txt
$null = $p.Handle   # keep the handle open so ExitCode is available afterwards
$p.WaitForExit($Seconds * 1000) | Out-Null
$code = 124
if (!$p.HasExited) { Stop-Process -Id $p.Id -Force; "== killed after $Seconds s" } else { $code = $p.ExitCode; "== exited with code $code" }
if (Test-Path doubledash.log) {
    if ($Sym) { python tools\sym.py doubledash.log | Select-Object -Last $Lines }
    else { Get-Content doubledash.log -Tail $Lines }
}
exit $code
