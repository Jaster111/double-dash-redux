Mario Kart: Double Dash!! - native Windows port
================================================

This is a static recompilation of the European release of Mario Kart: Double Dash!! (GM4P01) to native
Windows code. It contains no game data: you need your own disc image of the game.

Requirements
------------
- Windows 10 or 11, 64-bit, and a graphics card with Direct3D 11
- A disc image of Mario Kart: Double Dash!! (Europe, game ID GM4P01) as a plain .iso or .gcm file,
  for example one dumped from your own disc with Dolphin.
  Compressed images (.rvz, .wia, .gcz, .ciso) have to be converted first: in Dolphin, right-click the
  game, choose "Convert File...", set Format to ISO and click Convert.
  The North American (GM4E01) and Japanese (GM4J01) releases are not supported.

Getting started
---------------
1. Extract this folder anywhere you can write to (not into Program Files): settings, saves and the
   shader cache are kept next to doubledash.exe.
2. Put the disc image in the same folder, or start doubledash.exe and select it when asked. The choice
   is remembered in settings.ini.
3. Start doubledash.exe.

Controls
--------
Xbox-style (XInput) controllers, up to four players:
  A / B / X / Y         A / B / X / Y
  Left stick            control stick
  D-pad                 D-pad
  LT / RT               L / R (analog)
  LB or RB              Z
  Start or Back         Start
Keyboard (player 1):
  Arrow keys or WASD    control stick
  Space or X            A
  Z or Backspace        B
  C or Ctrl             X
  V                     Y
  Q                     L
  E or Shift            R
  F or Tab              Z
  Enter or Esc          Start
  I / K / J / L         D-pad
Window:
  Alt+Enter or F11      toggle borderless fullscreen
  Alt+F4                quit

Settings
--------
settings.ini is created with defaults and comments on the first start: window size, fullscreen,
internal resolution, 50/60 Hz, widescreen, volume, rumble, the memory card images and the disc image.
Changes take effect the next time the game starts.

Widescreen: set "widescreen = 1" in the [video] section for 16:9. It uses the "16:9 Widescreen v2"
code by Ralf and Kester (with Ralf's menu backgrounds add-on) from Dolphin's game settings: the races
show a wider view with the HUD at the screen edges, menus keep their proportions, and the opening movie
stays 4:3. Some menus show bits of off-screen graphics at the left and right edges, as they do in
Dolphin with the same code.

Saves
-----
The memory card in slot A is saves\MemoryCardA.raw (Dolphin .raw format, so a Dolphin memory card for
the European region can be copied here, and back).

Problems
--------
doubledash.log, next to the executable, records what happened during the last run; please include it
when reporting a problem.
