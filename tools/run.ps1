# Build (optionally) and run the game for a limited time, then print the log tail.
#   powershell -File tools\run.ps1 [-Seconds 20] [-NoBuild] [-Lines 80]
#              [-Script tools\scripts\gp_next_race.txt] [-Laps 1] [-HangExit] [-Sym] [-Card FILE [-NewCard]]
#              [-Autopilot on|win] [-Monitor N] [-Mute] [-RunDir DIR]
# -Script sets DD_SCRIPT (scripted controller input), -Laps sets DD_LAPS (course lap count),
# -HangExit makes the runtime exit after reporting a hang, -Sym runs the log tail through tools\sym.py.
# -Card sets DD_MEMCARD_A (slot A image; -NewCard deletes it first so the game starts with a blank card).
# With -Script and no -Card, slot A is empty (DD_MEMCARD_A=none) so menu timings do not depend on a save.
# With -Script the game runs in 50 Hz mode (DD_60HZ=0: no 50/60 Hz prompt, the scripts' menu timings) unless
# -Hz60 is given or DD_60HZ is already set.
# -Autopilot sets DD_AUTOPILOT (the game's AI drives the players' karts; win = also keep P1 first in GP points).
# -Monitor N opens the window on display N without taking the focus (DD_MONITOR), -Mute sets DD_MUTE (no audio device).
# -RunDir runs the game in another directory (it needs orig\ there, e.g. a junction, and gets its own
# DoubleDashRedux.log, shots\, saves\ and cache\), so two test runs can go at the same time.
# Test runs are windowed (DD_FULLSCREEN=0) unless DD_FULLSCREEN is set; settings.ini supplies everything else.
# Other DD_* variables already set in the environment are passed through.
param([int]$Seconds = 20, [switch]$NoBuild, [int]$Lines = 80, [string]$Script, [int]$Laps = -1, [switch]$HangExit,
      [switch]$Sym, [string]$Card, [switch]$NewCard, [switch]$Hz60, [string]$Autopilot, [int]$Monitor = 0, [switch]$Mute, [string]$RunDir)
Set-Location (Split-Path $PSScriptRoot -Parent)
if (-not $NoBuild) {
    powershell -ExecutionPolicy Bypass -File tools\build.ps1 | Out-Host
    if ($LASTEXITCODE) { exit 1 }
}
if ($Script) { $env:DD_SCRIPT = (Resolve-Path $Script).Path }
$exe = (Resolve-Path out\Release\DoubleDashRedux.exe).Path
if ($RunDir) { Set-Location $RunDir }
if ($Card) {
    if ($NewCard) { Remove-Item $Card -ErrorAction SilentlyContinue }
    $env:DD_MEMCARD_A = $Card
} elseif ($Script -and -not $env:DD_MEMCARD_A) { $env:DD_MEMCARD_A = "none" }
if ($Hz60) { $env:DD_60HZ = "1" } elseif ($Script -and -not $env:DD_60HZ) { $env:DD_60HZ = "0" }
if ($Laps -ge 0) { $env:DD_LAPS = "$Laps" }
if ($HangExit) { $env:DD_HANG_EXIT = "1" }
if ($Autopilot) { $env:DD_AUTOPILOT = $Autopilot }
if ($Monitor -gt 0) { $env:DD_MONITOR = "$Monitor" }
if ($Mute) { $env:DD_MUTE = "1" }
if (-not $env:DD_FULLSCREEN) { $env:DD_FULLSCREEN = "0" }   # test runs stay windowed whatever settings.ini says
if (-not $env:DD_LAUNCHER) { $env:DD_LAUNCHER = "0" }       # and skip the launcher (set DD_LAUNCHER=1 to see it)
Remove-Item DoubleDashRedux.log -ErrorAction SilentlyContinue
$p = Start-Process -FilePath $exe -PassThru -RedirectStandardError out\stderr.txt -RedirectStandardOutput out\stdout.txt
$null = $p.Handle   # keep the handle open so ExitCode is available afterwards
$p.WaitForExit($Seconds * 1000) | Out-Null
$code = 124
if (!$p.HasExited) { Stop-Process -Id $p.Id -Force; "== killed after $Seconds s" } else { $code = $p.ExitCode; "== exited with code $code" }
if (Test-Path DoubleDashRedux.log) {
    if ($Sym) { python tools\sym.py DoubleDashRedux.log | Select-Object -Last $Lines }
    else { Get-Content DoubleDashRedux.log -Tail $Lines }
}
exit $code
