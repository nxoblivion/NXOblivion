NXOblivion for Nintendo Switch - installer (unofficial, homebrew)
===============================================================

This folder builds NXOblivion, a Switch port of The Elder Scrolls IV: Oblivion from YOUR OWN Xbox 360
copy, drawn by reNut's Direct3D-level renderer on deko3d, instead of an emulated Xbox 360 GPU.
No part of the game comes with it: the game code and the game's shaders are translated and compiled on your PC,
from your files, and your files never leave your PC. Nothing needs to be installed; keep this folder together.

You need
- Your Oblivion Xbox 360 disc as a disc image (.iso) or as extracted files (default.xex, Oblivion.ini,
  data, Data2). Only the version this port was made for works; the installer checks it.
- A Switch with Atmosphere custom firmware, and about 7 GB free on its SD card.
- Windows 10/11 (64-bit). The first build takes a few minutes (it uses all CPU cores) and about 3 GB of disk.

Steps
1. Start OblivionNX-Installer.exe.
2. "Disc image..." (or "Folder...") and pick your copy. It is checked right away.
3. "Build oblivion.nro" (or go straight to Install, which builds first). This also translates the game's shaders.
4. Choose where to install:
   - SD card or folder: pick the SD card's drive (or a folder, then copy its "switch" and "oblivion"
     folders to the root of the SD card).
   - Switch over USB: on the Switch open DBI, choose "Run MTP responder", connect the USB cable,
     press Refresh, pick "... > SD Card", Install.
   Install can be re-run: game files that are already there are skipped.

Starting it on the Switch
Hold R while starting any game from the HOME menu, keep holding until the Homebrew Menu appears, pick
"NXOblivion". (The Album gives homebrew too little memory.) Shaders the installer did not compile in advance
are compiled the first time they are needed; the console keeps them in deko3d_shaders.bin.

Settings (sd:/switch/reoblivion/oblivion.toml, a text file; a line added there overrides the built-in value)
  oblivion_renut_frame_cap = 30.0   the most frames a second (the Xbox 360 version runs at 30; 60.0 lifts the cap,
                                    the Switch seldom gets there)
  switch_clocks = true              CPU 1785 MHz, GPU 460.8 MHz handheld / 768 docked, memory 1600 MHz while the game
                                    runs (the system's own highest speeds; more battery use). false: the system's
                                    clocks, much slower
  switch_fps_overlay = true         the FPS counter (L+R+Up hides or shows it)
  switch_profile_stacks = false     true: also record where the CPU time goes (for bug reports about speed)
Every 10 s the game writes its speed to logs/oblivion_*.log (lines with "[oblivion] game thread") and to
logs/rex/rex_profile.log (each thread's CPU share, the console's clocks and temperature).

What ends up on the SD card
  sd:/switch/reoblivion/reoblivion.nro          the program built on your PC
  sd:/switch/reoblivion/deko3d_glsl.bin         the game's shaders, translated on your PC
  sd:/switch/reoblivion/deko3d_shaders.bin      the shaders compiled for the Switch GPU (the console adds to it)
  sd:/switch/reoblivion/oblivion.toml           settings (written by the game when missing)
  sd:/switch/reoblivion/logs/                   written by the game (send them with bug reports)
  sd:/oblivion/assets/...                       your game files (shared with the other Switch port)

Files in this folder
  OblivionNX-Installer.exe   the installer
  kit/toolchain              LLVM clang and lld (Apache-2.0 with LLVM exception)
  kit/codegen                ReXGlue code generator (katamari-nx's fork; BSD-3-Clause)
  kit/lib, kit/include       the precompiled port (GPL-3.0: ReXGlue runtime, reNut's renderer, plume, uam), deko3d,
                             libnx, newlib/libstdc++ and the headers the game code needs
  kit/shaders                the shader translator (XenosRecomp, MIT; Microsoft's DXC), SPIRV-Cross, uam, zstd,
                             the conversion scripts, the port's own shaders for the three Bink movie shaders the game
                             builds at run time, and lists of the pipelines the game uses (shader hashes and states)
  kit/python                 Python 3 for the scripts (PSF license)
  kit/licenses               licenses of all of the above
  work/                      created by the installer: build files (safe to delete)

Credits: reNut by masterspike52; katamari-nx by ChanseyIsTheBest; ReXGlue by Tom Clay, after Xenia.
The port is GPL-3.0; its source is the repository it comes from (nx/ in the Oblivion recompilation project).
This is an unofficial fan project, not affiliated with Bethesda, Microsoft or Nintendo.
