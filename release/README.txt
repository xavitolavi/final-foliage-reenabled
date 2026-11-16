Final Foliage Reenabled  -  living foliage for FINAL FANTASY VII REBIRTH (PC)
============================================================================

WHAT IT DOES
  - Grass, flowers and bushes bend when characters walk through them.
  - Sword swings and spells push plants around.
  - Plants keep moving in the wind much farther away (80 m instead of a few metres).
  - Plants that never moved now sway gently.
  - Soft rustle sounds and little leaf bursts when you brush past plants.


INSTALL
  1. Close the game.
  2. Double-click  Install.bat
  3. Play.

  The installer finds the game by itself. If it asks, paste the game folder
  (the one that contains the "End" folder).


UNINSTALL
  Close the game and double-click  Uninstall.bat
  Everything goes back to how it was.


AFTER A GAME UPDATE
  The mod switches itself off (nothing breaks). Wait for a new version of the mod.
  If you used Steam's "Verify integrity of game files", just run Install.bat again.


CHANGE SETTINGS (optional)
  Open this file with Notepad:
      <game folder>\End\Binaries\Win64\FinalFoliage.ini
  Change a number and save. It applies right away, even with the game running.

  The most useful ones:
    volume=          how loud the rustle sounds are (0 to 1)
    sounds=0         turns the sounds off
    vfx=0            turns the leaf bursts off
    gain=            how much a type of plant bends (in each [class] section; 0 = not at all)

  To go back to the default settings, delete FinalFoliage.ini; the game creates a fresh one.


REQUIREMENTS
  - Steam PC version of the game (update of 28 October 2025).
  - Windows 10 or 11.


GOOD TO KNOW
  - Works together with ReShade, OptiScaler and texture mods.
  - Don't combine it with other mods that change ff7rebirth_.exe.
  - Plant shadows don't bend, only the plants.


SOMETHING WRONG?
  Look for FinalFoliage.log next to FinalFoliage.ini and send it along with your report.
  No log at all means the mod isn't installed: run Install.bat again.
