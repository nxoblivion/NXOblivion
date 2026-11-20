# Oblivion for Nintendo Switch (ReXGlue recomp, homebrew) - install guide

This is an experimental homebrew port of the Xbox 360 recompilation of The Elder Scrolls IV: Oblivion.
It runs as a normal homebrew `.nro` under Atmosphere. **You must supply your own game files**; none are included.

**Status: verified only in the Eden emulator (logos, intro video, title screen, main menu, controller
navigation). It has never been run on real hardware.** Expect problems, and please send the log back
(see "If it crashes").

## Requirements

- A Switch running Atmosphere custom firmware with hbmenu (homebrew menu).
- About 7 GB free on the SD card (game files are about 5.9 GB). exFAT or FAT32 both work; the largest
  file is 1.3 GB, below the FAT32 4 GB limit.
- Your own Oblivion (Xbox 360) game files extracted from your disc: `default.xex`, `Oblivion.ini`,
  `data/`, `Data2/` (the repo's `assets/` folder: 166 files, about 5.9 GB).
- The port needs the **full application memory** (about 3 GB), so it must be started through title takeover
  (below). Applet mode (album / hbmenu opened normally) does not have enough memory.

## What goes where (SD card root)

```
sd:/switch/oblivion/oblivion.nro        the program (from this package)
sd:/oblivion/oblivion.toml              optional settings (from this package)
sd:/oblivion/assets/default.xex         YOUR game files, copied by you
sd:/oblivion/assets/Oblivion.ini
sd:/oblivion/assets/data/...
sd:/oblivion/assets/Data2/...
sd:/oblivion/log.txt                    created by the program (log, flushed on every line)
sd:/oblivion/cache, sd:/oblivion/user   created by the program (shader/save data)
```

1. Unzip `oblivion-switch.zip` onto the root of the SD card (it contains the `switch/` and `oblivion/`
   folders; merge with the existing ones).
2. Copy the game files so that `default.xex` ends up at `sd:/oblivion/assets/default.xex`.
   From a Windows PC with the repo checked out: `powershell -File ports\switch\copy-assets.ps1 E:`
   (E: = the SD card drive letter, or any folder acting as the card root). It uses robocopy, keeps the
   folder names exactly as they are (`data`, `Data2`, `Oblivion.ini`, `default.xex`) and can be re-run to
   resume. A copy of the script is also in this package; outside the repo pass the folder with your files, e.g.
   `.\copy-assets.ps1 E: -Source D:\oblivion_files`. Or copy the folder by hand in Explorer.
3. Eject the card safely and put it back in the Switch.

## Launching (title takeover)

1. On the home screen, **hold R** and start any installed game (not an application that is
   suspended, and not the Album). Keep holding R until the homebrew menu (hbmenu) appears.
2. Pick **Oblivion** in hbmenu and press A.

Why: opening hbmenu through the Album gives homebrew only about 400 MB (applet mode). Launched from a
game's slot ("title takeover") the program gets the full ~3 GB the recompiled game and its emulated 512 MB
of console memory require. If you start it in applet mode the port shows a message explaining this
and exits when you press Plus.

First launch is slow: the game loads, then every graphics shader is translated and compiled at the
moment it is first needed (the title screen took about 5 minutes to appear in the emulator; real
hardware may be faster or slower). The screen can stay black or frozen for long stretches. Please be patient,
and look at `sdmc:/oblivion/log.txt` (it keeps growing while the game is alive).

## Controls

| Switch | Game (Xbox 360 pad) |
|---|---|
| Plus | Start |
| Minus | Back |
| Switch B (bottom) | A |
| Switch A (right) | B |
| Switch Y (left) | X |
| Switch X (top) | Y |
| L / R | LB / RB |
| ZL / ZR | Left / Right trigger |
| Left / right stick, stick clicks | Left / right stick, L3 / R3 |
| D-pad | D-pad |

Face buttons follow the Xbox positions. To use Nintendo labels instead (Switch A = A, Switch B = B),
set `switch_button_layout = "nintendo"` in `sdmc:/oblivion/oblivion.toml`.
Player 1 is handheld mode, a Pro Controller or a Joy-Con pair. Rumble is not implemented.
There is no touch or motion input. The Home button belongs to the system as usual.

In Eden (PC emulator) the keyboard maps through Eden's own controller settings; the defaults are
Plus = M, Minus = N, Switch A = C, Switch B = X, Switch X = V, Switch Y = Z, left stick = WASD,
d-pad = arrow keys, L = Q, R = E, ZL = R, ZR = T, stick clicks = F / G (check Configure > Controls).

## If it crashes or looks wrong

Send back `sd:/oblivion/log.txt` (and, if you can, a photo of the screen). The log is flushed after every
line, so it survives a crash or a forced shutdown. The next start may rotate or overwrite it, so copy it
off the card before launching again.

## Known limits (honest list)

- **Untested on hardware.** Everything above was checked in the Eden emulator on Windows only. Real
  Switch GPU driver behaviour (Mesa nouveau), memory limits and speed are unknown. Eden had a few
  driver quirks that were worked around; real hardware may have different ones.
- Very slow start (shader compilation), and probably slow frame rates in game.
- **Audio** is implemented (libnx audout, stereo 48 kHz; the game's XMA music/effects are decoded on the CPU) but is UNTESTED (it compiles and initialises in Eden, but nobody has listened to it; never run on real hardware). Decoding costs CPU on top of an already slow port, so crackling or dropouts are possible. Optional debug setting: `switch_audio_dump = "sdmc:/oblivion/audio.wav"` writes everything sent to the speakers to a WAV file (grows ~11 MB per minute).
- Gameplay beyond the main menu does not work yet: in the emulator "New game" reaches the loading
  screen (progress bar nearly full) but then stalls and never enters the game. Saving, rumble and the unimplemented GPU features (tessellation, memexport, MSAA, some
  texture types) may cause visual errors.
- Settings from the in-game menu are not specially supported on Switch.
- Needs about 3 GB of application memory: do not run it with other homebrew in the background,
  and expect it to quit if the system reclaims memory.
- Sleep mode and docking while running are not handled specially.

## Future options (not provided)

- A home-screen forwarder (NSP) so you could launch without title takeover is not included: building one
  needs extra tools and the console's own keys, and it would still be limited by memory unless installed
  as a real application. It may be added later.

## Diagnostics (v0.4)
If the screen stays black: read `sdmc:/oblivion/boot.txt` (last line = last step reached) and `sdmc:/oblivion/crash.txt`. `switch/memprobe.nro` writes `memprobe.txt` with the memory layout. `usb_log.py` streams the same lines live over USB (see ports/switch/DEBUGGING.md section 6).
