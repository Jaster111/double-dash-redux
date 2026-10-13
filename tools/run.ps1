# Build (optionally) and run the game for a limited time, then print the log tail.
#   powershell -File tools\run.ps1 [-Seconds 20] [-NoBuild] [-Lines 80]
#              [-Script tools\scripts\gp_next_race.txt] [-Laps 1] [-HangExit] [-Sym] [-Card FILE [-NewCard]]
# -Script sets DD_SCRIPT (scripted controller input), -Laps sets DD_LAPS (course lap count),
# -HangExit makes the runtime exit after reporting a hang, -Sym runs the log tail through tools\sym.py.
# -Card sets DD_MEMCARD_A (slot A image; -NewCard deletes it first so the game starts with a blank card).
# With -Script and no -Card, slot A is empty (DD_MEMCARD_A=none) so menu timings do not depend on a save.
# With -Script the game runs in 50 Hz mode (DD_60HZ=0: no 50/60 Hz prompt, the scripts' menu timings) unless
# -Hz60 is given or DD_60HZ is already set.
# Other DD_* variables already set in the environment are passed through.
param([int]$Seconds = 20, [switch]$NoBuild, [int]$Lines = 80, [string]$Script, [int]$Laps = -1, [switch]$HangExit,
      [switch]$Sym, [string]$Card, [switch]$NewCard, [switch]$Hz60)
Set-Location (Split-Path $PSScriptRoot -Parent)
if (-not $NoBuild) {
    cmake -S . -B out -G "Visual Studio 17 2022" -A x64 | Out-Null
    cmake --build out --config Release -- /m /v:minimal 2>&1 | Out-File -Encoding utf8 out\build.log
    $errs = Select-String -Path out\build.log -Pattern ": error" | Select-Object -First 30
    if ($errs) { $errs | ForEach-Object { $_.Line }; exit 1 }
}
if ($Script) { $env:DD_SCRIPT = (Resolve-Path $Script).Path }
if ($Card) {
    if ($NewCard) { Remove-Item $Card -ErrorAction SilentlyContinue }
    $env:DD_MEMCARD_A = $Card
} elseif ($Script -and -not $env:DD_MEMCARD_A) { $env:DD_MEMCARD_A = "none" }
if ($Hz60) { $env:DD_60HZ = "1" } elseif ($Script -and -not $env:DD_60HZ) { $env:DD_60HZ = "0" }
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
