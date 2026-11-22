Final Foliage Reenabled 1.1  -  living foliage for FINAL FANTASY VII REBIRTH (PC)
================================================================================


WHAT'S NEW IN 1.1
  - Much faster. Each plant now checks first whether anything is near it, and only plants
    near a character, a sword swing or a spell run the full bending code. The rest skip it.
    In a wide grass field the mod's cost dropped by about 75%. Tested on an RTX 4060 Laptop
    GPU at 1920x1080 with frame generation on:
        unmodded game     68.8 FPS
        1.0               51.4 FPS
        1.1               63.2 FPS
  - Looks and plays exactly the same as 1.0.

  Updating from 1.0: replace End\Content\Paks\zz_FinalFoliage_P.pak with the one from this
  download. winmm.dll is unchanged (it still writes "v1.0" in its log; that's expected).
  Your FinalFoliage.ini settings are kept.

  Using POM Textures with its Final Foliage core? That core carries its own copy of the
  plant shaders, so update it to the version made for Final Foliage 1.1 as well.
  Otherwise you keep 1.0's speed.

WHAT IT DOES
  - Grass, flowers and bushes bend when characters walk through them.
  - Sword swings and spells push plants around.
  - Plants keep moving in the wind much farther away (80 m instead of a few metres).
  - Plants that never moved now sway gently.
  - Soft rustle sounds and little leaf bursts when you brush past plants.


INSTALL
  1. Close the game.
  2. Copy the "End" folder from this download into your game folder
     (the folder that already has an "End" folder). Say yes if Windows asks to merge folders.
  3. Play.

  That puts two files in place:
      End\Binaries\Win64\winmm.dll          (the mod)
      End\Content\Paks\zz_FinalFoliage_P.pak (the plant shaders)

  The game folder is usually:
      C:\Program Files (x86)\Steam\steamapps\common\FINAL FANTASY VII REBIRTH
  (in Steam: right-click the game > Manage > Browse local files)


UNINSTALL
  Delete these files:
      End\Binaries\Win64\winmm.dll
      End\Content\Paks\zz_FinalFoliage_P.pak
  (and FinalFoliage.ini / FinalFoliage.log in the Win64 folder if you want them gone too)


AFTER A GAME UPDATE
  If the update changes the game too much, the mod switches itself off and the game
  runs as normal. Wait for a new version of the mod.


CHANGE SETTINGS (optional)
  The first time you play, the mod creates this file:
      <game folder>\End\Binaries\Win64\FinalFoliage.ini
  Open it with Notepad, change a number and save. It applies right away, even with the game running.

  The most useful ones:
    volume=          how loud the rustle sounds are (0 to 1)
    sounds=0         turns the sounds off
    vfx=0            turns the leaf bursts off
    gain=            how much a type of plant bends (in each [class] section; 0 = not at all)
    game_wind_reach_m=   how far away plants keep moving in the wind (0 = no limit)

  To go back to the default settings, delete FinalFoliage.ini; the game creates a fresh one.


REQUIREMENTS
  - Steam PC version of the game (update of 28 October 2025).
  - Windows 10 or 11.


GOOD TO KNOW
  - Works together with ReShade, OptiScaler, ShaderInjector and texture mods.
  - The mod never changes the game's files; it is loaded by the game as winmm.dll.
  - If another mod also uses a file called winmm.dll, only one of them can be installed.
  - Plant shadows don't bend, only the plants.


SOMETHING WRONG?
  Look for FinalFoliage.log in End\Binaries\Win64 and send it along with your report.
  No log at all means the mod isn't installed: check that winmm.dll is in End\Binaries\Win64.
