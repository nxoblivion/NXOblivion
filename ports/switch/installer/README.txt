Oblivion for Nintendo Switch - installer (unofficial, homebrew)
================================================================

This folder builds the Switch version of The Elder Scrolls IV: Oblivion from YOUR OWN Xbox 360 copy.
No part of the game comes with it: the game code is translated and compiled on your PC, from your files,
and your files never leave your PC. Nothing needs to be installed; keep this folder together.

You need
- Your Oblivion Xbox 360 disc as a disc image (.iso) or as extracted files (default.xex, Oblivion.ini,
  data, Data2). Only the version this port was made for works; the installer checks it.
- A Switch with Atmosphere custom firmware, and about 7 GB free on its SD card.
- Windows 10/11 (64-bit). The first build takes a few minutes (it uses all CPU cores) and about 3 GB of disk.

Steps
1. Start OblivionNX-Installer.exe.
2. "Disc image..." (or "Folder...") and pick your copy. It is checked right away.
3. "Build oblivion.nro" (or go straight to Install, which builds first).
4. Choose where to install:
   - SD card or folder: pick the SD card's drive (or a folder, then copy its "switch" and "oblivion"
     folders to the root of the SD card).
   - Switch over USB: on the Switch open DBI, choose "Run MTP responder", connect the USB cable,
     press Refresh, pick "... > SD Card", Install.
   Install can be re-run: game files that are already there are skipped, your oblivion.toml settings are kept.

Starting it on the Switch
Hold R while starting any game from the HOME menu, keep holding until the Homebrew Menu appears, pick Oblivion.
(The Album gives homebrew too little memory.) The first start compiles graphics shaders and is slow.

What ends up on the SD card
  sd:/switch/oblivion/oblivion.nro    the program built on your PC
  sd:/oblivion/oblivion.toml          settings (only written if not there yet)
  sd:/oblivion/assets/...             your game files
  sd:/oblivion/log.txt                written by the game (send it with bug reports)

Files in this folder
  OblivionNX-Installer.exe   the installer
  kit/toolchain              LLVM clang and lld (Apache-2.0 with LLVM exception)
  kit/codegen                ReXGlue code generator (BSD-3-Clause)
  kit/lib, kit/include       the precompiled port runtime, libnx, Mesa, newlib/libstdc++ and their headers
  kit/licenses               licenses of all of the above
  work/                      created by the installer: build files (safe to delete)

This is an unofficial fan project, not affiliated with Bethesda, Microsoft or Nintendo.
